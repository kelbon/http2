
#pragma once

#include "http2v2/errors.hpp"

#include <exception>
#include <string_view>

#include <fmt/core.h>
#include <hpack/basic_types.hpp>

namespace http2v2 {

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

struct protocol_error : std::exception {
  errc_e errc = errc_e::PROTOCOL_ERROR;
  std::string dbginfo;

  explicit protocol_error(errc_e merrc = errc_e::PROTOCOL_ERROR,
                          std::string mdbginfo = {}) noexcept
      : errc(merrc), dbginfo(std::move(mdbginfo)) {}

  char const *what() const noexcept override { return "http2 protocol error"; }
  std::string msg() const {
    return fmt::format("http2 protocol error, errc: {}, dbginfo: {}", (int)errc,
                       dbginfo);
  }
};

struct connection_error : protocol_error {
  explicit connection_error(errc_e e = errc_e::NO_ERROR) noexcept
      : protocol_error(e) {}
};

struct stream_error : protocol_error {
  stream_id_t streamid = stream_id_t(-1);

  explicit stream_error(stream_id_t id) noexcept : streamid(id) {}
};

struct rst_stream_received : std::exception {
  errc_e err = {};

  explicit rst_stream_received(errc_e e) noexcept : err(e) {}

  char const *what() const noexcept override {
    // assumes its null terminated
    return e2str(err).data();
  }
};

struct goaway_exception : std::exception {
  stream_id_t lastStreamId;
  errc_e errorCode;
  std::string debugInfo;

  goaway_exception(stream_id_t lastId, errc_e ec, std::string dbgInfo) noexcept
      : lastStreamId(lastId), errorCode(ec), debugInfo(std::move(dbgInfo)) {}

  char const *what() const noexcept override {
    return e2str(errorCode).data(); // assume null terminated
  }
};

} // namespace http2v2
