

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wsign-compare"

#include "http2/http2_writer.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/http_base.hpp"
#include "http2/http_body.hpp"
#include "http2/asio/asio_executor.hpp"
#include "http2/request_context.hpp"

#include <hpack/encoder.hpp>
#include <zal/zal.hpp>

#include <boost/asio/error.hpp>

namespace http2 {

constexpr inline auto H2FHL = FRAME_HEADER_LEN;

// client-side
static void generate_http2_connect_headers(request_node const& node, hpack::encoder& encoder,
                                           bytes_t& bytes) {
  assert(node.req.method == http_method_e::CONNECT);

  auto& req = node.req;
  auto out = std::back_inserter(bytes);
  // https://datatracker.ietf.org/doc/html/rfc8441#section-4
  bool extended_connect = std::ranges::find(req.headers, std::string_view(":protocol"),
                                            &http_header_t::name) != req.headers.end();

  encoder.encode(hpack::static_table_t::method_get, "CONNECT", out);
  // does not check single value "websocket" for :protocol pseudoheader for future extensions
  // anyway server will check it
  if (extended_connect) {
    // pseudoheaders must be first!
    // do not handle / reorder user headers here to make sure
    // echo server will exactly copy what user writes in request
    assert(req.headers.front().name() == ":protocol");
    assert(req.body.content_type.empty());
    using hdrs = hpack::static_table_t::values;
    hdrs scheme = req.scheme == scheme_e::HTTPS ? hdrs::scheme_https : hdrs::scheme_http;
    encoder.encode_header_fully_indexed(scheme, out);
    if (!req.authority.empty()) {
      encoder.encode_with_cache(hdrs::authority, req.authority, out);
    }
    encoder.encode_with_cache(hdrs::path, req.path, out);
  } else {
    // https://www.rfc-editor.org/rfc/rfc9113.html#name-the-connect-method
    // :path :scheme MUST be omitted, authority required
    assert(!req.authority.empty());  // must be setted, address for server TCP connection
  }
  for (auto& h : req.headers) {
    encoder.encode(h.name(), h.value(), out);
  }
}

template <bool IS_CLIENT>
static void generate_http2_headers_to(request_node const& node, hpack::encoder& encoder, bytes_t& headers) {
  using hdrs = hpack::static_table_t::values;
  auto const& request = node.req;

  assert(!IS_CLIENT || !request.path.empty() || node.is_connect_request());

  auto out = std::back_inserter(headers);

  if constexpr (IS_CLIENT) {
    // Note: order
    // method, scheme, path, authority
    // to match grpc Call-Definition https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md
    // (in case someone will implement grpc over this HTTP/2 impl)

    switch (request.method) {
      case http_method_e::GET:
        encoder.encode_header_fully_indexed(hdrs::method_get, out);
        break;
      case http_method_e::POST:
        encoder.encode_header_fully_indexed(hdrs::method_post, out);
        break;
      case http_method_e::CONNECT:
        if constexpr (IS_CLIENT) {
          generate_http2_connect_headers(node, encoder, headers);
          return;
        } else {
          [[fallthrough]];
        }
      default:
        encoder.encode_with_cache(hdrs::method_get, e2str(request.method), out);
    }
    hdrs scheme = request.scheme == scheme_e::HTTPS ? hdrs::scheme_https : hdrs::scheme_http;
    encoder.encode_header_fully_indexed(scheme, out);
    encoder.encode_with_cache(hdrs::path, request.path, out);
    if (!request.authority.empty()) {
      encoder.encode_with_cache(hdrs::authority, request.authority, out);
    }
  } else {
    // server, required only :status
    assert(node.status > 0);
    encoder.encode_status(node.status, out);
  }

  if (!request.body.content_type.empty()) {
    encoder.encode_with_cache(hdrs::content_type, request.body.content_type, out);
  }

  // custom headers

  for (auto& [name, value] : request.headers) {
    assert(is_lowercase(name) && "http2 requires headers to be in lowercase");
    encoder.encode_with_cache(name, value, out);
  }
}

// precondition: node.req.data is not empty
// forms new data frame
// also handles window size changes
// returns length of result DATA frame
// or 0 if cannot send because of control flow
// precondition: 'out' contains atleast 9 valid bytes
template <bool Streaming>
[[nodiscard]] static cfint_t fill_data_header(request_node const& node, http2_connection const& con,
                                              size_t unhandledBytes, byte_t* out) noexcept {
  using enum frame_e;
  using namespace flags;

  assert(!node.req.body.data.empty());
  assert(node.req.body.data.size() >= unhandledBytes);
  frame_header header;
  cfint_t len = std::min<int64_t>({int64_t(unhandledBytes), con.remoteSettings.maxFrameSize,
                                   node.lrStreamlevelWindowSize, con.receiverWindowSize});
  if (len <= 0) [[unlikely]] {
    return len;
  }
  header.length = len;
  header.type = DATA;
  if constexpr (!Streaming) {
    header.flags = unhandledBytes == header.length ? END_STREAM : EMPTY_FLAGS;
  } else {
    header.flags = EMPTY_FLAGS;
  }
  header.streamId = node.streamid;
  header.form(out);

  return header.length;
}

template <bool Streaming>
static dd::task<void> write_data(node_ptr work, http2_connection_ptr_t con, writer_callbacks_ptr cbs,
                                 io_error_code& ec) try {
  assert(con && work && cbs && cbs->neterrcb && cbs->sleepcb);
  http_body_bytes& data = work->req.body.data;

  // uses guarantee about FRAME_LEN_BYTES before .data()
  static_assert(
      std::is_same_v<typename decltype(work->req.body.data)::allocator_type, detail::allocator_p9<byte_t>>);

  cfint_t framelen = 0;
  byte_t* in = data.data();
  byte_t* dataEnd = in + data.size();

  for (; in != dataEnd; in += framelen) {
    if (work->finished() || con->isDropped()) {
      co_return;
    }
    framelen = fill_data_header<Streaming>(*work, *con, std::distance(in, dataEnd), in - H2FHL);
    if (framelen <= 0) [[unlikely]] {
      HTTP2_LOG(TRACE,
                "cannot send bytes now! unhandled: {}, max_frame_len: {}, "
                "stream wsz {}, con wsz: {}",
                std::distance(in, dataEnd), con->remoteSettings.maxFrameSize, work->lrStreamlevelWindowSize,
                con->receiverWindowSize, con->name);
      co_await cbs->sleepcb(std::chrono::nanoseconds(500), ec);
      if (ec) {
        HTTP2_LOG(ERROR, "something went wrong while sleeping, con: {}", ec.what(), (void*)con.get());
        if (ec == boost::asio::error::operation_aborted) {
          co_return;
        }
        // continue, ignore sleep errors
      }
      framelen = 0;  // avoid in += framelen which is < 0
      continue;
    }
    HTTP2_LOG(TRACE,
              "FRAME for stream {}, len: {}, unhandled: {}, rws: {}, "
              "csSlWsz: {}, maxFrameSize: {}, DATA: {}",
              work->streamid, framelen, std::distance(in, dataEnd), con->receiverWindowSize,
              work->lrStreamlevelWindowSize, con->remoteSettings.maxFrameSize,
              std::string_view((char const*)in, framelen), con->name);
    // send frame
    HTTP2_WAIT_WRITE(*con);
    co_await con->write(std::span(in - H2FHL, framelen + H2FHL), ec);

    if (ec)
      co_return;
    // control flow
    decrease_window_size(con->receiverWindowSize, framelen);        // connection
    decrease_window_size(work->lrStreamlevelWindowSize, framelen);  // stream
  }  // end loop
  HTTP2_LOG(TRACE, "DATA for stream {} successfully sended", work->streamid, con->name);
  co_return;
} catch (std::exception& e) {
  con->finishRequest(*work, reqerr_e::UNKNOWN_ERR);
  send_rst_stream(con, work->streamid, errc_e::CANCEL).start_and_detach();
  HTTP2_LOG(ERROR, "writing DATA for stream {} ended with error, err: {}", work->streamid, e.what(),
            con->name);
}

// writes CONTINUATION frames
// assumes first 9 bytes of `hdrs` are reserved for frame header
static dd::task<void> write_continuations(http2_connection_ptr_t con, stream_id_t streamid, size_t handled,
                                          bytes_t hdrs, io_error_code& ec) {
  assert(con);
  assert(handled < hdrs.size());
  assert(handled >= H2FHL);
  byte_t* b = hdrs.data() + handled;
  byte_t* e = hdrs.data() + hdrs.size();
  HTTP2_WAIT_WRITE(*con);
  con->continuationGateway.close();
  on_scope_exit {
    asio_executor exe{con->ioctx};
    con->continuationGateway.open(exe);
  };
  size_t framesz;
  for (; b != e; b += framesz) {
    framesz = std::min<size_t>(con->remoteSettings.maxFrameSize, e - b);
    frame_header h{
        .length = uint32_t(framesz),
        .type = frame_e::CONTINUATION,
        .flags = framesz == e - b ? flags::END_HEADERS : flags::EMPTY_FLAGS,
        .streamId = streamid,
    };
    h.form(b - H2FHL);
    HTTP2_LOG(TRACE, "writing CONTINUATION frame for stream {}, len: {}", streamid, framesz, con->name);
    co_await con->write(std::span(b - H2FHL, framesz + H2FHL), ec);

    if (ec || con->isDropped())
      co_return;
  }
}

static dd::task<void> write_trailers(http2_connection& con, stream_id_t streamid, http_headers_t headers,
                                     io_error_code& ec) {
  HTTP2_LOG(TRACE, "sendind trailers for stream {}", streamid, con.name);
  // reserve memory for frame header
  std::vector<byte_t> bytes(H2FHL);
  auto out = std::back_inserter(bytes);
  for (auto& [name, value] : headers) {
    assert(is_lowercase(name) && "http2 requires headers to be in lowercase");
    con.encoder.encode_with_cache(name, value, out);
  }
  size_t framelen = bytes.size() - H2FHL;
  frame_header fhdr;
  fhdr.length = std::min<uint32_t>(framelen, con.remoteSettings.maxFrameSize);
  bool one_frame = fhdr.length == framelen;
  fhdr.type = frame_e::HEADERS;
  fhdr.streamId = streamid;
  if (one_frame) [[likely]] {
    fhdr.flags = flags::END_STREAM | flags::END_HEADERS;  // trailers
  } else {
    fhdr.flags = flags::END_STREAM;
  }
  fhdr.form(bytes.data());
  HTTP2_WAIT_WRITE(con);
  co_await con.write(std::span(bytes.data(), fhdr.length + H2FHL), ec);
  if (!one_frame) [[unlikely]] {
    co_await write_continuations(&con, streamid, fhdr.length + H2FHL, std::move(bytes), ec);
  }
}

template <bool IS_CLIENT>
dd::job write_stream_data(node_ptr node, http2_connection_ptr_t con, writer_callbacks_ptr cbs) try {
  assert(node && node->is_output_streaming());

  request_node& snode = *node;
  assert(!!snode.makebody);

  io_error_code ec;

  // channel may fill trailers to send them
  http_headers_t trailers;

  // Note: order. `chan` destroyed before `makebody` (which destroyed in returnNode)
  streaming_body_t chan = snode.makebody(trailers, request_context(*node));

  on_scope_exit {
    snode.req.body = {};
    snode.makebody.reset();
  };
  // if !IS_CLIENT request finished on each code path
  // create 'b' before loop to handle exception after loop
  HTTP2_ASSUME_THREAD_UNCHANGED_START;
  auto b = co_await chan.begin();
  for (; b != chan.end(); (void)(co_await (++b))) {
    HTTP2_ASSUME_THREAD_UNCHANGED_END;
    std::span<const byte_t> chunk = *b;
    if (snode.finished() || con->isDropped())
      co_return;
    if (chunk.empty())
      continue;
    snode.req.body.data.resize(chunk.size(), uninitialized_byte);
    memcpy(snode.req.body.data.data(), chunk.data(), chunk.size());
    HTTP2_LOG(TRACE, "sendind DATA part for stream {}, len: {}, bodystr: \"{}\"", snode.streamid,
              snode.req.body.data.size(), snode.req.body.strview(), con->name);
    co_await write_data</*Streaming=*/true>(node, con, cbs, ec);

    if (ec)
      goto end;
  }

  if (std::exception_ptr e = chan.take_exception()) {
    con->finishRequestWithUserException(*node, std::current_exception());
    HTTP2_LOG(ERROR, "writing streaming data for stream {} ended with user exception", node->streamid,
              con->name);
    co_return;
  }

  if (snode.finished() || con->isDropped()) {
    co_return;
  }

  if (!trailers.empty()) {
    co_await write_trailers(*con, node->streamid, std::move(trailers), ec);

    if (snode.finished() || con->isDropped())
      co_return;
    if (ec)
      goto end;
  } else {
    // write empty DATA with END_STREAM
    byte_t bytes[H2FHL];
    data_frame::end_stream_marker(node->streamid).form(+bytes);
    HTTP2_WAIT_WRITE(*con);
    co_await con->write(bytes, ec);

    if (snode.finished() || con->isDropped())
      co_return;
    if (ec)
      goto end;
  }
  if constexpr (!IS_CLIENT) {
    con->finishRequest(snode, snode.status);
  } else {
    // client sends all what it need, do not want to receive anything
    if (snode.is_connect_request())
      con->finishRequest(snode, snode.status);
  }
  co_return;
end:
  con->finishRequest(
      snode, ec != boost::asio::error::operation_aborted ? reqerr_e::NETWORK_ERR : reqerr_e::CANCELLED);
  cbs->neterrcb();
} catch (std::exception& e) {
  con->finishRequest(*node, reqerr_e::UNKNOWN_ERR);
  send_rst_stream(con, node->streamid, errc_e::CANCEL).start_and_detach();
  HTTP2_LOG(ERROR, "writing streaming DATA for stream {} ended with error, err: {}", node->streamid, e.what(),
            con->name);
}

template dd::job write_stream_data<true>(node_ptr node, http2_connection_ptr_t con, writer_callbacks_ptr cbs);
template dd::job write_stream_data<false>(node_ptr node, http2_connection_ptr_t con,
                                          writer_callbacks_ptr cbs);

template <bool IS_CLIENT>
dd::job start_writer_for(http2_connection_ptr_t con, writer_sleepcb_t sleepcb,
                         writer_on_network_err_t neterrcb, bool forcedisablehpack, dd::gate::holder) {
  assert(con && sleepcb && neterrcb);
  // make callbacks easy to copy into write_pending_frames for future use
  writer_callbacks_ptr cbs = new writer_callbacks(std::move(sleepcb), std::move(neterrcb));

  HTTP2_LOG(TRACE, "writer started", con->name);
  on_scope_exit {
    HTTP2_LOG(TRACE, "writer ended", con->name);
  };

  io_error_code ec;
  bytes_t headers;
  headers.reserve(128);
  for (;;) {
    // waiting for job or connection shutdown

    if (!co_await con->waitWork()) {
      goto end;
    }
    assert(!con->requests.empty());

    while (!con->requests.empty()) {
      node_ptr node = &con->requests.front();

      con->requests.pop_front();
      con->insertResponseNode(*node);

      // send headers

      if constexpr (IS_CLIENT) {
        while (con->concurrentStreamsNow() >= con->remoteSettings.maxConcurrentStreams) [[unlikely]] {
          HTTP2_LOG(TRACE, "too many streams, waiting (max is {})", con->remoteSettings.maxConcurrentStreams,
                    con->name);
          co_await yield_on_ioctx(con->ioctx);
          if (ec || con->isDropped()) {
            if (ec != boost::asio::error::operation_aborted) {
              con->finishRequest(*node, reqerr_e::NETWORK_ERR);
            }
            goto end;
          }
        }
      }

      headers.resize(H2FHL);  // reserve for frame header

      con->start_headers_block(*node, forcedisablehpack, headers);

      generate_http2_headers_to<IS_CLIENT>(*node, con->encoder, headers);
      using namespace flags;
      size_t hdrslen = headers.size() - H2FHL;
      frame_header fhdr;
      fhdr.length = std::min<uint32_t>(con->remoteSettings.maxFrameSize, hdrslen);
      bool one_frame = hdrslen == fhdr.length;
      fhdr.type = frame_e::HEADERS;
      fhdr.streamId = node->streamid;
      if (one_frame) [[likely]] {
        fhdr.flags = flags_t(node->has_body() ? END_HEADERS : (END_HEADERS | END_STREAM));
      } else {
        fhdr.flags = flags_t(node->has_body() ? EMPTY_FLAGS : END_STREAM);
      }
      fhdr.form(headers.data());
      HTTP2_LOG(TRACE, "sending headers block: stream {}, block size: {}", node->streamid,
                headers.size() - H2FHL, con->name);
#ifdef HTTP2_ENABLE_TRACE
      trace_request_headers(*node, IS_CLIENT);
#endif
      HTTP2_WAIT_WRITE(*con);
      co_await con->write(std::span(headers.data(), fhdr.length + H2FHL), ec);

      if (ec || con->isDropped()) {
        // otherwise will be finished by drop_connection with
        // reqerr_e::cancelled
        if (ec != boost::asio::error::operation_aborted) {
          con->finishRequest(*node, reqerr_e::NETWORK_ERR);
        }
        goto end;
      }

      if (!one_frame) [[unlikely]] {
        co_await write_continuations(con, node->streamid, fhdr.length + H2FHL, std::move(headers), ec);

        if (ec || con->isDropped()) {
          if (ec != boost::asio::error::operation_aborted) {
            con->finishRequest(*node, reqerr_e::NETWORK_ERR);
          }
          goto end;
        }
      }

      // send data
      if (!node->req.body.data.empty()) {
        co_await write_data</*Streaming=*/false>(node, con, cbs, ec);

        if (ec || con->isDropped()) {
          if (ec != boost::asio::error::operation_aborted) {
            con->finishRequest(*node, reqerr_e::NETWORK_ERR);
          }
          goto end;
        }
        if constexpr (!IS_CLIENT) {
          con->finishRequest(*node, node->status);
        }
      } else if (!node->is_output_streaming()) {
        // request has no body
        if constexpr (!IS_CLIENT) {
          con->finishRequest(*node, node->status);
        }
      } else {
        if constexpr (IS_CLIENT) {
          if (!node->is_connect_request())
            (void)write_stream_data<IS_CLIENT>(node, con, cbs);
        } else {
          // stream finished in write_stream-data
          (void)write_stream_data<IS_CLIENT>(node, con, cbs);
        }
      }
    }
  }  // end loop handling requests
end:
  cbs->neterrcb();
}

dd::job start_writer_for_client(http2_connection_ptr_t con, writer_sleepcb_t sleepcb,
                                writer_on_network_err_t neterrcb, bool forcedisablehpack,
                                dd::gate::holder guard) {
  return start_writer_for</*IS_CLIENT=*/true>(std::move(con), std::move(sleepcb), std::move(neterrcb),
                                              forcedisablehpack, std::move(guard));
}

dd::job start_writer_for_server(http2_connection_ptr_t con, writer_sleepcb_t sleepcb,
                                writer_on_network_err_t neterrcb, bool forcedisablehpack,
                                dd::gate::holder guard) {
  return start_writer_for</*IS_CLIENT=*/false>(std::move(con), std::move(sleepcb), std::move(neterrcb),
                                               forcedisablehpack, std::move(guard));
}

}  // namespace http2
#pragma GCC diagnostic pop
