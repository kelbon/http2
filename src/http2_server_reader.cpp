

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

// handles only utility frames (not DATA / HEADERS), returns false on protocol
// error (may throw it too)
static bool server_handle_utility_frame(http2_frame_t frame, server_session& session) {
  using enum frame_e;

  http2_connection& con = *session.connection;

  switch (frame.header.type) {
    case HEADERS:
    case DATA:
      unreachable();
    case SETTINGS:
      session.clientSettingsChanged(frame);
      return true;
    case PING:
      handle_ping(ping_frame::parse(frame.header, frame.data), &con).start_and_detach();
      return true;
    case RST_STREAM:
      if (!session.rstStreamServer(rst_stream::parse(frame.header, frame.data))) {
        HTTP2_LOG(INFO, "client finished stream (id: {}) which is not exists", frame.header.streamId,
                  session.name());
      }
      return true;
    case GOAWAY: {
      goaway_frame f = goaway_frame::parse(frame.header, frame.data);
      if (f.errorCode != errc_e::NO_ERROR) {
        throw goaway_exception(f.lastStreamId, f.errorCode, std::move(f.debugInfo));
      } else {
        session.clientRequestsGracefulShutdown(f);
        return true;
      }
    }
    case WINDOW_UPDATE:
      con.windowUpdate(window_update_frame::parse(frame.header, frame.data));
      return true;
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
      return true;
  }
}

// TODO избавиться от этих функкций мб и просто handle frame сделать
// handles DATA or HEADERS, returns false on protocol error
[[nodiscard]] static bool server_handle_headers_or_data(http2_frame_t frame, server_session& session) {
  using enum frame_e;
  http2_connection& con = *session.connection;

  assert(con.localSettings.deprecatedPriorityDisabled);
  if (frame.header.streamId == 0) {
    return false;
  }
  if (!frame.removePadding()) {
    return false;
  }
  session.connection->validateDataOrHeadersFrameSize(frame.header);
  if ((frame.header.streamId % 2) == 0) [[unlikely]] {
    HTTP2_LOG(ERROR, "client tries to initiate stream with even stream id", session.name());
    return false;
  }
  switch (frame.header.type) {
    case HEADERS: {
      frame.ignoreDeprecatedPriority();
      if (session.newRequestsForbiden) [[unlikely]] {
        session.connection->ignoreFrame(frame);
        send_rst_stream(&con, frame.header.streamId, errc_e::REFUSED_STREAM).start_and_detach();
        return true;
      }

      session.startRequestAssemble(frame);
      return true;
    }
    case DATA: {
      request_node* node = con.findResponseByStreamid(frame.header.streamId);
      if (!node) {
        con.ignoreFrame(frame);
        return true;
      }
      if (node->is_half_closed_server()) {
        throw stream_error(errc_e::STREAM_CLOSED, frame.header.streamId, "stream already assembled");
      }
      // applicable only to data
      // Note: includes padding!
      // https://www.rfc-editor.org/rfc/rfc9113.html#section-4.2-1
      decrease_window_size(con.myWindowSize, int32_t(frame.header.length));
      node->receiveRequestData(frame);
      if (frame.header.flags & flags::END_STREAM) {
        // Note: manages 'node' lifetime
        session.onRequestReady(*node);
      }
      return true;
    }
    default:
      unreachable();
  }
  return true;
}

// returns false on protocol error
[[nodiscard]] static bool server_handle_frame(http2_frame_t frame, server_session& session) try {
  using enum frame_e;
  switch (frame.header.type) {
    case HEADERS:
    case DATA:
      return server_handle_headers_or_data(frame, session);
    default:
      return server_handle_utility_frame(frame, session);
  }
} catch (stream_error& e) {
  HTTP2_LOG(ERROR, "stream exception in reader. err: {}", e.what(), session.name());
  session.rstStreamAfterError(e);
  return true;  // do not require connection close
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

  try {
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

      if (!server_handle_frame(frame, session)) {
        co_return reqerr_e::PROTOCOL_ERR;
      }
      // connection control flow (streamlevel in server_handle_frame)
      if (con.myWindowSize < http2::MAX_WINDOW_SIZE / 2) {
        co_await update_window_to_max(con.myWindowSize, 0, &con);
      }
    }
  } catch (hpack::protocol_error& e) {
    HTTP2_LOG(ERROR, "hpack error happens in reader, err: {}", e.what(), session.name());
    send_goaway(&con, con.lastInitiatedStreamId(), errc_e::COMPRESSION_ERROR, e.what()).start_and_detach();
    goto hpack_error;
  } catch (protocol_error& e) {
    HTTP2_LOG(ERROR, "exception in reader. err: {}", e.what(), session.name());
    send_goaway(&con, MAX_STREAM_ID, e.errc, e.what()).start_and_detach();
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
  unreachable();
hpack_error:
  co_return reqerr_e::PROTOCOL_ERR;
} catch (std::exception& e) {
  HTTP2_LOG(ERROR, "reader ended with exception: {}", e.what(), session.name());
  co_return reqerr_e::UNKNOWN_ERR;
}

}  // namespace http2
