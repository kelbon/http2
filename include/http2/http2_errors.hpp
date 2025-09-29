
#pragma once

#include "http2/errors.hpp"

#include <cassert>
#include <exception>
#include <string_view>

#include <format>
#include <hpack/basic_types.hpp>

namespace http2 {

// 0 reserved for connection related
// odd for client
// even for server
// only increments during connection
// when limit reached, connection dropped
using stream_id_t = uint32_t;
constexpr inline stream_id_t MAX_STREAM_ID = (stream_id_t(1) << 31) - 1;

enum struct errc_e : uint32_t {
  NO_ERROR = 0x0,
  PROTOCOL_ERROR = 0x1,
  INTERNAL_ERROR = 0x2,
  FLOW_CONTROL_ERROR = 0x3,
  SETTINGS_TIMEOUT = 0x4,
  STREAM_CLOSED = 0x5,
  FRAME_SIZE_ERROR = 0x6,
  REFUSED_STREAM = 0x7,
  CANCEL = 0x8,
  COMPRESSION_ERROR = 0x9,
  CONNECT_ERROR = 0xa,
  ENHANCE_YOUR_CALM = 0xb,
  INADEQUATE_SECURITY = 0xc,
  HTTP_1_1_REQUIRED = 0xd,
};

constexpr std::string_view e2str(errc_e e) noexcept {
  switch (e) {
    case errc_e::NO_ERROR:
      return "NO_ERROR";
    case errc_e::PROTOCOL_ERROR:
      return "PROTOCOL_ERROR";
    case errc_e::INTERNAL_ERROR:
      return "INTERNAL_ERROR";
    case errc_e::FLOW_CONTROL_ERROR:
      return "FLOW_CONTROL_ERROR";
    case errc_e::SETTINGS_TIMEOUT:
      return "SETTINGS_TIMEOUT";
    case errc_e::STREAM_CLOSED:
      return "STREAM_CLOSED";
    case errc_e::FRAME_SIZE_ERROR:
      return "FRAME_SIZE_ERROR";
    case errc_e::REFUSED_STREAM:
      return "REFUSED_STREAM";
    case errc_e::CANCEL:
      return "CANCEL";
    case errc_e::COMPRESSION_ERROR:
      return "COMPRESSION_ERROR";
    case errc_e::CONNECT_ERROR:
      return "CONNECT_ERROR";
    case errc_e::ENHANCE_YOUR_CALM:
      return "ENHANCE_YOUR_CALM";
    case errc_e::INADEQUATE_SECURITY:
      return "INADEQUATE_SECURITY";
    case errc_e::HTTP_1_1_REQUIRED:
      return "HTTP_1_1_REQUIRED";
    default:
      return "UNKNOWN";
  }
}

// causes GOAWAY and connection close
struct protocol_error : std::exception {
  errc_e errc = errc_e::PROTOCOL_ERROR;
  std::string dbginfo;

  protocol_error() = default;
  explicit protocol_error(errc_e merrc) noexcept : errc(merrc) {
  }

  explicit protocol_error(errc_e merrc, std::string mdbginfo)
      : errc(merrc),
        dbginfo(std::format("HTTP/2 protocol error: errc: {}, dbginfo: \"{}\"", e2str(errc), mdbginfo)) {
  }

  char const* what() const noexcept override {
    return dbginfo.c_str();
  }
};

[[nodiscard]] inline protocol_error unimplemented_feature(std::string_view s) {
  return protocol_error(errc_e::INTERNAL_ERROR, std::format("UNSUPPORTED {}", s));
}

// causes RST_STREAM
struct stream_error : protocol_error {
  stream_id_t streamid;

  // precondition: streamid != 0
  stream_error(errc_e e, stream_id_t id, std::string msg) : protocol_error(e), streamid(id) {
    assert(streamid != 0);
    this->dbginfo =
        std::format("HTTP/2 stream error: errc: {}, dbginfo: \"{}\", streamid: {}", e2str(errc), msg, id);
  }
};

struct goaway_exception : std::exception {
  stream_id_t lastStreamId;
  errc_e errorCode;
  std::string debugInfo;
  std::string msg;

  goaway_exception(stream_id_t lastId, errc_e ec, std::string dbgInfo)
      : lastStreamId(lastId), errorCode(ec), debugInfo(std::move(dbgInfo)) {
    msg = std::format("errc: {}, debug info: \"{}\", last stream id: {}", e2str(errorCode), debugInfo,
                      lastStreamId);
  }

  char const* what() const noexcept override {
    return msg.c_str();
  }
};

}  // namespace http2
