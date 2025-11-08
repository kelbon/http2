#include "test_connection.hpp"

#include <source_location>
#include <utility>

#include <format>

#include <http2/logger.hpp>
#include "fuzzer.hpp"

#define FAKE_HTTP2_LOG(TYPE, STR, ...)       \
  HTTP2_LOG(TYPE,                            \
            STR                              \
            " in {} "                        \
            "{}" __VA_OPT__(, ) __VA_ARGS__, \
            __func__, "[FAKE]", this->con->name)

namespace http2 {

std::string sourceloc_str(std::source_location loc) {
  return std::format("{}:{}:{}", loc.file_name(), loc.line(), loc.column());
}

void remove_padding_etc(h2frame& f) {
  assert(f.hdr.type == frame_e::HEADERS || f.hdr.type == frame_e::DATA);
  http2_frame_t frame(f.hdr, f.data);
  REQUIRE_NOTHROW(frame.validate_streamid(), frame.removePadding());
  if (f.hdr.type == frame_e::HEADERS) {
    frame.ignoreDeprecatedPriority();
  }
  if (f.data.size() != frame.data.size()) {
    f.hdr = frame.header;
    f.data.assign(frame.data.begin(), frame.data.end());
  }
}

}  // namespace http2

namespace http2 {

test_h2connection::test_h2connection(h2connection_ptr ccon, bool client) noexcept
    : con(std::move(ccon)), is_client_con(client) {
  if (is_client())
    con->name.set_prefix(CLIENT_CONNECTION_PREFIX);
  else
    con->name.set_prefix(SERVER_SESSION_PREFIX);
}

dd::task<void> test_h2connection::receiveGoAway(uint32_t lastStreamId, errc_e errorCode, ping_e ping,
                                                deadline_t deadline, std::source_location loc) {
  FAKE_HTTP2_LOG(INFO, "");

  h2frame f = co_await nextFrame(deadline, ping, window_e::SKIP, loc);
  REQUIRE(f.hdr.type == frame_e::GOAWAY);
  goaway_frame gf;
  REQUIRE_NOTHROW(gf = goaway_frame::parse(f.hdr, f.data));
  REQUIRE(gf.errorCode == errorCode);
  REQUIRE(gf.lastStreamId == lastStreamId);
}

dd::task<void> test_h2connection::sendGoAway(uint32_t lastStreamId, errc_e errc, std::string_view debugInfo) {
  FAKE_HTTP2_LOG(INFO, "");

  std::vector<byte_t> bytes;
  goaway_frame::form(lastStreamId, errc, std::string(debugInfo), std::back_inserter(bytes));

  return sendRawFrame(std::move(bytes));
}

dd::task<void> test_h2connection::sendRstStream(uint32_t streamId, errc_e errc) {
  FAKE_HTTP2_LOG(INFO, "");

  std::vector<byte_t> bytes;
  rst_stream::form(streamId, errc, std::back_inserter(bytes));

  return sendRawFrame(std::move(bytes));
}

dd::task<void> test_h2connection::receiveRstStream(uint32_t streamId, errc_e error, deadline_t deadline,
                                                   std::source_location sl) {
  FAKE_HTTP2_LOG(INFO, "");

  h2frame f = co_await nextFrame(deadline, ping_e::RESPONSE, window_e::SKIP, sl);

  REQUIRE(f.hdr.type == frame_e::RST_STREAM);
  REQUIRE(f.hdr.streamId == streamId);
  rst_stream rf;
  REQUIRE_NOTHROW(rf = rst_stream::parse(f.hdr, f.data));
  REQUIRE(rf.errorCode == error);
}

dd::task<void> test_h2connection::receiveSettingsAck(deadline_t deadline, std::source_location loc) {
  h2frame f = co_await nextFrame(deadline, ping_e::RESPONSE, window_e::RETURN, loc);

  REQUIRE(f.hdr.type == frame_e::SETTINGS);
  REQUIRE(f.hdr.flags & flags::ACK);
}

dd::task<void> test_h2connection::sendSettingsAck() {
  return sendFrame({accepted_settings_frame()});
}

dd::task<h2frame> test_h2connection::receiveSettings(deadline_t deadline, std::source_location loc) {
  FAKE_HTTP2_LOG(INFO, "");

  h2frame f = co_await nextFrame(deadline, ping_e::RESPONSE, window_e::RETURN, loc);

  // for test purpose apply settings before receiving ACK to make code easier
  settings_frame::parse(f.hdr, f.data, [&](setting_t s) {
    if (s.identifier == SETTINGS_HEADER_TABLE_SIZE) {
      assert(con->encoder.dyntab.current_size() == 0 && "receiving settings when connection already used");
      con->encoder.dyntab.set_user_protocol_max_size(s.value);
    }
  });
  REQUIRE(f.hdr.type == frame_e::SETTINGS);
  REQUIRE(!(f.hdr.flags & flags::ACK));

  co_return f;
}

dd::task<void> test_h2connection::receiveAndCheckSettings(std::map<setting_id_e, uint32_t> expected,
                                                          std::set<setting_id_e> unexpected,
                                                          deadline_t deadline, std::source_location loc) {
  h2frame f = co_await receiveSettings(deadline, loc);

  settings_frame::parse(f.hdr, f.data, [&](setting_t s) {
    if (expected.contains(s.identifier)) {
      REQUIRE(expected.at(s.identifier) == s.value);
      expected.erase(s.identifier);
    }
    REQUIRE(!unexpected.contains(s.identifier));
  });

  REQUIRE(expected.empty());
}

dd::task<void> test_h2connection::sendSettings() {
  FAKE_HTTP2_LOG(INFO, "");

  settings_t settings;
  settings.maxConcurrentStreams = 0x7fffffff;
  settings.maxFrameSize = m_maxFrameSize;
  settings.deprecatedPriorityDisabled = true;
  if (m_headerTabSize.has_value()) {
    settings.headerTableSize = *m_headerTabSize;
  }

  std::vector<byte_t> bytes;
  settings_frame::form(settings, std::back_inserter(bytes));

  return sendRawFrame(std::move(bytes));
}

dd::task<uint64_t> test_h2connection::receivePing(deadline_t deadline, std::source_location loc) {
  FAKE_HTTP2_LOG(INFO, "");

  h2frame f;
  for (;;) {
    f = co_await receiveFrame(deadline, loc);
    switch (f.hdr.type) {
      case frame_e::WINDOW_UPDATE:
        continue;
      case frame_e::PING:
        goto end;
      default:
        FAIL(std::format("unexpected frame {}, loc: {}", f.hdr, sourceloc_str(loc)));
    }
  }
end:
  ping_frame pf;
  REQUIRE_NOTHROW(pf = ping_frame::parse(f.hdr, f.data));
  REQUIRE(!(f.hdr.flags & flags::ACK));

  co_return pf.getData();
}

dd::task<void> test_h2connection::sendPing(uint64_t opaqueData) {
  FAKE_HTTP2_LOG(INFO, "");
  std::vector<byte_t> bytes;
  ping_frame::form(opaqueData, /*requestAnswer=*/true, std::back_inserter(bytes));
  return sendRawFrame(std::move(bytes));
}

dd::task<void> test_h2connection::sendPong(uint64_t opaqueData) {
  FAKE_HTTP2_LOG(INFO, "");
  std::vector<byte_t> bytes;
  ping_frame::form(opaqueData, /*requestAnswer=*/false, std::back_inserter(bytes));
  return sendRawFrame(std::move(bytes));
}

dd::task<h2frame> test_h2connection::receiveData(uint32_t streamId, deadline_t deadline) {
  FAKE_HTTP2_LOG(INFO, "");

  h2frame f = co_await nextFrame(deadline, ping_e::RESPONSE, window_e::SKIP);

  REQUIRE(f.hdr.type == frame_e::DATA);
  if (streamId) {
    REQUIRE(f.hdr.streamId == streamId);
  }
  remove_padding_etc(f);

  co_return f;
}

dd::task<hdrs_and_data> test_h2connection::receiveReq(deadline_t deadline, std::source_location loc) {
  FAKE_HTTP2_LOG(INFO, "");
  hdrs_and_data hd;
  h2frame f = co_await nextFrame(deadline, ping_e::RESPONSE, window_e::SKIP, loc);
  REQUIRE(f.hdr.type == frame_e::HEADERS);
  REQUIRE(f.hdr.flags & flags::END_HEADERS);
  hd.streamId = f.hdr.streamId;
  remove_padding_etc(f);
  std::span headers{f.data.begin(), f.data.end()};
  std::vector<header> decoded;
  hpack::decode_headers_block(
      con->decoder, headers, [this, &decoded](std::string_view name, std::string_view value) {
        decoded.push_back(
            {std::string(name), std::string(value), con->decoder.dyntab.find(name, value).value_indexed});
      });

  hd.headers = std::move(decoded);
  hd.endStream = f.hdr.flags & flags::END_STREAM;

  if (!hd.endStream) {
    f = co_await receiveData(hd.streamId, deadline);
    hd.body = std::move(f.data);
    hd.endStream = f.hdr.flags & flags::END_STREAM;
  }

  co_return hd;
}

dd::task<void> test_h2connection::sendRsp(stream_id_t streamid, std::vector<header> headers,
                                          http_body_bytes body, bool endstream) {
  FAKE_HTTP2_LOG(INFO, "");
  h2frame hdrs;

  for (auto&& [name, value, _] : headers) {
    con->encoder.encode(name, value, std::back_inserter(hdrs.data));
  }
  // hope hdrs size < max frame size in tests
  hdrs.hdr.length = uint32_t(hdrs.data.size());
  hdrs.hdr.flags = flags::END_HEADERS;
  hdrs.hdr.type = frame_e::HEADERS;
  hdrs.hdr.streamId = streamid;
  if (body.empty() && endstream) {
    hdrs.hdr.flags |= flags::END_STREAM;
  }
  co_await sendFrame(std::move(hdrs));
  if (body.empty()) {
    co_return;
  }
  h2frame data;
  REQUIRE(body.size() <= MIN_MAX_FRAME_LEN);
  data.hdr.length = uint32_t(body.size());
  data.hdr.type = frame_e::DATA;  // -V1048
  if (endstream) {
    data.hdr.flags = flags::END_STREAM;
  }
  data.hdr.streamId = streamid;
  data.data = std::move(body);
  co_await sendFrame(std::move(data));
}

dd::task<void> test_h2connection::sendHeaders(stream_id_t streamid, std::vector<header> headers,
                                              bool endstream) {
  FAKE_HTTP2_LOG(INFO, "");
  // reuse sendReq, which will only send one HEADERS frame
  return sendRsp(streamid, std::move(headers), {}, endstream);
}

dd::task<void> test_h2connection::sendReq(stream_id_t streamid, std::vector<header> headers,
                                          http_body_bytes body, bool endstream) {
  REQUIRE(is_client());
  return sendRsp(streamid, std::move(headers), std::move(body), endstream);
}

dd::task<void> test_h2connection::sendRawHdr(uint32_t streamId, std::span<const byte_t> headers,
                                             bool end_stream, bool split) {
  FAKE_HTTP2_LOG(INFO, "");
  if (split) {
    fuzzing::fuzzer fuz;
    bool first = true;
    for (std::span chunk : fuz.chunks(headers)) {
      h2frame f;
      f.hdr.length = uint32_t(chunk.size());
      f.hdr.streamId = streamId;
      f.hdr.type = first ? frame_e::HEADERS : frame_e::CONTINUATION;
      if (first && end_stream)
        f.hdr.flags |= flags::END_STREAM;
      // stuped msvc stl asserts when comparting iterators from different spans
      // even if they are to same array
      //  chunk.end() == headers.end()
      if (chunk.data() + chunk.size() == headers.data() + headers.size())  // last chunk
        f.hdr.flags |= flags::END_HEADERS;
      first = false;
      f.data.assign(chunk.begin(), chunk.end());

      co_await sendFrame(std::move(f));
    }
  } else {  // !split
    h2frame f;
    f.hdr.length = uint32_t(headers.size());
    f.hdr.type = frame_e::HEADERS;
    f.hdr.flags = flags::END_HEADERS;
    f.hdr.streamId = streamId;
    if (end_stream)
      f.hdr.flags |= flags::END_STREAM;
    f.data.assign(headers.begin(), headers.end());

    co_await sendFrame(std::move(f));
  }
}

dd::task<void> test_h2connection::sendRawContinuation(stream_id_t streamId, std::span<byte_t> headers,
                                                      bool end_headers) {
  FAKE_HTTP2_LOG(INFO, "");
  h2frame f;
  f.hdr = {
      .length = uint32_t(headers.size()),
      .type = frame_e::CONTINUATION,
      .flags = end_headers ? flags::END_HEADERS : flags::EMPTY_FLAGS,
      .streamId = streamId,
  };
  f.data.assign(headers.begin(), headers.end());

  co_await sendFrame(std::move(f));
}

std::vector<byte_t> test_h2connection::encode_headers(std::vector<header> hdrs) {
  std::vector<byte_t> bytes;
  for (auto& h : hdrs)
    if (h.indexed)
      con->encoder.encode(h.name, h.value, std::back_inserter(bytes));
    else
      con->encoder.encode_header_without_indexing(h.name, h.value, std::back_inserter(bytes));
  return bytes;
}

dd::task<void> test_h2connection::sendData(uint32_t streamId, std::string_view body, bool endStream) {
  FAKE_HTTP2_LOG(INFO, "");

  uint32_t bodySize = static_cast<uint32_t>(body.size());
  REQUIRE(bodySize);  // check data
  h2frame f;
  f.hdr.type = frame_e::DATA;
  f.hdr.length = uint32_t(body.size());
  f.hdr.streamId = streamId;
  f.hdr.flags = endStream ? flags::END_STREAM : flags::EMPTY_FLAGS;
  f.data.assign(body.begin(), body.end());
  return sendFrame(std::move(f));
}

dd::task<void> test_h2connection::sendWindowSizeIncrement(uint32_t streamId, uint32_t increment) {
  FAKE_HTTP2_LOG(INFO, "");

  std::vector<byte_t> bytes;
  window_update_frame::form(streamId, increment, std::back_inserter(bytes));

  return sendRawFrame(std::move(bytes));
}

dd::task<h2frame> test_h2connection::nextFrame(deadline_t d, ping_e pingbehavior, window_e windowbehavior,
                                               std::source_location loc) {
  FAKE_HTTP2_LOG(INFO, "");
  do {
    h2frame frame;

    frame = co_await receiveFrame(d, loc);

    if (frame.hdr.type == frame_e::PING) {
      switch (pingbehavior) {
        case ping_e::RESPONSE:
          if (!(frame.hdr.flags & flags::ACK)) {
            frame.hdr.flags &= flags::ACK;
            co_await sendFrame(std::move(frame));
          }
          continue;
        case ping_e::ERROR:
          FAIL("Ping frame received, but not expected");
          unreachable();
        default:
          FAIL("invalid ping_e");
          unreachable();
      }
    } else if (frame.hdr.type == frame_e::WINDOW_UPDATE) {
      switch (windowbehavior) {
        case window_e::RETURN:
          co_return frame;
        case window_e::SKIP:
          // ignores control flow etc
          continue;
      }
    } else {
      co_return frame;
    }
  } while (true);
}

dd::task<void> test_h2connection::receiveClientMagic(std::source_location) {
  FAKE_HTTP2_LOG(INFO, "");
  byte_t buf[sizeof(CONNECTION_PREFACE)];
  io_error_code ec;
  co_await con->read(buf, ec);
  REQUIRE(!ec);
  REQUIRE(memcmp(buf, CONNECTION_PREFACE, sizeof(CONNECTION_PREFACE)) == 0);
}

dd::task<void> test_h2connection::sendFrame(h2frame frame) {
  FAKE_HTTP2_LOG(INFO, "sending frame {}", frame.hdr);
  assert(frame.data.size() == frame.hdr.length);
  if (frame.data.capacity() == 0)
    frame.data.reserve(1);  // force use special allocator p9
  auto* b = frame.data.data() - FRAME_HEADER_LEN;
  frame.hdr.form(b);
  io_error_code ec;
  if (frame.data.size() == 0) {
    co_await con->write({b, FRAME_HEADER_LEN}, ec);
  } else {
    co_await con->write({b, frame.data.data() + frame.data.size()}, ec);
  }
  REQUIRE(!ec);
}

dd::task<void> test_h2connection::sendRawFrame(std::vector<byte_t> bytes) {
  FAKE_HTTP2_LOG(INFO, "sending raw frame: {} bytes", bytes.size());
  io_error_code ec;
  co_await con->write(bytes, ec);
}

dd::task<h2frame> test_h2connection::receiveFrame(deadline_t d, std::source_location loc) {
  bool done = false;  // avoid dangling

  auto f = [&](test_h2connection& self) -> dd::task<h2frame> {
    on_scope_exit {
      done = true;
    };
    h2frame frame;
    std::array<byte_t, FRAME_HEADER_LEN> hdr;
    io_error_code ec;
    co_await self.con->read(hdr, ec);
    REQUIRE(!ec);
    frame.hdr = frame_header::parse(hdr);
    if (frame.hdr.type != frame_e::GOAWAY)
      FAKE_HTTP2_LOG(DEBUG, "receive frame: {}", frame.hdr);
    REQUIRE(frame.hdr.length <= self.m_maxFrameSize);
    frame.data.resize(frame.hdr.length);
    co_await self.con->read(frame.data, ec);
    REQUIRE(!ec);
    if (frame.hdr.type == frame_e::GOAWAY) {
      auto gf = goaway_frame::parse(frame.hdr, frame.data);
      FAKE_HTTP2_LOG(DEBUG, "receive GOAWAY frame, ec: {}, debugInfo: {}", e2str(gf.errorCode), gf.debugInfo);
    }
    co_return frame;
  };
  asio::steady_timer timer(con->ioctx);
  timer.expires_at(d.tp);
  timer.async_wait([&](const io_error_code& ec) {
    if (ec != asio::error::operation_aborted) {
      FAKE_HTTP2_LOG(ERROR, "receiveFrame: deadline reached {}", sourceloc_str(loc));
      con->shutdown(reqerr_e::TIMEOUT);
    }
  });
  h2frame res = co_await f(*this);
  timer.cancel();
  co_return res;
}

dd::task<void> test_h2connection::sendClientMagic() {
  FAKE_HTTP2_LOG(INFO, "");
  io_error_code ec;
  co_await con->write(CONNECTION_PREFACE, ec);
  REQUIRE(!ec);
}

dd::task<void> test_h2connection::waitConnectionDropped(deadline_t deadline, std::source_location loc) {
  FAKE_HTTP2_LOG(INFO, "");
  asio::steady_timer timer(con->ioctx);
  timer.expires_at(deadline.tp);
  bool timedout = false;
  timer.async_wait([&](const io_error_code& ec) {
    if (ec != asio::error::operation_aborted) {
      timedout = true;
      con->shutdown(reqerr_e::TIMEOUT);
    }
  });
  io_error_code ec;
  byte_t buf[512];
  while (!ec && !timedout)
    co_await con->read(buf, ec);
  REQUIRE(!timedout);
}

void test_h2connection::close() {
  if (con) {
    FAKE_HTTP2_LOG(INFO, "");
    con->shutdown(reqerr_e::values_e::CANCELLED);
  }
}

}  // namespace http2
