

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

#include <hpack/encoder.hpp>
#include <zal/zal.hpp>

#include <boost/asio/error.hpp>

namespace http2 {

constexpr inline auto H2FHL = FRAME_HEADER_LEN;

template <bool IS_CLIENT>
static void generate_http2_headers_to(request_node const& node, hpack::encoder& encoder, bytes_t& headers) {
  using hdrs = hpack::static_table_t::values;
  auto const& request = node.req;

  assert(!IS_CLIENT || !request.path.empty());

  auto out = std::back_inserter(headers);

  if constexpr (IS_CLIENT) {
    // required scheme, method, authority, path
    hdrs scheme = request.scheme == scheme_e::HTTPS ? hdrs::scheme_https : hdrs::scheme_http;
    encoder.encode_header_fully_indexed(scheme, out);

    switch (request.method) {
      case http_method_e::GET:
        encoder.encode_header_fully_indexed(hdrs::method_get, out);
        break;
      case http_method_e::POST:
        encoder.encode_header_fully_indexed(hdrs::method_post, out);
        break;
      default:
        encoder.encode_with_cache(hdrs::method_get, e2str(request.method), out);
    }
    if (!request.authority.empty()) {
      encoder.encode_with_cache(hdrs::authority, request.authority, out);
    }
    encoder.encode_with_cache(hdrs::path, request.path, out);
  } else {
    // server, required only :status
    assert(node.status > 0);
    encoder.encode_status(node.status, out);
  }

  if (!request.body.contentType.empty()) {
    encoder.encode_with_cache(hdrs::content_type, request.body.contentType, out);
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

struct writer_callbacks {
  writer_sleepcb_t sleepcb;
  writer_on_network_err_t neterrcb;
  size_t refcount = 0;
};

static void intrusive_ptr_add_ref(writer_callbacks* p) noexcept {
  ++p->refcount;
}

static void intrusive_ptr_release(writer_callbacks* p) noexcept {
  --p->refcount;
  if (p->refcount == 0) {
    delete p;
  }
}

using writer_callbacks_ptr = boost::intrusive_ptr<writer_callbacks>;

template <bool Streaming>
static dd::task<void> write_data(node_ptr work, http2_connection_ptr_t con, writer_callbacks_ptr cbs,
                                 io_error_code& ec) try {
  assert(con && work && cbs && cbs->neterrcb && cbs->sleepcb);
  http_body_bytes& data = work->req.body.data;

  // uses guarantee about FRAME_LEN_BYTES before .data()
  static_assert(std::is_same_v<decltype(work->req.body.data)::allocator_type, detail::allocator_p9<byte_t>>);

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
    (void)co_await con->write(std::span(in - H2FHL, framelen + H2FHL), ec);

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

static dd::task<void> write_trailers(http2_connection& con, stream_id_t streamid, http_headers_t headers,
                                     io_error_code& ec) {
  HTTP2_LOG(TRACE, "sendind trailers for stream {}", streamid, con.name);
  // reserve memory for frame header
  std::vector<byte_t> bytes(FRAME_HEADER_LEN);
  auto out = std::back_inserter(bytes);
  for (auto& [name, value] : headers) {
    assert(is_lowercase(name) && "http2 requires headers to be in lowercase");
    con.encoder.encode_with_cache(name, value, out);
  }
  // TODO if len > max frame size
  frame_header headersframeheader{
      .length = uint32_t(bytes.size() - FRAME_HEADER_LEN),
      .type = frame_e::HEADERS,
      .flags = flags::END_STREAM | flags::END_HEADERS,  // trailers
      .streamId = streamid,
  };
  headersframeheader.form(bytes.data());
  co_await con.write(bytes, ec);
}

static dd::job write_stream_data(node_ptr node, http2_connection_ptr_t con, writer_callbacks_ptr cbs) try {
  assert(node && node->is_streaming());

  request_node& snode = *node;

  assert(!!snode.makebody);

  io_error_code ec;

  // channel may fill trailers to send them
  http_headers_t trailers;

  // Note: order. `chan` destroyed before `makebody`
  on_scope_exit {
    // clear for future using from freelist
    node->makebody.reset();
    assert(!node->is_streaming());
  };
  streaming_body_t chan = snode.makebody(trailers);

  on_scope_exit {
    snode.req.body = {};
  };
  // create 'b' before loop to handle exception after loop
  auto b = co_await chan.begin();
  for (; b != chan.end(); (co_await (++b))) {
    std::span<const byte_t> chunk = *b;
    assert(chunk.size() > 0);
    if (snode.finished() || con->isDropped())
      co_return;
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
    byte_t bytes[FRAME_HEADER_LEN];
    data_frame::end_stream_marker(node->streamid).form(+bytes);

    co_await con->write(bytes, ec);

    if (snode.finished() || con->isDropped())
      co_return;
    if (ec)
      goto end;
  }

  co_return;
end:
  if (ec != boost::asio::error::operation_aborted) {
    con->finishRequest(snode, reqerr_e::NETWORK_ERR);
  }
  cbs->neterrcb();
} catch (std::exception& e) {
  con->finishRequest(*node, reqerr_e::UNKNOWN_ERR);
  send_rst_stream(con, node->streamid, errc_e::CANCEL).start_and_detach();
  HTTP2_LOG(ERROR, "writing streaming DATA for stream {} ended with error, err: {}", node->streamid, e.what(),
            con->name);
}

template <bool IS_CLIENT>
dd::job start_writer_for(http2_connection_ptr_t con, writer_sleepcb_t sleepcb,
                         writer_on_network_err_t neterrcb, bool forcedisablehpack, gate::holder) {
  assert(con && sleepcb && neterrcb);
  // make callbacks easy to copy into write_pending_frames for future use
  writer_callbacks_ptr cbs = new writer_callbacks(std::move(sleepcb), std::move(neterrcb));

  HTTP2_LOG(TRACE, "writer started", con->name);
  on_scope_exit {
    HTTP2_LOG(TRACE, "writer ended", con->name);
  };

  io_error_code ec;
  for (;;) {
    // waiting for job or connection shutdown

    if (!co_await con->waitWork()) {
      goto end;
    }
    assert(!con->requests.empty());

    // ignores con->concurrentStreamsNow()
    // TODO if headers > con->settings.maxFrameSize

    while (!con->requests.empty()) {
      node_ptr node = &con->requests.front();

      con->requests.pop_front();
      con->insertResponseNode(*node);

      // send headers

      // ignores con->concurrent_streams_now()
      // TODO if headers > con->settings.max_frame_size
      bytes_t headers(FRAME_HEADER_LEN, 0);  // reserve for frame header

      // https://www.rfc-editor.org/rfc/rfc9113.html#name-settings-synchronization
      if (con->encodertablesizechangerequested) [[unlikely]] {
        con->encodertablesizechangerequested = false;
        if (con->remoteSettings.headerTableSize < con->encoder.dyntab.max_size()) {
          con->encoder.encode_dynamic_table_size_update(con->remoteSettings.headerTableSize,
                                                        std::back_inserter(headers));
        }
        con->encoder.dyntab.set_user_protocol_max_size(con->remoteSettings.headerTableSize);
      }
      if (node->streamid == 1 && forcedisablehpack) [[unlikely]] {
        con->encoder.encode_dynamic_table_size_update(0, std::back_inserter(headers));
        con->encoder.dyntab.update_size(0);
      }

      generate_http2_headers_to<IS_CLIENT>(*node, con->encoder, headers);
      using namespace flags;
      frame_header headersframeheader{
          .length = uint32_t(headers.size() - FRAME_HEADER_LEN),
          .type = frame_e::HEADERS,
          .flags = flags_t(node->has_body() ? END_HEADERS : (END_HEADERS | END_STREAM)),
          .streamId = node->streamid,
      };
      headersframeheader.form(headers.data());
      HTTP2_LOG(TRACE, "sending headers block: stream {}, block size: {}", node->streamid,
                headers.size() - H2FHL, con->name);
#ifdef HTTP2_ENABLE_TRACE
      trace_request_headers(*node, IS_CLIENT);
#endif
      (void)co_await con->write(headers, ec);

      if (ec || con->isDropped()) {
        // otherwise will be finished by drop_connection with
        // reqerr_e::cancelled
        if (ec != boost::asio::error::operation_aborted) {
          con->finishRequest(*node, reqerr_e::NETWORK_ERR);
        }
        goto end;
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
      }
      if constexpr (!IS_CLIENT) {
        assert(!node->onHeader && !node->onDataPart);
        con->finishRequest(*node, node->status);
      }
      if constexpr (IS_CLIENT) {
        if (node->is_streaming()) {
          (void)write_stream_data(node, con, cbs);
        }
      }
    }
  }  // end loop handling requests
end:
  cbs->neterrcb();
}

dd::job start_writer_for_client(http2_connection_ptr_t con, writer_sleepcb_t sleepcb,
                                writer_on_network_err_t neterrcb, bool forcedisablehpack,
                                gate::holder guard) {
  return start_writer_for</*IS_CLIENT=*/true>(std::move(con), std::move(sleepcb), std::move(neterrcb),
                                              forcedisablehpack, std::move(guard));
}

dd::job start_writer_for_server(http2_connection_ptr_t con, writer_sleepcb_t sleepcb,
                                writer_on_network_err_t neterrcb, bool forcedisablehpack,
                                gate::holder guard) {
  return start_writer_for</*IS_CLIENT=*/false>(std::move(con), std::move(sleepcb), std::move(neterrcb),
                                               forcedisablehpack, std::move(guard));
}

}  // namespace http2
#pragma GCC diagnostic pop
