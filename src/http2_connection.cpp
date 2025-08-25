

#include "http2/http2_connection.hpp"

#include "http2/http2_send_frames.hpp"
#include "http2/logger.hpp"

#include <unordered_set>
#include <zal/zal.hpp>

#ifdef HTTP2_ENABLE_TRACE

namespace http2 {

void trace_request_headers(request_node const& node, bool fromclient) {
  auto& req = node.req;
  std::string s;
  if (fromclient) {
    s += std::format(":path: {}\n:authority: {}\n:method: {}\n:scheme: {}\n", req.path,
                     req.authority.empty() ? "<unset>" : req.authority, e2str(req.method), e2str(req.scheme));
  } else {
    s += std::format(":status: {}\n", node.status);
  }
  if (!req.body.contentType.empty()) {
    s += std::format("content-type: {}\n", req.body.contentType);
  }
  for (auto& h : req.headers) {
    s += std::format("name: {}, value: {}\n", h.name(), h.value());
  }
  HTTP2_LOG_TRACE("{}", s);
}

}  // namespace http2

#endif

namespace http2 {

void intrusive_ptr_add_ref(http2_connection* p) noexcept {
  ++p->refcount;
}

void intrusive_ptr_release(http2_connection* p) noexcept {
  --p->refcount;
  if (p->refcount == 0) {
    delete p;
  }
}

void intrusive_ptr_add_ref(request_node* p) noexcept {
  ++p->refcount;
}

void intrusive_ptr_release(request_node* p) noexcept {
  --p->refcount;
  if (p->refcount == 0) {
    p->connection->returnNode(p);
  }
}

static void validate_trailer_header(std::string_view name, stream_id_t streamid) {
  // https://www.rfc-editor.org/rfc/rfc7540#section-8.1.2.1
  // ( Pseudo-header fields MUST NOT appear in trailers )
  if (name.starts_with(':')) {
    throw protocol_error(
        errc_e::PROTOCOL_ERROR,
        std::format("trailers section must not include pseudo-headers, name: {}, streamid: {}", name,
                    streamid));
  }
}

void request_node::receiveTrailersHeaders(hpack::decoder& decoder, http2_frame_t frame) {
  // may handle both request trailers and response trailers
  HTTP2_LOG(TRACE, "received HEADERS (trailers): stream: {}, len: {}", frame.header.streamId,
            frame.header.length, name());
  constexpr auto mask = flags::END_STREAM | flags::END_HEADERS;
  if (((frame.header.flags & mask) != mask)) {
    throw protocol_error(errc_e::STREAM_CLOSED, "trailers header without END_STREAM | END_HEADERS");
  }
  on_scope_exit {
    end_stream_received = true;
  };
  hpack::decode_headers_block(decoder, frame.data, [&](std::string_view name, std::string_view value) {
    HTTP2_LOG(TRACE, "name: {}, value: {}", name, value, this->name());
    validate_trailer_header(name, frame.header.streamId);
    if (onHeader)
      (*onHeader)(name, value);
  });

  if (onDataPart) {
    // pass empty DATA chunk, so user will know, that data is ended
    // its required, because when trailers present there are no
    // DATA frame with END_STREAM flag
    (*onDataPart)({}, /*last chunk*/ true);
  }
}

void request_node::receiveRequestTrailers(hpack::decoder& decoder, http2_frame_t hdrs) {
  assert(hdrs.header.type == frame_e::HEADERS);
  auto old_on_header = onHeader;
  on_scope_exit {
    onHeader = old_on_header;
  };
  auto onheader = [&](std::string_view name, std::string_view value) {
    req.headers.push_back(http_header_t(std::string(name), std::string(value)));
  };
  // server does not set 'onHeader' / 'onDataPart' callbacks, but reuses this function for trailers
  onHeader = &onheader;
  receiveTrailersHeaders(decoder, hdrs);
}

void request_node::receiveResponseHeaders(hpack::decoder& decoder, http2_frame_t frame) {
  assert(frame.header.streamId == streamid);
  assert(frame.header.type == frame_e::HEADERS);
  HTTP2_LOG(TRACE, "received HEADERS: stream: {}, len: {}", frame.header.streamId, frame.header.length,
            name());
  // Note: this code decodes headers block or fails with protocol_error (ends connection)
  // thats why we dont care about `decode_headers_block` in fail branckes

  assert(frame.header.flags & flags::END_HEADERS);

  // 199 - last informational status. Informational responses are interim and cannot have trailer section
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1-4
  // Note: ignores END_STREAM flag for interim responses, not marks it as error
  if (status > 199) [[unlikely]] {
    return receiveTrailersHeaders(decoder, frame);
  }
  on_scope_exit {
    end_stream_received = frame.header.flags & flags::END_STREAM;
  };
  byte_t const* in = frame.data.data();
  byte_t const* e = in + frame.data.size();
  status = decoder.decode_response_status(in, e);
  // headers must be decoded to maintain HPACK dynamic table in correct state
  hpack::decode_headers_block(decoder, std::span(in, e), [&](std::string_view name, std::string_view value) {
    HTTP2_LOG(TRACE, "name: {}, value: {}", name, value, this->name());
    if (onHeader) {
      (*onHeader)(name, value);
    }
  });
}

void request_node::receiveResponseData(http2_frame_t frame) {
  assert(frame.header.streamId == streamid);
  assert(frame.header.type == frame_e::DATA);
  on_scope_exit {
    end_stream_received = frame.header.flags & flags::END_STREAM;
  };
  decrease_window_size(rlStreamlevelWindowSize, int32_t(frame.header.length));
  if (rlStreamlevelWindowSize < MAX_WINDOW_SIZE / 2 && !(frame.header.flags & flags::END_STREAM)) {
    update_window_to_max(rlStreamlevelWindowSize, streamid, connection).start_and_detach();
  }
  if (onDataPart) {
    (*onDataPart)(frame.data, (frame.header.flags & flags::END_STREAM));
  }
  HTTP2_LOG(TRACE, "received DATA: stream: {}, len: {}, DATA: {}", frame.header.streamId, frame.header.length,
            std::string_view((char const*)frame.data.data(), frame.data.size()), name());
}

void request_node::receiveRequestHeaders(hpack::decoder& decoder, http2_frame_t frame) {
  assert(frame.header.streamId == streamid);
  assert(frame.header.type == frame_e::HEADERS);
  assert(frame.header.flags & flags::END_HEADERS);
  assert(req.headers.empty());

  on_scope_exit {
    end_stream_received = frame.header.flags & flags::END_STREAM;
  };

  HTTP2_LOG(TRACE, "received HEADERS: stream: {}, len: {}", frame.header.streamId, frame.header.length,
            name());

  parse_http2_request_headers(decoder, frame.data, req, frame.header.streamId);
#ifdef HTTP2_ENABLE_TRACE
  trace_request_headers(*this, /*from client=*/true);
#endif
}

void request_node::receiveRequestData(http2_frame_t frame) {
  assert(frame.header.streamId == streamid);
  assert(frame.header.type == frame_e::DATA);

  on_scope_exit {
    end_stream_received = frame.header.flags & flags::END_STREAM;
  };

  HTTP2_LOG(TRACE, "received DATA: stream: {}, len: {}, DATA: {}", frame.header.streamId, frame.header.length,
            std::string_view((char const*)frame.data.data(), frame.data.size()), name());

  decrease_window_size(rlStreamlevelWindowSize, int32_t(frame.header.length));
  if (rlStreamlevelWindowSize < MAX_WINDOW_SIZE / 2 && !(frame.header.flags & flags::END_STREAM)) {
    update_window_to_max(rlStreamlevelWindowSize, streamid, connection).start_and_detach();
  }
  if (is_half_closed()) {
    throw stream_error(errc_e::STREAM_CLOSED, frame.header.streamId, "stream already assembled");
  }

  if (!is_input_streaming()) {
    req.body.data.insert(req.body.data.end(), frame.data.begin(), frame.data.end());
  } else {
    (*onDataPart)(frame.data, frame.header.flags& flags::END_STREAM);
  }
}

std::string_view request_node::name() const noexcept {
  return connection ? connection->name.str() : "<null>";
}

// http2_connection methods

http2_connection::http2_connection(any_connection_t&& c, boost::asio::io_context& ctx)
    : tcpCon(std::move(c)),
      buckets(initial_buckets_count),
      responses({buckets.data(), buckets.size()}),
      pingtimer(ctx),
      pingdeadlinetimer(ctx),
      timeoutWardenTimer(ctx),
      ioctx(ctx) {
}

http2_connection::~http2_connection() {
  freeNodes.clear_and_dispose([](request_node* node) { delete node; });
}

void http2_connection::settings_changed(http2_frame_t newsettings, bool remote_is_client) {
  if (newsettings.header.flags & flags::ACK) {
    validate_settings_ack_frame(newsettings.header);
    // только после подтверждения настроек я действительно могу перейти на свои настройки
    // ведь до этого клиент/сервер мог посылать запросы/ответы по старому размеру динамической таблицы
    decoder.dyntab.set_user_protocol_max_size(localSettings.headerTableSize);
    return;
  }
  settings_t before = remoteSettings;
  if (remote_is_client) {
    settings_frame::parse(newsettings.header, newsettings.data,
                          client_settings_visitor{remoteSettings,
                                                  /*firstframe=*/!first_settings_frame_received});
  } else {
    settings_frame::parse(newsettings.header, newsettings.data,
                          server_settings_visitor{remoteSettings,
                                                  /*firstframe=*/!first_settings_frame_received});
  }
  first_settings_frame_received = true;
  if (before.headerTableSize != remoteSettings.headerTableSize) {
    HTTP2_LOG(INFO, "HPACK table resized: new size {}, old size: {}", remoteSettings.headerTableSize,
              before.headerTableSize, name);
    encodertablesizechangerequested = true;
  }
  // encoder обновится на основании новых настроек когда писатель увидит `encodertablesizechangerequested`
  adjustWindowForAllStreams(before.initialStreamWindowSize, remoteSettings.initialStreamWindowSize);
  // then change all active streams window size
  send_settings_ack(this).start_and_detach();
}

void http2_connection::serverSettingsChanged(http2_frame_t newsettings) {
  settings_changed(newsettings, /*remote_is_client=*/false);
}

void http2_connection::serverRequestsGracefulShutdown(goaway_frame f) {
  HTTP2_LOG(TRACE, "graceful shutdown initiated: last stream id: {}", f.lastStreamId, name);
  // if we did not initiate this graceful shutdown
  if (!gracefulshutdownGoawaySended) {
    initiateGracefulShutdown(f.lastStreamId);
    gracefulshutdownGoawaySended = true;
  }
  // do not drop connection, coStop will do it or reader (its out of streams)
}

void http2_connection::initiateGracefulShutdown(stream_id_t laststreamid) noexcept {
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8-3
  // when GOAWAY with NO_ERROR received interpret it as shutdown initiation.
  // Receivers of a GOAWAY frame MUST NOT open additional streams on the
  // connection this state similar to state when connection is out of streams.
  // So, connection will work until all done and create new connection for new
  // streams. we will do all what requested, > last stream id will be ignored, <
  // last stream id will be handled
  laststartedstreamid = MAX_STREAM_ID;
  for (auto b = responses.begin(); b != responses.end();) {
    auto n = std::next(b);
    if (b->streamid > laststreamid) {
      finishRequest(*b, reqerr_e::SERVER_CANCELLED_REQUEST);
    }
    b = n;
  }
  requests.clear_and_dispose([&](request_node* r) { finishRequest(*r, reqerr_e::SERVER_CANCELLED_REQUEST); });
}

void http2_connection::forget(request_node& node) noexcept {
  if (node.requestsHook.is_linked()) {
    erase_byref(requests, node);
  }
  if (node.responsesHook.is_linked()) {
    assert(responses.count(node.streamid) == 1);
    erase_byref(responses, node);
  }
  if (node.timersHook.is_linked()) {
    erase_byref(timers, node);
  }
}

void http2_connection::finishRequest(request_node& node, int status) noexcept {
  forget(node);
  if (!node.task) {
    return;
  }
  if (status <= 0) {
    HTTP2_LOG(TRACE, "stream {} finished, status: {}", node.streamid, e2str(reqerr_e::values_e(status)),
              name);
  } else {
    HTTP2_LOG(TRACE, "stream {} finished, status: {}", node.streamid, status, name);
  }
  node.status = status;
  node_ptr p = &node;  // hold node
  auto t = std::exchange(node.task, nullptr);
  if (status == reqerr_e::CANCELLED || status == reqerr_e::TIMEOUT) {
    // ignore possible bad alloc for coroutine
    send_rst_stream(this, node.streamid, errc_e::CANCEL).start_and_detach();
  }
  t.resume();
}

void http2_connection::finishRequestWithUserException(request_node& node, std::exception_ptr e) noexcept {
  forget(node);
  if (!node.task) {
    return;
  }
  HTTP2_LOG(TRACE, "stream {} finished with user exception", node.streamid, name);
  send_rst_stream(this, node.streamid, errc_e::CANCEL).start_and_detach();
  node.task.promise().exception = std::move(e);
  // Note: избегаем выставления одновременно и результата и исключения,
  // поэтому не будим напрямую .task (она выставит результат из .status), вместо
  // этого будим того кто её ждёт
  node.task.promise().who_waits.resume();
}

bool http2_connection::finishStreamWithError(rst_stream rstframe) {
  validateRstFrame(rstframe);
  auto* node = findResponseByStreamid(rstframe.header.streamId);
  if (!node) {
    return false;
  }
  finishRequest(*node, reqerr_e::SERVER_CANCELLED_REQUEST);
  return true;
}

void http2_connection::finishAllWithReason(reqerr_e::values_e reason) {
  assert(isDropped());  // must be called only while dropConnection()

  // assume only i have access to it
  auto reqs = std::move(requests);
  auto rsps = std::move(responses);
  // >= because request may not be inserted in 'timers' if deadline == never
  assert(reqs.size() + rsps.size() >= timers.size());
  // nodes in reqs or in rsps, timers do not own them
  timers.clear();
  if (!reqs.empty() || !rsps.empty()) {
    HTTP2_LOG(TRACE, "finish {} requests and {} responses, reason code: {}", reqs.size(), rsps.size(),
              e2str(reason), name);
  }
  auto forgetAndResume = [&](request_node* node) { finishRequest(*node, reason); };
  reqs.clear_and_dispose(forgetAndResume);
  rsps.clear_and_dispose(forgetAndResume);
}

[[nodiscard]] request_node* http2_connection::findResponseByStreamid(stream_id_t id) noexcept {
  auto it = responses.find(id);
  return it != responses.end() ? &*it : nullptr;
}

void http2_connection::dropTimeouted() {
  // prevent destruction of *this while resuming
  http2_connection_ptr_t lock = this;
  while (!timers.empty() && timers.top()->deadline.isReached()) {
    // node deleted from timers by forgetting
    finishRequestByTimeout(*timers.top());
  }
}

void http2_connection::windowUpdate(window_update_frame frame) {
  HTTP2_LOG(TRACE, "received window update, stream: {}, inc: {}", frame.header.streamId,
            frame.windowSizeIncrement, name);
  if (frame.header.streamId == 0) {
    increment_window_size(receiverWindowSize, int32_t(frame.windowSizeIncrement), 0);
    return;
  }
  request_node* node = findResponseByStreamid(frame.header.streamId);
  if (!node) {
    HTTP2_LOG(WARN, "received window update for stream which not exist, streamid: {}", frame.header.streamId,
              name);
    if (is_idle_stream(frame.header.streamId)) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("WINDOW_UPDATE for idle frame, streamid: {}", frame.header.streamId));
    }
    return;
  }
  increment_window_size(node->lrStreamlevelWindowSize, int32_t(frame.windowSizeIncrement),
                        frame.header.streamId);
}

bool http2_connection::prepareToShutdown(reqerr_e::values_e reason) noexcept {
  if (isDropped()) {
    return false;
  }

  HTTP2_LOG(TRACE, "shutdown", name);

  // set flag for anyone who will be resumed while shutting down this connection
  startDrop();

  // prevents me to be destroyed while resuming writer/reader etc
  http2_connection_ptr_t lock = this;

  pingtimer.cancel();
  pingtimer.set_callback({});  // delete prev callback and shared ptr to connection in it
  pingdeadlinetimer.cancel();
  pingdeadlinetimer.set_callback({});
  timeoutWardenTimer.cancel();
  timeoutWardenTimer.set_callback({});

  // firstly stop handling new data on connection
  if (writer.handle) {
    writer.handle.destroy();
    writer.handle = nullptr;
  } else {
    // writer not waits for work, but suspended (because we are in single thread
    // and working now)
    // == its in write/sleep
    // then writer must be canceled by socket.cancel() or shutdown
  }
  finishAllWithReason(reason);
  return true;
}

void http2_connection::shutdown(reqerr_e::values_e reason) noexcept {
  if (!prepareToShutdown(reason)) {
    return;
  }
  tcpCon->shutdown();
}

node_ptr http2_connection::newRequestNode(http_request&& request, deadline_t deadline,
                                          on_header_fn_ptr onHeader, on_data_part_fn_ptr onDataPart,
                                          stream_id_t id) {
  node_ptr node;
  if (freeNodes.empty()) {
    node = new request_node;
  } else {
    node = &freeNodes.front();
    freeNodes.pop_front();
  }
  node->lrStreamlevelWindowSize = remoteSettings.initialStreamWindowSize;
  node->rlStreamlevelWindowSize = localSettings.initialStreamWindowSize;
  node->req = std::move(request);
  node->streamid = id;
  node->deadline = deadline;
  node->task = nullptr;
  node->connection = this;
  node->onHeader = onHeader;
  node->onDataPart = onDataPart;
  node->status = reqerr_e::UNKNOWN_ERR;
  node->canceledByRstStream = false;
  node->responded = false;
  node->answered_before_data = false;
  node->end_stream_received = false;

  assert(node->refcount == 1);
  assert(!node->requestsHook.is_linked());
  assert(!node->responsesHook.is_linked());
  assert(!node->timersHook.is_linked());
  assert(!node->is_output_streaming());
  return node;
}

node_ptr http2_connection::newStreamingRequestNode(http_request&& request, deadline_t deadline,
                                                   on_header_fn_ptr onHeader, on_data_part_fn_ptr onDataPart,
                                                   stream_id_t streamid, stream_body_maker_t makebody) {
  node_ptr node = newRequestNode(std::move(request), deadline, onHeader, onDataPart, streamid);
  node->makebody = std::move(makebody);
  return node;
}

void http2_connection::returnNode(request_node* ptr) noexcept {
  assert(ptr && ptr->connection);
  forget(*ptr);
  ptr->connection->mark_stream_closed(ptr->streamid);
  ptr->req = {};
  ptr->makebody.reset();
  // using always server settings, client creates requests, server controls
  if (freeNodes.size() >= serverSettings->maxConcurrentStreams) {
    delete ptr;
    return;
  }
  freeNodes.push_front(*ptr);
  // it may be last pointer to *this
  ptr->connection = nullptr;
}

http2_connection::response_awaiter http2_connection::responseReceived(request_node& node) noexcept {
  assert(!node.timersHook.is_linked());
  assert(!node.requestsHook.is_linked());
  assert(!node.responsesHook.is_linked());
  requests.push_back(node);
  // highly likely, that new value will be at end,
  // because new deadline will be greater then previous
  if (node.deadline != deadline_t::never()) {
    bool reschedule = timers.empty() || node.deadline < timers.top()->deadline;
    timers.insert(timers.end(), node);
    if (reschedule) {
      timeoutWardenTimer.rearm(timers.top()->deadline.tp);
    }
  }
  return response_awaiter{this, &node};
}

void http2_connection::ignoreFrame(http2_frame_t frame) {
  HTTP2_LOG(TRACE, "ignoring frame, type: {}, stream: {}. len: {}", e2str(frame.header.type),
            frame.header.streamId, frame.header.length, name);
  using enum frame_e;
  // here we assume, that there are no node with frame stream id (thats why it is ignored)

  // Note: sending frame for closed stream is only stream error, not protocol
  // because its possible that stream was canceled due timeout and then frame received
  switch (frame.header.type) {
    case HEADERS:
      // even if we ignoring frame, stream is done
      laststartedstreamid = std::max(laststartedstreamid, frame.header.streamId);
      // decode before all to ensure decoder will be in correct state
      // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8-19
      // maintain hpack dynamic table
      hpack::decode_headers_block(decoder, frame.data, [&](std::string_view, std::string_view) {});

      if (is_closed_stream(frame.header.streamId)) {
        throw stream_error(errc_e::STREAM_CLOSED, frame.header.streamId,
                           "HEADERS frame sent for closed stream");
      }
      mark_stream_closed(frame.header.streamId);
      return;
    case DATA:
      // NOTE: not using data.size(), since padding should be counted as received
      // octets
      // ('data' does not contain padding)
      decrease_window_size(myWindowSize, int32_t(frame.header.length));
      if (is_closed_stream(frame.header.streamId)) {
        throw stream_error(errc_e::STREAM_CLOSED, frame.header.streamId, "DATA frame sent for closed stream");
      }
      if (is_idle_stream(frame.header.streamId)) {
        throw protocol_error(
            errc_e::PROTOCOL_ERROR,
            std::format("DATA frame sent for idle stream, streamid {}", frame.header.streamId));
      }
      return;
    default:
      return;
  }
}

void http2_connection::adjustWindowForAllStreams(cfint_t old_window_size, cfint_t new_window_size) {
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.2-3
  //  When the value of SETTINGS_INITIAL_WINDOW_SIZE changes, a receiver MUST adjust the size of all stream
  //  flow-control windows that it maintains by the difference between the new value and the old value.
  if (old_window_size == new_window_size)
    return;
  // make sure every stream handled once
  std::unordered_set<request_node*> handled;
  cfint_t increment = new_window_size - old_window_size;
  if (std::abs(increment) > std::numeric_limits<int32_t>::max()) {
    throw protocol_error(
        errc_e::FLOW_CONTROL_ERROR,
        std::format("SETTINGS_INITIAL_WINDOW_SIZE leads to control flow overflow, old: {}, new: {}",
                    old_window_size, new_window_size));
  }

  auto adjust_stream = [&](request_node& x) {
    if (handled.contains(&x))
      return;
    try {
      increment_window_size(x.lrStreamlevelWindowSize, increment, x.streamid);
    } catch (stream_error const& e) {
      // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.2-7
      // "An endpoint MUST treat a change to SETTINGS_INITIAL_WINDOW_SIZE that causes any flow-control window
      // to exceed the maximum size as a connection error (Section 5.4.1) of type FLOW_CONTROL_ERROR"
      throw protocol_error(errc_e::FLOW_CONTROL_ERROR, e.what());
    }
    handled.insert(&x);
  };

  for (request_node& x : requests)
    adjust_stream(x);
  for (request_node& x : responses)
    adjust_stream(x);
}

dd::task<void> http2_connection::receive_headers_with_continuation(
    http2_frame_t frame, io_error_code& ec, move_only_fn<void()> oneachframe,
    move_only_fn<void(http2_frame_t)> whendone) {
  assert(frame.header.type == frame_e::HEADERS);
  assert(!(frame.header.flags & flags::END_HEADERS));
  assert(oneachframe && whendone);
  bytes_t bytes(frame.data.begin(), frame.data.end());
  // TODO потоковое получение здесь?
  frame.validate_streamid();
  frame.removePadding();
  frame.ignoreDeprecatedPriority();
  frame_header startheader = frame.header;

  on_scope_failure(decode_anyway) {
    // maintain dyntab
    try {
      hpack::decode_headers_block(decoder, bytes, [](std::string_view, std::string_view) {});
    } catch (std::exception& e) {
      // may be part of data only received, error expectable
      HTTP2_LOG(WARN, "error while decoding CONTINUATIONS: {}", e.what(), name);
    }
  };
  byte_t hdr[FRAME_HEADER_LEN];
  for (;;) {
    frame.data = hdr;

    co_await read(hdr, ec);

    if (ec || isDropped())
      co_return;

    // parse frame header

    frame.header = frame_header::parse(hdr);
    frame.validateHeader();
    validate_frame_max_size(frame.header);
    oneachframe();

    if (frame.header.type != frame_e::CONTINUATION || frame.header.streamId != startheader.streamId) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("expected CONTINUATION for stream {}, got {}", startheader.streamId,
                                       e2str(frame.header.type)));
    }
    if (frame.header.length + bytes.size() > MAX_CONTINUATION_LEN) {
      throw stream_error(errc_e::REFUSED_STREAM, frame.header.streamId,
                         std::format("CONTINUATION too big, limit: {} bytes", MAX_CONTINUATION_LEN));
    }
    // read frame data
    bytes.resize(bytes.size() + frame.header.length);
    co_await read(suffix(std::span(bytes), frame.header.length), ec);
    if (ec || isDropped())
      co_return;
    if (frame.header.flags & flags::END_HEADERS) {
      // будто пришёл просто огромный HEADERS
      static_assert(std::numeric_limits<uint32_t>::max() > MAX_CONTINUATION_LEN);
      frame.data = bytes;
      frame.header = startheader;
      frame.header.flags |= flags::END_HEADERS;
      frame.header.length = bytes.size();
      decode_anyway.no_longer_needed();
      whendone(frame);
      co_return;
    }
  }
  unreachable();
}

void http2_connection::client_receive_headers(http2_frame_t frame) {
  assert(frame.header.type == frame_e::HEADERS);
  frame.validate_streamid();
  frame.removePadding();
  frame.ignoreDeprecatedPriority();

  node_ptr node = findResponseByStreamid(frame.header.streamId);
  if (!node) {
    ignoreFrame(frame);
    return;
  }

  try {
    // sets end_stream_received flag
    node->receiveResponseHeaders(decoder, frame);
  } catch (hpack::protocol_error&) {
    throw;
  } catch (protocol_error&) {
    throw;
  } catch (...) {
    // user-handling exception, do not drop connection
    finishRequestWithUserException(*node, std::current_exception());
    return;
  }
  // ignore interim responses
  if (node->is_connect_request() && !(node->status > 99 && node->status < 200)) [[unlikely]] {
    if (node->end_stream_received) {
      return finishRequest(*node, reqerr_e::SERVER_CANCELLED_REQUEST);
    }
    assert(node->task);
    std::exchange(node->task, nullptr).resume();
    return;
  }
  if (node->end_stream_received) {
    finishRequest(*node, node->status);
  }
}

void http2_connection::client_receive_data(http2_frame_t frame) {
  assert(frame.header.type == frame_e::DATA);

  frame.validate_streamid();
  frame.removePadding();
  node_ptr node = findResponseByStreamid(frame.header.streamId);
  if (!node) {
    ignoreFrame(frame);
    return;
  }
  // applicable only to data
  // Note: includes padding!
  decrease_window_size(myWindowSize, frame.header.length);
  try {
    node->receiveResponseData(frame);
  } catch (hpack::protocol_error&) {
    throw;
  } catch (protocol_error&) {
    throw;
  } catch (...) {
    // user-handling exception, do not drop connection
    finishRequestWithUserException(*node, std::current_exception());
    return;
  }
  if (node->end_stream_received) {  // setted in receiveResponseData
    finishRequest(*node, node->status);
  }
}

}  // namespace http2
