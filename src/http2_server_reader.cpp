

#include "http2/http2_server_reader.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/http2_server_session.hpp"
#include "http2/logger.hpp"
#include "http2/utils/reusable_buffer.hpp"

#include <zal/zal.hpp>
#undef NO_ERROR
namespace http2 {

// handles only utility frames (not DATA / HEADERS)
static void server_handle_utility_frame(http2_frame_t frame, server_session& session) {
  using enum frame_e;

  http2_connection& con = *session.connection;

  switch (frame.header.type) {
    case HEADERS:
    case DATA:
      unreachable();
    case SETTINGS:
      session.clientSettingsChanged(frame);
      return;
    case PING:
      handle_ping(ping_frame::parse(frame.header, frame.data), &con).start_and_detach();
      return;
    case RST_STREAM:
      if (!session.rstStreamServer(rst_stream::parse(frame.header, frame.data))) {
        HTTP2_LOG(INFO, "client finished stream (id: {}) which is not exists", frame.header.streamId,
                  session.name());
      }
      return;
    case GOAWAY: {
      goaway_frame f = goaway_frame::parse(frame.header, frame.data);
      if (f.errorCode != errc_e::NO_ERROR) {
        throw goaway_exception(f.lastStreamId, f.errorCode, std::move(f.debugInfo));
      } else {
        session.clientRequestsGracefulShutdown(f);
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

dd::task<int> start_server_reader_for(http2::server_session& session) try {
  auto guard = session.connectionPartsGate.hold();
  assert(session.connection);
  using enum frame_e;
  HTTP2_LOG(TRACE, "reader started", session.name());
  on_scope_exit {
    HTTP2_LOG(TRACE, "reader ended", session.name());
  };
  http2_connection& con = *session.connection;
  io_error_code ec;
  reusable_buffer buffer;
  http2_frame_t frame;

  for (;;) {
    if (con.isDropped()) {
      co_return reqerr_e::DONE;
    }

    // read frame header

    frame.data = buffer.getExactly(http2::FRAME_HEADER_LEN);

    co_await con.read(frame.data, ec);

    if (ec) {
      co_return reqerr_e::NETWORK_ERR;
    }
    if (con.isDropped()) {
      co_return reqerr_e::DONE;
    }

    // parse frame header

    frame.header = frame_header::parse(frame.data);
    if (!frame.validateHeader()) {
      co_return reqerr_e::PROTOCOL_ERR;
    }

    // read frame data

    frame.data = buffer.getExactly(frame.header.length);
    co_await con.read(frame.data, ec);
    if (ec) {
      co_return reqerr_e::NETWORK_ERR;
    }
    if (con.isDropped()) {
      co_return reqerr_e::DONE;
    }
    ++session.framecount;
    if (session.connection->pingdeadlinetimer.armed()) [[unlikely]] {  // client not idle
      session.connection->pingdeadlinetimer.cancel();
    }

    // handle frame

    try {
      switch (frame.header.type) {
        case HEADERS:
          session.receive_headers(frame);
          break;
        case DATA:
          session.receive_data(frame);
          break;
        default:
          server_handle_utility_frame(frame, session);
          break;
      }
    } catch (stream_error& _e) {
      // workaround windows ABI https://github.com/llvm/llvm-project/issues/153949
      auto& e = _e;
      HTTP2_LOG(ERROR, "stream exception in reader. err: {}", e.what(), session.name());
      session.rstStreamAfterError(e);
      // do not require connection close
    }

    // connection control flow (streamlevel in server_handle_frame)
    if (con.myWindowSize < http2::MAX_WINDOW_SIZE / 2) {
      co_await update_window_to_max(con.myWindowSize, 0, &con);
    }
  }
  unreachable();
} catch (hpack::protocol_error& e) {
  HTTP2_LOG(ERROR, "hpack error happens in reader, err: {}", e.what(), session.name());
  send_goaway(session.connection, session.connection->lastInitiatedStreamId(), errc_e::COMPRESSION_ERROR,
              e.what())
      .start_and_detach();
  co_return reqerr_e::PROTOCOL_ERR;
} catch (protocol_error& e) {
  HTTP2_LOG(ERROR, "exception in reader. err: {}", e.what(), session.name());
  send_goaway(session.connection, MAX_STREAM_ID, e.errc, e.what()).start_and_detach();
  co_return reqerr_e::PROTOCOL_ERR;
} catch (goaway_exception& gae) {
  HTTP2_LOG(ERROR, "goaway received, {}", gae.what(), session.name());
  co_return reqerr_e::CANCELLED;
} catch (std::exception& se) {
  HTTP2_LOG(INFO, "unexpected exception in reader {}", se.what(), session.name());
  co_return reqerr_e::UNKNOWN_ERR;
} catch (...) {
  HTTP2_LOG(INFO, "unknown exception happens in reader", session.name());
  co_return reqerr_e::UNKNOWN_ERR;
}

}  // namespace http2
