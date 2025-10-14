

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"

#include "http2/http2_client.hpp"

#include "http2/asio/asio_executor.hpp"
#include "http2/http2_connection.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http2_errors.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/http2_writer.hpp"
#include "http2/logger.hpp"
#include "http2/utils/macro.hpp"
#include "http2/utils/reusable_buffer.hpp"
#include "http2/utils/timer.hpp"
#include "http2/asio/awaiters.hpp"

#include <hpack/hpack.hpp>
#include <zal/zal.hpp>

/*

Путь каждого запроса в http2_client:

1. корутина send_request захватывает соединение через borrowConnection, при этом
может потребоваться вызвать startConnecting, на время соединения send_request
попадаёт в client.m_connectionWaiters. Note: При ошибке создания соединения,
запросы в m_connectionWaiters будут отменёны. В будущем эту политику можно
заменить на ещё одну попытку соединения и отмену только тех запросов, что по
таймауту уже не успевают
2. После захвата соединения sendRequst создаёт стрим, инициализирует его
streamid и засыпает на responseReceived, который:
    * инициализирует stream.task хендлом корутины send_request
    * складывает стрим в connection.responses (для отправки писателем) и
connection.timers (для проверки на таймаут)
3. писатель берёт из очереди стрим, перекладывает его в connection.responses и
начинает писать серверу. Важно переложить в responses заранее, чтобы не потерять
никаких фреймов для этого стрима
4. читатель начинает получать фреймы для стрима, вызывая on_header /
on_data_part при чтении HEADERS/DATA фреймов соответственно. При исключении из
пользовательских калбеков читатель вызывает finishRequestWithUserException

5. При получении фрейма с флагом END_STREAM читатель вызывает finishRequest,
который корректно забывает (forget) стрим из всех контейнеров и делает
task.resume, будя корутину send_request

    Note: фреймом с END_STREAM может быть в том числе trailer HEADERS после DATA

На любом этапе может прийти фрейм RST_STREAM/Goaway или вызваться graceful_stop, это
также приведёт к finishRequest
*/

namespace http2 {

void http2_client::notifyConnectionWaiters(h2connection_ptr result) noexcept {
  // assume only i have access to waiters
  auto waiters = std::move(m_connectionWaiters);
  HTTP2_LOG(TRACE, "resuming connection waiters, count: {}", waiters.size(),
            result ? result->name.str() : "<null>");
  assert(m_connectionWaiters.empty());  // check boost really works as expected
  waiters.clear_and_dispose([&](noexport::waiter_of_connection* w) {
    assert(!w->result);
    w->result = result;
    assert(w->task);
    // ordering matters for getting correct stream id
    w->task.resume();
  });
}

// periodically invoked in connection.pingtimer
struct ping_callback {
  h2connection_ptr con = nullptr;
  stream_id_t lastid = 0;
  duration_t pingtimeout;
  http2_client* client = nullptr;

  // precondition: c != nullptr, client != nullptr
  explicit ping_callback(h2connection_ptr c, http2_client* clientptr, duration_t pingTimeout) {
    assert(c);
    assert(clientptr);
    con = std::move(c);
    client = std::move(clientptr);
    lastid = con->laststartedstreamid;
    pingtimeout = pingTimeout;
  }

  void operator()() {
    // send ping only if nothing happens since last iteration
    if (lastid != con->laststartedstreamid) {
      lastid = con->laststartedstreamid;
      return;
    }
    if (!con->pingdeadlinetimer.armed()) {
      con->pingdeadlinetimer.arm(pingtimeout);
    }
    // assume will be ended before client dies (io_ctx)
    send_ping(con, PING_VALUE, /*requestPong=*/true).start_and_detach();
  }
};

dd::job http2_client::startConnecting(http2_client* self, deadline_t deadline) {
  assert(self);
  co_await std::suspend_always{};  // resumed when needed by creator

  if (self->m_connection || !self->m_connectionGate.try_enter()) {
    self->notifyConnectionWaiters(self->m_connection);
    co_return;
  }
  on_scope_exit {
    self->m_connectionGate.leave();
  };
  if (self->stopRequested()) {
    HTTP2_LOG(TRACE, "connection tries to create when stop requested, ignored", self->name());
    self->notifyConnectionWaiters(nullptr);
    co_return;
  }
  if (self->alreadyConnecting()) {
    // connection awaiters will be awakened by connection when ends
    co_return;
  }
  h2connection_ptr newConnection = nullptr;

  try {
    // note: connection unlocked before notify, avoiding this:
    // * first request drops connection,
    // * starting new connection
    // * ignore new connection (connections locked)
    // * no new requests, all stopped.
    {
      auto lock = self->lockConnections();
      HTTP2_LOG(TRACE, "creating connection", self->name());
      on_scope_exit {
        lock.release();
        // assume all connection waiters will observe new connection.
        // Note: writer/reader not yet created, but it should not be problem
        assert(!self->m_connection);
        self->m_connection = newConnection;
        self->notifyConnectionWaiters(newConnection);
      };
      any_connection_t tcpCon = co_await self->m_factory->createConnection(self->get_host(), deadline);
      h2connection_ptr con = new h2connection(std::move(tcpCon), self->ioctx());
      timer_t timer(self->ioctx());
      timer.arm(deadline.tp);
      timer.set_callback([con] { con->shutdown(reqerr_e::TIMEOUT); });
      assert(!self->m_notYetReadyConnection);
      self->m_notYetReadyConnection = con;
      on_scope_exit {
        self->m_notYetReadyConnection = nullptr;
      };
      newConnection = co_await establish_http2_session_client(std::move(con), self->m_options);
      timer.cancel();
    }
    assert(newConnection);

    // this gate closed only in graceful_stop and only after all startConnecting already done
    assert(!self->m_connectionPartsGate.is_closed());

    startReaderFor(self, newConnection);
    self->m_connection->writer.handle = nullptr;
    // writer itself sets writer handle in connection
    auto sleepcb = [self](duration_t d, io_error_code& ec) { return self->sleep(d, ec); };
    auto onnetworkerr = [self] { self->drop_connection(reqerr_e::NETWORK_ERR); };
    start_writer_for_client(newConnection, std::move(sleepcb), std::move(onnetworkerr),
                            self->get_options().forceDisableHpack, self->m_connectionPartsGate.hold());

    if (self->m_options.pingInterval != duration_t::max()) {
      newConnection->pingtimer.arm_periodic(self->m_options.pingInterval);
      newConnection->pingtimer.set_callback(ping_callback(newConnection, self, self->m_options.pingTimeout));
      // armed when ping sended, canceled when ping received
      newConnection->pingdeadlinetimer.set_callback([self] { self->drop_connection(reqerr_e::TIMEOUT); });
    }
    // newConnection->timeoutWardenTimer will be armed when requests will be added
    newConnection->timeoutWardenTimer.set_callback([newConnection] {
      newConnection->dropTimeouted();
      if (!newConnection->timers.empty()) {
        newConnection->timeoutWardenTimer.arm(newConnection->timers.top()->deadline.tp);
      }
    });
  } catch (std::exception& e) {
    HTTP2_LOG(ERROR, "exception while trying to connect: {}", e.what(), self->name());
    // if we created connection, but failed when starting reader / writer - drop
    self->drop_connection(reqerr_e::UNKNOWN_ERR);
  }
}

// handles only utility frames (not DATA / HEADERS)
static void handle_utility_frame(http2_frame_t frame, h2connection& con) {
  using enum frame_e;

  switch (frame.header.type) {
    case HEADERS:
    case DATA:
      unreachable();
    case SETTINGS:
      con.serverSettingsChanged(frame);
      return;
    case PING:
      handle_ping(ping_frame::parse(frame.header, frame.data), &con).start_and_detach();
      return;
    case RST_STREAM:
      if (!con.rstStreamClient(rst_stream::parse(frame.header, frame.data))) {
        HTTP2_LOG(INFO, "server finished stream (id: {}) which is not exists (maybe timeout or canceled)",
                  frame.header.streamId, con.name);
      }
      return;
    case GOAWAY: {
      goaway_frame f = goaway_frame::parse(frame.header, frame.data);
      if (f.errorCode != errc_e::NO_ERROR) {
        throw goaway_exception(f.lastStreamId, f.errorCode, std::move(f.debugInfo));
      } else {
        con.serverRequestsGracefulShutdown(f);
        return;
      }
    }
    case WINDOW_UPDATE:
      con.windowUpdate(window_update_frame::parse(frame.header, frame.data));
      return;
    case PUSH_PROMISE:
      // https://datatracker.ietf.org/doc/html/rfc9113#section-6.6-9
      assert(!con.localSettings.enablePush);  // always setted to 0
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           "PUSH_PROMISE must not be sent, SETTINGS_ENABLE_PUSH is 0");
    case CONTINUATION:
      // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.10-8
      throw protocol_error(
          errc_e::PROTOCOL_ERROR,
          "CONTINUATION frame received without a preceding HEADERS without END_HEADERS flag");
    case PRIORITY:
      con.validatePriorityFrameHeader(frame);
      [[fallthrough]];
    case PRIORITY_UPDATE:
    default:
      // ignore
      return;
  }
}

// writer works on node with reader (window_update / rst_stream possible)
// also node may be cancelled or destroyed, so writer and reader must never
// cache node between co_awaits
dd::job http2_client::startReaderFor(http2_client* self, h2connection_ptr c) {
  using enum frame_e;
  assert(self && c);
  assert(!self->m_connectionPartsGate.is_closed());

  HTTP2_LOG(TRACE, "reader started {}", self->name(), c->name);

  on_scope_exit {
    HTTP2_LOG(TRACE, "reader ended {}", self->name(), c->name);
  };
  auto guard = self->m_connectionPartsGate.hold();

  h2connection& con = *c;
  io_error_code ec;
  reusable_buffer buffer;
  http2_frame_t frame;
  reqerr_e::values_e reason = reqerr_e::UNKNOWN_ERR;
  protocol_error goaway_info;

  try {
    while (!con.isDoneCompletely()) {
      if (con.isDropped()) {
        goto connection_dropped;
      }

      // read frame header

      frame.data = buffer.getExactly(FRAME_HEADER_LEN);

      co_await con.read(frame.data, ec);

      if (ec) {
        goto network_error;
      }
      if (con.isDropped()) {
        goto connection_dropped;
      }

      // parse frame header

      frame.header = frame_header::parse(frame.data);
      frame.validateHeader();
      con.validate_frame_max_size(frame.header);
      // read frame data

      frame.data = buffer.getExactly(frame.header.length);
      co_await con.read(frame.data, ec);
      if (ec) {
        goto network_error;
      }
      if (con.isDropped()) {
        goto connection_dropped;
      }

      // handle frame

      try {
        switch (frame.header.type) {
          case HEADERS:
            if (frame.header.flags & flags::END_HEADERS) [[likely]] {
              con.client_receive_headers(frame);
            } else {
              co_await con.receive_headers_with_continuation(
                  frame, ec, [] {}, [&](http2_frame_t frame) { con.client_receive_headers(frame); });
              if (ec) {
                goto network_error;
              }
              if (con.isDropped()) {
                goto connection_dropped;
              }
            }
            break;
          case DATA:
            con.client_receive_data(frame);
            break;
          default:
            handle_utility_frame(frame, con);
            break;
        }
      } catch (stream_error& _e) {
        // workaround windows ABI https://github.com/llvm/llvm-project/issues/153949
        auto& e = _e;
        // reuse finish request with 'user' exception to make sure user will know about error
        h2stream* node = con.findResponseByStreamid(e.streamid);
        if (node) {
          con.finishRequestWithUserException(*node, std::current_exception());
        } else {
          // may be request ended by timeout
          send_rst_stream(&con, e.streamid, e.errc).start_and_detach();
        }
        // do not require connection close
      }

      // connection control flow (streamlevel in handle_frame)
      if (con.myWindowSize < MAX_WINDOW_SIZE / 2) {
        co_await update_window_to_max(con.myWindowSize, 0, c);
      }
    }
  } catch (hpack::protocol_error& e) {
    HTTP2_LOG(ERROR, "exception while decoding headers block (HPACK), err: {}", e.what(), con.name);
    goaway_info.errc = errc_e::COMPRESSION_ERROR;
    goaway_info.dbginfo = e.what();
    goto protocol_error;
  } catch (protocol_error& e) {
    HTTP2_LOG(INFO, "exception: {}", e.what(), c->name);
    goaway_info = std::move(e);
    goto protocol_error;
  } catch (goaway_exception& gae) {
    HTTP2_LOG(ERROR, "goaway received, {}", gae.what(), c->name);
    reason = reqerr_e::SERVER_CANCELLED_REQUEST;
    goto dropConnection;
  } catch (std::exception& se) {
    HTTP2_LOG(INFO, "unexpected exception {}", se.what(), c->name);
    reason = reqerr_e::UNKNOWN_ERR;
    goto dropConnection;
  } catch (...) {
    HTTP2_LOG(INFO, "unknown exception happens", c->name);
    reason = reqerr_e::UNKNOWN_ERR;
    goto dropConnection;
  }

  assert(con.isDoneCompletely());
  // must not resume anyone with 'done', because no pending requests (completely done)
  reason = reqerr_e::DONE;
  goto dropConnection;
protocol_error:
  reason = reqerr_e::PROTOCOL_ERR;
  // To avoid ambiguity just send max_stream_id every time. Server did not
  // initiate streams (SERVER_PUSH disabled) and nghttp2 for example answers
  // with protocol error for some values
  //
  // Rfc:
  // If a connection terminates without a GOAWAY frame,
  // the last stream identifier is effectively the highest possible stream
  // identifier
  send_goaway(&con, MAX_STREAM_ID, goaway_info.errc, std::move(goaway_info.dbginfo)).start_and_detach();
  goto dropConnection;
network_error:
  reason = ec == boost::asio::error::operation_aborted ? reqerr_e::CANCELLED : reqerr_e::NETWORK_ERR;
  if (reason == reqerr_e::NETWORK_ERR) {
    HTTP2_LOG(TRACE, "reader drops connection after network err: {}", ec.what(), con.name);
  }
dropConnection:
  self->drop_connection(reason);
connection_dropped:
  if (!self->m_connectionWaiters.empty() && !self->alreadyConnecting()) {
    HTTP2_LOG(TRACE, "client initiates reconnect after graceful shutdown or out of streams", con.name);
    co_await dd::this_coro::destroy_and_transfer_control_to(
        startConnecting(self, deadline_after(self->m_options.connectionTimeout)).handle);
  }
  co_return;
}

http2_client::~http2_client() {
  HTTP2_LOG(TRACE, "~http2_client", name());
  cancel_all();
  // make sure client was closed correctly before destroy
  assert(!m_connection);
  assert(m_connectionWaiters.empty());
  assert(m_requestsInProgress == 0);
}

http2_client::http2_client(endpoint host, http2_client_options opts,
                           move_only_fn<any_transport_factory(asio::io_context&)> tf)
    : m_host(std::move(host)), m_options(opts), m_factory(tf(m_ioctx)) {
  assert(m_factory);
  m_name.set_prefix(CLIENT_PREFIX);
  m_options.maxReceiveFrameSize = std::min(FRAME_LEN_MAX, m_options.maxReceiveFrameSize);
  HTTP2_LOG(TRACE, "http2_client created", name());
}

noexport::waiter_of_connection::~waiter_of_connection() {
  // in case when .destroy on handle called correctly cancels request
  if (is_linked()) {
    erase_byref(client->m_connectionWaiters, *this);
  }
}

bool noexport::waiter_of_connection::await_ready() noexcept {
  if (!client->m_connection || client->m_connection->isDropped() ||
      client->m_connection->isOutofStreamids()) {
    return false;
  }
  result = client->m_connection;
  return true;
}

std::coroutine_handle<> noexport::waiter_of_connection::await_suspend(std::coroutine_handle<> h) noexcept {
  task = h;
  client->m_connectionWaiters.push_back(*this);
  if (client->alreadyConnecting()) {
    return std::noop_coroutine();
  }
  return client->startConnecting(client, deadline).handle;
}

[[nodiscard]] h2connection_ptr noexport::waiter_of_connection::await_resume() {
  if (!result || result->isDropped() || client->stopRequested()) {
    return nullptr;
  }
  return std::move(result);
}

void http2_client::drop_connection(reqerr_e::values_e reason) noexcept {
  h2connection_ptr con = std::move(m_connection);
  if (!con) {
    return;
  }
  // note: i have shared ptr to con, so it will not be destroyed while shutting
  // down and resuming its reader/writer
  con->shutdown(reason);
}

dd::task<int> http2_client::send_request(on_header_fn_ptr onHeader, on_data_part_fn_ptr onDataPart,
                                         http_request request, deadline_t deadline) {
  // for CONNECT send_connect_request
  assert(request.method != http_method_e::CONNECT);
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  if (deadline.isReached()) [[unlikely]] {
    co_return reqerr_e::TIMEOUT;
  }
  ++m_requestsInProgress;
  on_scope_exit {
    --m_requestsInProgress;
  };
  h2connection_ptr con = co_await borrowConnection(deadline);
  if (deadline.isReached()) {
    co_return reqerr_e::TIMEOUT;
  }
  if (!con) {
    co_return reqerr_e::NETWORK_ERR;
  }
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  assert(!request.path.empty());
  request.scheme = con->tcpCon->is_https() ? scheme_e::HTTPS : scheme_e::HTTP;
  stream_id_t streamid = con->nextStreamid();
  HTTP2_LOG(TRACE, "sending http2 request, path: {}, method: {}, streamid: {}", request.path,
            e2str(request.method), streamid, con->name);

  stream_ptr node = con->new_stream_node(std::move(request), deadline, onHeader, onDataPart, streamid);

  co_return co_await con->responseReceived(*node);
}

dd::task<int> http2_client::send_streaming_request(on_header_fn_ptr on_header,
                                                   on_data_part_fn_ptr on_data_part, http_request request,
                                                   stream_body_maker_t makebody, deadline_t deadline) {
  assert(request.body.data.empty());
  assert(makebody);
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  if (deadline.isReached()) [[unlikely]] {
    co_return reqerr_e::TIMEOUT;
  }
  ++m_requestsInProgress;
  on_scope_exit {
    --m_requestsInProgress;
  };
  h2connection_ptr con = co_await borrowConnection(deadline);
  if (deadline.isReached()) {
    co_return reqerr_e::TIMEOUT;
  }
  if (!con) {
    co_return reqerr_e::NETWORK_ERR;
  }
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  assert(!request.path.empty());
  request.scheme = con->tcpCon->is_https() ? scheme_e::HTTPS : scheme_e::HTTP;
  stream_id_t streamid = con->nextStreamid();
  HTTP2_LOG(TRACE, "sending http2 streaming request, path: {}, method: {}, streamid: {}", request.path,
            e2str(request.method), streamid, con->name);

  stream_ptr node = con->newStreamingRequestNode(std::move(request), deadline, on_header, on_data_part,
                                                 streamid, std::move(makebody));

  co_return co_await con->responseReceived(*node);
}

[[noreturn]] static void throw_bad_status(int status) {
  assert(status < 0);
  using enum reqerr_e::values_e;
  switch (reqerr_e::values_e(status)) {
    case DONE:
      unreachable();
    case TIMEOUT:
      throw timeout_exception{};
    case NETWORK_ERR:
      throw network_exception{""};
    case PROTOCOL_ERR:
      throw protocol_error(errc_e::PROTOCOL_ERROR);
    case CANCELLED:
      throw std::runtime_error("HTTP client: request was canceled");
    case SERVER_CANCELLED_REQUEST:
      throw std::runtime_error("HTTP client: request was canceled by server");
    default:
    case UNKNOWN_ERR:
      throw std::runtime_error("HTTP client unknown error happens");
  }
}

dd::task<http_response> http2_client::send_request(http_request request, deadline_t deadline) {
  http_response rsp;
  auto onHeader = [&](std::string_view name, std::string_view value) {
    rsp.headers.emplace_back(std::string(name), std::string(value));
  };
  auto onDataPart = [&](std::span<byte_t const> bytes, bool /*lastPart*/) {
    rsp.body.insert(rsp.body.end(), bytes.begin(), bytes.end());
  };
  rsp.status = co_await send_request(&onHeader, &onDataPart, std::move(request), deadline);
  if (rsp.status < 0) {
    throw_bad_status(rsp.status);
  }

  co_return rsp;
}

dd::task<http_response> http2_client::send_streaming_request(http_request request,
                                                             stream_body_maker_t makebody,
                                                             deadline_t deadline) {
  assert(makebody);
  http_response rsp;
  auto onHeader = [&](std::string_view name, std::string_view value) {
    rsp.headers.emplace_back(std::string(name), std::string(value));
  };
  auto onDataPart = [&](std::span<byte_t const> bytes, bool /*lastPart*/) {
    rsp.body.insert(rsp.body.end(), bytes.begin(), bytes.end());
  };
  rsp.status = co_await send_streaming_request(&onHeader, &onDataPart, std::move(request),
                                               std::move(makebody), deadline);
  if (rsp.status < 0) {
    throw_bad_status(rsp.status);
  }

  co_return rsp;
}

dd::task<int> http2_client::send_connect_request(
    http_request request,
    move_only_fn<streaming_body_t(http_response, memory_queue_ptr, request_context)> makestream,
    deadline_t deadline) {
  assert(request.method == http_method_e::CONNECT);
  assert(request.body.data.empty());
  assert(makestream);
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  if (deadline.isReached()) [[unlikely]] {
    co_return reqerr_e::TIMEOUT;
  }
  ++m_requestsInProgress;
  on_scope_exit {
    --m_requestsInProgress;
  };
  h2connection_ptr con = co_await borrowConnection(deadline);
  if (deadline.isReached()) {
    co_return reqerr_e::TIMEOUT;
  }
  if (!con) {
    co_return reqerr_e::NETWORK_ERR;
  }
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  request.scheme = con->tcpCon->is_https() ? scheme_e::HTTPS : scheme_e::HTTP;
  stream_id_t streamid = con->nextStreamid();
  HTTP2_LOG(TRACE, "sending CONNECT request, streamid: {}", streamid, con->name);

  http_response rsp;

  stream_ptr node = con->new_stream_node(std::move(request), deadline, nullptr, nullptr, streamid);

  auto on_header = [&](std::string_view name, std::string_view value) {
    rsp.headers.emplace_back(std::string(name), std::string(value));
  };
  node->onHeader = &on_header;
  // marks `node` as streaming, so END_STREAM will not be set in HEADERS
  node->makebody = [](http_headers_t&, request_context) -> streaming_body_t {
    assert(false);
    std::terminate();
  };
  int status = co_await con->responseReceived(*node);
  if (status < 0) {
    throw_bad_status(status);
  }
  // server accepted connect request
  rsp.status = status;

  auto makechan = [rsp = std::move(rsp), mkstream = std::move(makestream)](http_headers_t&,
                                                                           request_context ctx) mutable {
    return mkstream(std::move(rsp), new memory_queue(*ctx.node), ctx);
  };
  node->makebody = std::move(makechan);

  auto sleepcb = [this, node](duration_t d, io_error_code& ec) -> dd::task<void> {
    if (node->finished() || !node->connection || node->connection->isDropped())
      throw stream_error(errc_e::STREAM_CLOSED, node->streamid, "stream already canceled");
    return sleep(d, ec);
  };
  auto neterrcb = [] { /*noop*/ };

  co_await dd::suspend_and_t([&](dd::task<int>::handle_type me) {
    node->task = me;
    // do not wait this stream, instead waiting RST_STREAM / END_STREAM flag
    (void)write_stream_data</*IS_CLIENT=*/true>(node, con, new writer_callbacks(sleepcb, neterrcb));
  });
  co_return status;
}

dd::task<void> http2_client::graceful_stop() {
  HTTP2_LOG(TRACE, "http2_client::graceful_stop started", name());
  on_scope_exit {
    HTTP2_LOG(TRACE, "http2_client::graceful_stop ended", name());
  };
  io_error_code ec;
  h2connection_ptr con = m_connection;  // prevent destroy for graceful shutdown.

  // waiting all requests finish

  // prevent new requests on this connection
  if (con && !con->gracefulshutdownGoawaySended) {
    con->initiateGracefulShutdown(con->lastInitiatedStreamId());
  }
  // wait all requests done
  while (m_requestsInProgress != 0) {
    co_await sleep(std::chrono::nanoseconds(10), ec);
  }
  // request stop
  ++m_stopRequested;
  on_scope_exit {
    --m_stopRequested;
  };
  // wait all 'connect' coroutines done
  co_await m_connectionGate.close();
  co_await yield_on_ioctx(ioctx());
  // prevent new connection tries and wait all startConnecting coroutines are
  // done
  auto lock = lockConnections();
  assert(!m_notYetReadyConnection);
  // 1 is my connection lock here.
  // case when startConnecting on stage creating tcp connection
  while (m_isConnecting > 1) {
    co_await sleep(std::chrono::nanoseconds(10), ec);
  }
  // notify all not started requests about stop
  notifyConnectionWaiters(nullptr);
  // drop our connection correctly if exists
  drop_connection(reqerr_e::CANCELLED);

  co_await m_connectionPartsGate.close();
  co_await yield_on_ioctx(ioctx());
  m_connectionPartsGate = {};  // reopen
  m_connectionGate = {};       // reopen

  lock.release();
  assert(!m_connection);
  assert(m_connectionWaiters.empty());
  assert(m_requestsInProgress == 0);
}

bool http2_client::connected() const {
  return !!m_connection;
}

bool http2_client::is_https() const noexcept {
  assert(m_connection);
  return m_connection.get()->tcpCon->is_https();
}

void http2_client::set_host(endpoint s) noexcept {
  assert(!connected());
  m_host = std::move(s);
}

dd::task<bool> http2_client::try_connect(deadline_t deadline) {
  if (stopRequested() || m_connectionGate.is_closed()) {
    co_return false;
  }
  auto guard = m_connectionGate.hold();
  h2connection_ptr con = co_await borrowConnection(deadline);
  co_return !!con;
}

void http2_client::cancel_all() noexcept {
  auto all_canceled = [&] { return !m_notYetReadyConnection && !m_connection && m_requestsInProgress == 0; };
  while (!all_canceled()) {
    drop_connection(reqerr_e::CANCELLED);
    notifyConnectionWaiters(nullptr);
    if (m_notYetReadyConnection)
      m_notYetReadyConnection->shutdown(reqerr_e::CANCELLED);
    if (!m_ioctx.get_executor().running_in_this_thread())
      m_ioctx.poll();  // do smth pending
  }
}

dd::task<void> http2_client::sleep(duration_t d, io_error_code& ec) {
  boost::asio::steady_timer timer(ioctx());
  co_await net.sleep(timer, d, ec);
}

size_t http2_client::count_active_requests() const noexcept {
  return m_requestsInProgress;
}

size_t http2_client::max_count_requests_allowed() const noexcept {
  return m_connection ? m_connection->remoteSettings.maxConcurrentStreams : size_t(-1);
}

}  // namespace http2
#pragma GCC diagnostic pop
