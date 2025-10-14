

#include "http2/http2_send_frames.hpp"

#include "http2/http2_connection.hpp"

namespace http2 {

dd::task<bool> send_goaway(http2_connection_ptr_t con, stream_id_t streamid, errc_e errc,
                           std::string dbginfo) {
  if (!con || con->isDropped()) {
    co_return false;
  }
  HTTP2_LOG(TRACE, "sending goaway frame: errc: {}, laststreamid: {}, dbginfo: {}", e2str(errc), streamid,
            dbginfo, con->name);
  if (errc == errc_e::NO_ERROR) {
    if (con->gracefulshutdownGoawaySended) {
      co_return true;
    }
    con->gracefulshutdownGoawaySended = true;
  }
  bytes_t bytes;
  goaway_frame::form(streamid, errc, std::move(dbginfo), std::back_inserter(bytes));
  HTTP2_WAIT_WRITE(*con);
  io_error_code ec;
  co_await con->write(bytes, ec);
  if (ec) {
    if (!con->isDropped()) {
      // ignore error if we dropped connection anyway
      HTTP2_LOG(TRACE, "err while sending GOAWAY: err: {}", ec.what(), con->name);
    }
    co_return false;
  }
  co_return true;
}

dd::task<void> send_rst_stream(http2_connection_ptr_t con, stream_id_t streamid, errc_e errc) {
  if (!con || con->isDropped()) {
    co_return;
  }
  HTTP2_LOG(TRACE, "sending rst stream: id: {}, errc: {}", streamid, e2str(errc), con->name);
  byte_t bytes[rst_stream::LEN];
  rst_stream::form(streamid, errc, bytes);
  HTTP2_WAIT_WRITE(*con);
  io_error_code ec;
  co_await con->write(bytes, ec);
  if (ec) {
    if (!con->isDropped()) {
      // ignore error if we dropped connection anyway
      HTTP2_LOG(ERROR, "cannot rst stream: ec: {}", ec.what(), con->name);
    }
  }
}

dd::task<void> send_settings_ack(http2_connection_ptr_t con) {
  if (!con || con->isDropped()) {
    co_return;
  }
  HTTP2_LOG(TRACE, "sending settings ack", con->name);
  bytes_t bytes;
  accepted_settings_frame().form(std::back_inserter(bytes));
  HTTP2_WAIT_WRITE(*con);
  io_error_code ec;
  co_await con->write(bytes, ec);
  if (ec) {
    HTTP2_LOG(ERROR, "cannot send settings ACK: err: {}", ec.what(), con->name);
  }
}

dd::task<bool> send_ping(http2_connection_ptr_t con, uint64_t data, bool requestPong) {
  if (!con || con->isDropped()) {
    co_return false;
  }
  HTTP2_LOG(TRACE, "sending ping", con->name);
  io_error_code ec;
  byte_t buf[ping_frame::LEN];
  ping_frame::form(data, requestPong, buf);
  HTTP2_WAIT_WRITE(*con);
  co_await con->write(buf, ec);
  co_return !ec;
}

dd::task<void> handle_ping(ping_frame ping, http2_connection_ptr_t con) {
  HTTP2_LOG(TRACE, "received ping, data: {}", ping.getData(), con->name);
  if (ping.header.flags & flags::ACK) {
    if (ping.getData() == PING_VALUE) {
      HTTP2_LOG(TRACE, "server DID respond ping frame", con->name);
      con->pingdeadlinetimer.cancel();
    }
    co_return;
  }
  if (!co_await send_ping(con, ping.getData(),
                          /*requestPong=*/false)) {
    HTTP2_LOG(ERROR, "cannot handle ping", con->name);
  }
}

dd::task<bool> send_window_update(http2_connection_ptr_t con, stream_id_t id, uint32_t inc) {
  if (!con || con->isDropped()) {
    co_return false;
  }
  byte_t buf[window_update_frame::LEN];
  window_update_frame::form(id, inc, buf);
  HTTP2_LOG(TRACE, "sending window update: stream: {}, inc: {}", id, inc, con->name);
  HTTP2_WAIT_WRITE(*con);
  io_error_code ec;
  co_await con->write(std::span(buf), ec);
  co_return !ec;
}

// sends WINDOW_UPDATE correctly to set window size to max
dd::task<void> update_window_to_max(cfint_t& size, stream_id_t streamid, http2_connection_ptr_t con) try {
  assert(con);
  // its possible to have < 0 in such cases like
  // * sending DATA before settings exchange
  // * updating settings value SETTINGS_INITIAL_WINDOW_SIZE
  while (size < 0) {
    // avoid too big window size increment
    if (co_await send_window_update(con, streamid, MAX_WINDOW_SIZE)) {
      increment_window_size_trusted(size, MAX_WINDOW_SIZE);
    } else {
      co_return;
    }
    if (con->isDropped()) {
      co_return;
    }
  }
  if (size != MAX_WINDOW_SIZE) [[likely]] {
    static_assert(std::numeric_limits<uint32_t>::max() > MAX_WINDOW_SIZE);
    uint32_t inc = uint32_t(MAX_WINDOW_SIZE - size);
    if (co_await send_window_update(con, 0, inc)) {
      increment_window_size_trusted(size, int32_t(inc));
    }
  }
} catch (std::exception& e) {
  // do not finish streams / send goaway. Will repeat try to update window later
  // anyway
  HTTP2_LOG(ERROR, "sending window update ended with error: {}", e.what(), con->name);
}

}  // namespace http2
