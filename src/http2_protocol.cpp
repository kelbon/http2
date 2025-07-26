

#include "http2/http2_protocol.hpp"

#include "http2/http_base.hpp"

#include <hpack/hpack.hpp>
#include <strswitch/strswitch.hpp>

namespace http2 {

static void validate_initial_window_size(setting_t s) {
  assert(s.identifier == SETTINGS_INITIAL_WINDOW_SIZE);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2-2.8.3
  if (s.value > MAX_WINDOW_SIZE) {
    throw protocol_error(errc_e::FLOW_CONTROL_ERROR,
                         std::format("SETTINGS_INITIAL_WINDOW_SIZE > max, max: {}, value: {}",
                                     MAX_WINDOW_SIZE, uint32_t(s.value)));
  }
}

static void validate_max_frame_size(setting_t s) {
  assert(s.identifier == SETTINGS_MAX_FRAME_SIZE);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2-2.10.2

  // The value advertised by an endpoint MUST be between this initial value and the maximum
  // allowed frame size (2^24-1 or 16,777,215 octets), inclusive
  if (s.value < MIN_MAX_FRAME_LEN || s.value > 16'777'215) {
    throw protocol_error(
        errc_e::PROTOCOL_ERROR,
        std::format("invalid MAX_FRAME_SIZE, must be in range [16'384, 16`777`215], value: {}",
                    uint32_t(s.value)));
  }
}

static void validate_norfc7540_priority(setting_t s, bool firstframe) {
  assert(s.identifier == SETTINGS_NO_RFC7540_PRIORITIES);
  /*
Senders MUST NOT change the SETTINGS_NO_RFC7540_PRIORITIES value after the
first SETTINGS frame. Receivers that detect a change MAY treat it as a
connection error of type PROTOCOL_ERROR.
*/
  if (s.value > 1 || !firstframe) {
    throw protocol_error(
        errc_e::PROTOCOL_ERROR,
        "MUST NOT change the SETTINGS_NO_RFC7540_PRIORITIES value after the first SETTINGS frame");
  }
}

static void validate_enable_push_from_client(setting_t s) {
  assert(s.identifier == SETTINGS_ENABLE_PUSH);
  if (s.value > 1) {
    throw protocol_error(errc_e::PROTOCOL_ERROR,
                         std::format("invalid client settings enable_push value: {}", uint32_t(s.value)));
  }
}

static void validate_enable_push_from_server(setting_t s) {
  assert(s.identifier == SETTINGS_ENABLE_PUSH);
  // server MUST NOT send i
  if (s.value > 0) {
    throw protocol_error(errc_e::PROTOCOL_ERROR,
                         std::format("invalid server settings enable push, value: {}", uint32_t(s.value)));
  }
}

static void validate_rst_stream(const frame_header& h) {
  assert(h.type == frame_e::RST_STREAM);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.4-6
  if (h.streamId == 0) {
    throw protocol_error(errc_e::PROTOCOL_ERROR,
                         std::format("RST_STREAM frame with streamid == 0 ({})", h.streamId));
  }
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.4-8
  if (h.length != 4) {
    throw protocol_error(errc_e::FRAME_SIZE_ERROR,
                         std::format("RST_STREAM frame with invalid len != 4 ({})", h.length));
  }
}

static void validate_ping(const frame_header& h) {
  assert(h.type == frame_e::PING);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.7-8
  if (h.streamId != 0) {
    throw protocol_error(errc_e::PROTOCOL_ERROR,
                         std::format("PING frame with streamid != 0 ({})", h.streamId));
  }
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.7-9
  if (h.length != 8) {
    throw protocol_error(errc_e::FRAME_SIZE_ERROR,
                         std::format("PING frame with invalid len != 8 ({})", h.length));
  }
}

static void validate_goaway(const frame_header& h) {
  assert(h.type == frame_e::GOAWAY);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.8-11
  if (h.streamId != 0) {
    throw protocol_error(errc_e::PROTOCOL_ERROR,
                         std::format("GOAWAY frame with streamid != 0 ({})", h.streamId));
  }
  // must be atleast last stream id and error code both 32 bit
  if (h.length < 8) {
    throw protocol_error(errc_e::FRAME_SIZE_ERROR,
                         std::format("GOAWAY frame with invalid len < 8 ({})", h.length));
  }
}

static void validate_window_update(const frame_header& h) {
  assert(h.type == frame_e::WINDOW_UPDATE);
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9-12
  if (h.length != 4) {
    throw protocol_error(errc_e::FRAME_SIZE_ERROR,
                         std::format("invalid WINDOW_UPDATE len != 4 ({})", h.length));
  }
}

static void validate_window_update_increment(const frame_header& h, cfint_t increment) {
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9-6
  if (increment >= cfint_t(1u << 31)) {
    throw protocol_error(errc_e::FLOW_CONTROL_ERROR,
                         std::format("invalid frame {}, window size increment too big ({})", h, increment));
  }
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9-9
  // (but now both stream and connection related errors are connection error, not stream error)
  if (increment == 0) {
    if (h.streamId != 0) {
      throw stream_error(
          errc_e::PROTOCOL_ERROR, h.streamId,
          std::format("invalid window update frame, increment == 0, streamid: {}", h.streamId));
    } else {
      throw protocol_error(
          errc_e::PROTOCOL_ERROR,
          std::format("invalid window update frame, increment == 0, streamid == 0", h.streamId));
    }
  }
}

std::string_view e2str(frame_e e) noexcept {
  switch (e) {
    case frame_e::DATA:
      return "DATA";
    case frame_e::HEADERS:
      return "HEADERS";
    case frame_e::PRIORITY:
      return "PRIORITY";
    case frame_e::RST_STREAM:
      return "RST_STREAM";
    case frame_e::SETTINGS:
      return "SETTINGS";
    case frame_e::PUSH_PROMISE:
      return "PUSH_PROMISE";
    case frame_e::PING:
      return "PING";
    case frame_e::GOAWAY:
      return "GOAWAY";
    case frame_e::WINDOW_UPDATE:
      return "WINDOW_UPDATE";
    case frame_e::CONTINUATION:
      return "CONTINUATION";
    case frame_e::PRIORITY_UPDATE:
      return "PRIORITY_UPDATE";
  }
  return "UNKNOWN_FRAME";
}

void server_settings_visitor::operator()(setting_t s) {
  switch (s.identifier) {
    case SETTINGS_HEADER_TABLE_SIZE:
      // TODO ? check if new size > old size, then protocol error (same in client
      // settings visitor)
      settings.headerTableSize = s.value;
      return;
    case SETTINGS_ENABLE_PUSH:
      validate_enable_push_from_server(s);
      settings.enablePush = s.value;
      return;
    case SETTINGS_MAX_CONCURRENT_STREAMS:
      settings.maxConcurrentStreams = s.value;
      return;
    case SETTINGS_INITIAL_WINDOW_SIZE:
      validate_initial_window_size(s);
      settings.initialStreamWindowSize = s.value;
      return;
    case SETTINGS_MAX_FRAME_SIZE:
      validate_max_frame_size(s);
      settings.maxFrameSize = s.value;
      return;
    case SETTINGS_MAX_HEADER_LIST_SIZE:
      settings.maxHeaderListSize = s.value;
      return;
    case SETTINGS_NO_RFC7540_PRIORITIES:
      validate_norfc7540_priority(s, firstframe);
      settings.deprecatedPriorityDisabled = s.value;
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
      validate_enable_push_from_client(s);
      settings.enablePush = s.value;
      return;
    case SETTINGS_MAX_CONCURRENT_STREAMS:
      settings.maxConcurrentStreams = s.value;
      return;
    case SETTINGS_INITIAL_WINDOW_SIZE:
      validate_initial_window_size(s);
      settings.initialStreamWindowSize = s.value;
      return;
    case SETTINGS_MAX_FRAME_SIZE:
      validate_max_frame_size(s);
      settings.maxFrameSize = s.value;
      return;
    case SETTINGS_MAX_HEADER_LIST_SIZE:
      settings.maxHeaderListSize = s.value;
      return;
    case SETTINGS_NO_RFC7540_PRIORITIES:
      validate_norfc7540_priority(s, firstframe);
      settings.deprecatedPriorityDisabled = s.value;
      return;
    default:
        // ignore if dont know
        ;
  }
}

rst_stream rst_stream::parse(frame_header h, std::span<byte_t const> bytes) {
  validate_rst_stream(h);
  assert(h.length == bytes.size());

  rst_stream frame(h);
  memcpy(&frame.errorCode, bytes.data(), 4);
  htonli(frame.errorCode);
  return frame;
}

ping_frame ping_frame::parse(frame_header h, std::span<byte_t const> bytes) {
  validate_ping(h);
  assert(h.length == bytes.size());

  ping_frame f(h);
  memcpy(f.data, bytes.data(), 8);
  return f;
}

goaway_frame goaway_frame::parse(frame_header header, std::span<byte_t const> bytes) {
  validate_goaway(header);
  assert(header.length == bytes.size());

  goaway_frame frame;
  memcpy(&frame.lastStreamId, bytes.data(), 4);
  memcpy(&frame.errorCode, bytes.data() + 4, 4);
  htonli(frame.lastStreamId);
  htonli(frame.errorCode);
  frame.debugInfo = std::string_view((char const*)bytes.data() + 8, (char const*)bytes.data() + bytes.size());
  return frame;
}

window_update_frame window_update_frame::parse(frame_header header, std::span<byte_t const> bytes) {
  assert(header.length == bytes.size());
  validate_window_update(header);

  window_update_frame frame{.header = header};
  std::memcpy(&frame.windowSizeIncrement, bytes.data(), 4);
  htonli(frame.windowSizeIncrement);
  validate_window_update_increment(header, frame.windowSizeIncrement);
  return frame;
}

static protocol_error duplicated_pseudoheader(stream_id_t streamid, std::string_view name) {
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-8.3-5
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-8.1.1-3
  // Malformed requests or responses that are detected MUST be treated
  // as a stream error (Section 5.4.2) of type PROTOCOL_ERROR.
  return stream_error(errc_e::PROTOCOL_ERROR, streamid,
                      std::format("pseudoheader {} appeared on the list twice", name));
}

static void validate_header_name(const hpack::header_view& h, stream_id_t streamid) {
  std::string_view str = h.name.str();
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-8.2.1-3.1
  for (int c : str) {
    switch (c) {
      default:
        continue;
      case 0x00 ... 0x20:
      case 0x41 ... 0x5a:
      case 0x7f ... 0xff:
      case ':':
        throw stream_error(errc_e::PROTOCOL_ERROR, streamid,
                           std::format("forbidden character 0x{:x} in header name \"{}\")", c, str));
    }
  }
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-8.2.2-1
  bool connection_specific =
      ss::string_switch<bool>(str)
          .cases("te", "connection", "proxy-connection", "keep-alive", "transfer-encoding", "upgrade", true)
          .orDefault(false);
  if (connection_specific) [[unlikely]] {
    // https://www.rfc-editor.org/rfc/rfc9113.html#section-8.2.2-2
    if (str != "te")
      throw stream_error(errc_e::PROTOCOL_ERROR, streamid,
                         std::format("connection specific header forbidden in HTTP/2, header name: {}", str));
    if (h.value.str() != "trailers")
      throw stream_error(
          errc_e::PROTOCOL_ERROR, streamid,
          std::format("TE header with value other than \"trailers\", actual value: {}", h.value.str()));
  }
}

void parse_http2_request_headers(hpack::decoder& d, std::span<hpack::byte_t const> bytes, http_request& req,
                                 stream_id_t streamid) {
  auto const* in = bytes.data();
  auto const* e = in + bytes.size();
  hpack::header_view header;

  // parse required pseudoheaders

  bool schemeParsed = false;
  bool pathParsed = false;
  bool methodParsed = false;
  bool authorityParsed = false;
  bool contenttypeParsed = false;
  while (in != e) {
    d.decode_header(in, e, header);
    if (!header)  // skip dynamic size updates
    {
      continue;
    }
    if (header.name == ":path") {
      if (pathParsed) {
        throw duplicated_pseudoheader(streamid, ":path");
      }
      pathParsed = true;
      req.path = header.value.str();
      if (req.path.empty()) {
        throw protocol_error(errc_e::PROTOCOL_ERROR, ":path header is empty");
      }
    } else if (header.name == ":method") {
      if (methodParsed) {
        throw duplicated_pseudoheader(streamid, ":method");
      }
      methodParsed = true;
      enum_from_string(header.value.str(), req.method);
    } else if (header.name == ":scheme") {
      if (schemeParsed) {
        throw duplicated_pseudoheader(streamid, ":scheme");
      }
      schemeParsed = true;
      enum_from_string(header.value.str(), req.scheme);
    } else if (header.name == ":authority") {
      if (authorityParsed) {
        throw duplicated_pseudoheader(streamid, ":authority");
      }
      authorityParsed = true;
      req.authority = header.value.str();
    } else if (header.name == "content-type") {
      if (contenttypeParsed) {
        throw stream_error(errc_e::PROTOCOL_ERROR, streamid, "\"content-type\" already parsed");
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
  push_header:
    validate_header_name(header, streamid);
    req.headers.push_back(http_header_t(std::string(header.name.str()), std::string(header.value.str())));
  }

  auto checkrequired = [](std::string_view hdrname, bool& parsed) {
    if (!parsed) [[unlikely]] {
      throw protocol_error(errc_e::PROTOCOL_ERROR, std::format("required header {} not present", hdrname));
    }
  };
  checkrequired(":path", pathParsed);
  checkrequired(":method", methodParsed);
  checkrequired(":scheme", schemeParsed);
  // authority not checked, since its possible to not receive authority (client not required to sent it)
}

}  // namespace http2
