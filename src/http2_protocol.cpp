

#include "http2v2/http2_protocol.hpp"

#include "http2v2/http_base.hpp"

#include <hpack/hpack.hpp>

namespace http2v2 {

void server_settings_visitor::operator()(setting_t s) {
  switch (s.identifier) {
  case SETTINGS_HEADER_TABLE_SIZE:
    // TODE ? check if new size > old size, then protocol error (same in client
    // settings visitor)
    settings.headerTableSize = s.value;
    return;
  case SETTINGS_ENABLE_PUSH:
    if (s.value > 0) {
      HTTP2_LOG(ERROR, "invalid server settings enable push, value: {}",
                auto(s.value));
      throw protocol_error{}; // server MUST NOT send i
    }
    settings.enablePush = false;
    return;
  case SETTINGS_MAX_CONCURRENT_STREAMS:
    settings.maxConcurrentStreams = s.value;
    return;
  case SETTINGS_INITIAL_WINDOW_SIZE:
    settings.initialStreamWindowSize = s.value;
    return;
  case SETTINGS_MAX_FRAME_SIZE:
    settings.maxFrameSize = s.value;
    return;
  case SETTINGS_MAX_HEADER_LIST_SIZE:
    settings.maxHeaderListSize = s.value;
    return;
  case SETTINGS_NO_RFC7540_PRIORITIES:
    /*
    Senders MUST NOT change the SETTINGS_NO_RFC7540_PRIORITIES value after the
    first SETTINGS frame. Receivers that detect a change MAY treat it as a
    connection error of type PROTOCOL_ERROR.
    */
    if (s.value > 1 || !firstframe) {
      HTTP2_LOG(ERROR,
                "invalid server setting NO_RFC7540 option, value: {}, is first "
                "settings frame: {}",
                auto(s.value), firstframe);
      throw protocol_error{};
    }
    return;
  default:
      // ignore if dont know
      ;
  }
}

void client_settings_visitor::operator()(setting_t s) {
  switch (s.identifier) {
  case SETTINGS_HEADER_TABLE_SIZE:
    settings.headerTableSize = s.value;
    return;
  case SETTINGS_ENABLE_PUSH:
    if (s.value > 1) {
      HTTP2_LOG(ERROR, "invalid client settings enable_push value: {}",
                auto(s.value));
      throw protocol_error{};
    }
    settings.enablePush = s.value;
    return;
  case SETTINGS_MAX_CONCURRENT_STREAMS:
    settings.maxConcurrentStreams = s.value;
    return;
  case SETTINGS_INITIAL_WINDOW_SIZE:
    settings.initialStreamWindowSize = s.value;
    return;
  case SETTINGS_MAX_FRAME_SIZE:
    settings.maxFrameSize = s.value;
    return;
  case SETTINGS_MAX_HEADER_LIST_SIZE:
    settings.maxHeaderListSize = s.value;
    return;
  case SETTINGS_NO_RFC7540_PRIORITIES:
    /*
    Senders MUST NOT change the SETTINGS_NO_RFC7540_PRIORITIES value after the
    first SETTINGS frame. Receivers that detect a change MAY treat it as a
    connection error of type PROTOCOL_ERROR.
    */
    if (s.value > 1 || !firstframe) {
      HTTP2_LOG(ERROR,
                "invalid client NO_RFC7440 setting, value {}, is first "
                "settings frame: {}",
                auto(s.value), firstframe);
      throw protocol_error{};
    }
    return;
  default:
      // ignore if dont know
      ;
  }
}

rst_stream rst_stream::parse(frame_header h, std::span<byte_t const> bytes) {
  assert(h.type == frame_e::RST_STREAM);
  if (h.streamId == 0 || h.length != 4) {
    HTTP2_LOG(ERROR, "invalid rst stream, streamid {}, len: {}", h.streamId,
              h.length);
    throw protocol_error{};
  }
  rst_stream frame(h);
  memcpy(&frame.errorCode, bytes.data(), 4);
  htonli(frame.errorCode);
  return frame;
}

ping_frame ping_frame::parse(frame_header h, std::span<byte_t const> bytes) {
  assert(h.type == frame_e::PING && h.length == bytes.size());
  ping_frame f(h);
  if (h.streamId != 0 || h.length != 8) {
    HTTP2_LOG(ERROR, "invalid ping frame, streamid {}, len: {}", h.streamId,
              h.length);
    throw protocol_error{};
  }
  memcpy(f.data, bytes.data(), 8);
  return f;
}

goaway_frame goaway_frame::parse(frame_header header,
                                 std::span<byte_t const> bytes) {
  assert(header.type == frame_e::GOAWAY);
  goaway_frame frame;
  if (header.length < 8) // TODE check streamid == 0 etc by rfc
  {
    HTTP2_LOG(ERROR, "invalid goaway frame, streamid {}, len: {}",
              header.streamId, header.length);
    throw protocol_error{};
  }
  memcpy(&frame.lastStreamId, bytes.data(), 4);
  memcpy(&frame.errorCode, bytes.data() + 4, 4);
  htonli(frame.lastStreamId);
  htonli(frame.errorCode);
  frame.debugInfo = std::string_view((char const *)bytes.data() + 8,
                                     (char const *)bytes.data() + bytes.size());
  return frame;
}

window_update_frame window_update_frame::parse(frame_header header,
                                               std::span<byte_t const> bytes) {
  assert(header.length == bytes.size());
  if (header.length != 4) {
    HTTP2_LOG(ERROR, "invalid window update frame, streamid {}, len: {}",
              header.streamId, header.length);
    throw connection_error(errc_e::FRAME_SIZE_ERROR);
  }
  window_update_frame frame{.header = header};
  std::memcpy(&frame.windowSizeIncrement, bytes.data(), 4);
  htonli(frame.windowSizeIncrement);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9-6
  if (frame.windowSizeIncrement >= uintmax_t(1u << 31)) {
    HTTP2_LOG(ERROR,
              "invalid window update frame (window size increment), streamid "
              "{}, len: {}, increment: {}, provided data size : {} ",
              header.streamId, header.length, frame.windowSizeIncrement,
              bytes.size());
    throw protocol_error{}; // reserved bit filled
  }
  if (frame.windowSizeIncrement == 0) {
    if (header.streamId) {
      HTTP2_LOG(ERROR,
                "invalid window update frame: increment == 0, streamid {}",
                header.streamId);
      throw stream_error{header.streamId};
    } else {
      HTTP2_LOG(ERROR,
                "invalid window update frame: increment == 0, streamid == 0");
      throw protocol_error{};
    }
  }
  return frame;
}

void parse_http2_request_headers(hpack::decoder &d,
                                 std::span<hpack::byte_t const> bytes,
                                 http_request &req) {
  auto const *in = bytes.data();
  auto const *e = in + bytes.size();
  hpack::header_view header;

  // parse required pseudoheaders

  bool schemeParsed = false;
  bool pathParsed = false;
  bool methodParsed = false;
  bool authorityParsed = false;
  bool contenttypeParsed = false;
  while (in != e) {
    d.decode_header(in, e, header);
    if (!header) // skip dynamic size updates
    {
      continue;
    }
    if (header.name == ":path") {
      if (pathParsed) {
        throw protocol_error{};
      }
      pathParsed = true;
      req.path = header.value.str();
      if (req.path.empty()) {
        throw protocol_error(errc_e::PROTOCOL_ERROR, ":path header is empty");
      }
    } else if (header.name == ":method") {
      if (methodParsed) {
        throw protocol_error{};
      }
      methodParsed = true;
      enum_from_string(header.value.str(), req.method);
    } else if (header.name == ":scheme") {
      if (schemeParsed) {
        throw protocol_error{};
      }
      schemeParsed = true;
      enum_from_string(header.value.str(), req.scheme);
    } else if (header.name == ":authority") {
      if (authorityParsed) {
        throw protocol_error{};
      }
      authorityParsed = true;
      req.authority = header.value.str();
    } else if (header.name == "content-type") {
      if (contenttypeParsed) {
        throw protocol_error{};
      }
      contenttypeParsed = true;
      req.body.contentType = header.value.str();
    } else {
      goto push_header;
    }
  }
  while (in != e) {
    d.decode_header(in, e, header);
    if (!header) {
      continue;
    }
    if (header.name.str().starts_with(':')) {
      throw protocol_error(
          errc_e::PROTOCOL_ERROR,
          fmt::format("pseudoheader {} after first not pseudoheader",
                      header.name.str()));
    }
  push_header: // -V2529
    req.headers.push_back(http_header_t(std::string(header.name.str()),
                                        std::string(header.value.str())));
  }
  auto checkrequired = [](std::string_view hdrname, bool &parsed) {
    if (!parsed) [[unlikely]] {
      throw protocol_error(
          errc_e::PROTOCOL_ERROR,
          fmt::format("required header {} not present", hdrname));
    }
  };
  checkrequired(":path", pathParsed);
  checkrequired(":method", methodParsed);
  checkrequired(":scheme", schemeParsed);
  checkrequired(":authority", authorityParsed);
}

} // namespace http2v2
