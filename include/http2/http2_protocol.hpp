
#pragma once

#include "http2/http2_errors.hpp"
#include "http2/logger.hpp"
#include "http2/utils/memory.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>

#include <hpack/hpack.hpp>

/*

 All in this file based on RFC 9113 HTTP2
 https://datatracker.ietf.org/doc/html/rfc9113

*/
// windows)))
#undef NO_ERROR
namespace http2 {

struct http_request;

}

namespace http2 {

enum struct frame_e : uint8_t {
  DATA = 0x0,
  HEADERS = 0x1,
  PRIORITY = 0x2,
  RST_STREAM = 0x3,
  SETTINGS = 0x4,
  PUSH_PROMISE = 0x5,
  PING = 0x6,
  GOAWAY = 0x7,
  WINDOW_UPDATE = 0x8,
  CONTINUATION = 0x9,
  PRIORITY_UPDATE = 0x10,
};

std::string_view e2str(frame_e) noexcept;

// control flow size int type
// signed, because its possible to get negative control flow window size
// (e.g. sending DATA before settings exchange + receiving smaller connection
// window size than DATA size) int64 for easy handling overflow
using cfint_t = int64_t;

constexpr inline cfint_t INITIAL_WINDOW_SIZE_FOR_CONNECTION_OVERALL = uint32_t(65'535);
constexpr inline cfint_t MAX_WINDOW_SIZE = (uint32_t(1) << 31) - 1;  // 2'147'483'647
constexpr inline uint32_t FRAME_HEADER_LEN = uint32_t(9);
constexpr inline uint32_t FRAME_LEN_MAX = (uint32_t(1) << 24) - 1;
// https://www.rfc-editor.org/rfc/rfc9113.html#section-4.1-4.2.1
// "Values greater than 16,384 MUST NOT be sent unless the receiver
// has set a larger value for SETTINGS_MAX_FRAME_SIZE"
//
// this means value less than 16'384 makes no sense
constexpr inline uint32_t MIN_MAX_FRAME_LEN = 16'384;

using flags_t = uint8_t;

namespace flags {
// all flags of all frames
constexpr inline flags_t EMPTY_FLAGS = 0;
constexpr inline flags_t ACK = 0x01;          // SETTINGS, PING, means ping answer or settings accept
constexpr inline flags_t END_STREAM = 0x01;   // DATA, HEADERS
constexpr inline flags_t PADDED = 0x08;       // DATA, HEADERS, PUSH_PROMISE
constexpr inline flags_t PRIORITY = 0x20;     // HEADERS (deprecated)
constexpr inline flags_t END_HEADERS = 0x04;  // HEADERS, PUSH_PROMISE

}  // namespace flags

struct frame_header {
  // does not include frame itself
  uint32_t length = 0;
  frame_e type = frame_e(0);
  flags_t flags = flags::EMPTY_FLAGS;
  stream_id_t streamId = 0;

  template <std::output_iterator<hpack::byte_t> O>
  O form(O out) const {
    auto pushByte = [&](uint8_t byte) {
      *out = byte;
      ++out;
    };
    uint32_t len = htonl_value(length);
    out = std::copy_n(as_bytes(len).data() + 1, 3, out);
    pushByte(uint8_t(type));
    pushByte(flags);
    stream_id_t id = htonl_value(streamId);
    out = std::copy_n((uint8_t*)&id, sizeof(id), out);
    return out;
  }

  // precondition: raw_header.size() == FRAME_HEADER_LEN
  // not staticaly typed because of std::span ideal interface
  [[nodiscard]] static frame_header parse(std::span<hpack::byte_t const> rawheader) {
    assert(rawheader.size() == FRAME_HEADER_LEN);
    frame_header h;
    h.length = uint32_t(rawheader[0] << 16) | uint32_t(rawheader[1] << 8) | uint32_t(rawheader[2]);
    h.type = frame_e(rawheader[3]);
    h.flags = rawheader[4];
    memcpy(&h.streamId, rawheader.data() + 5, 4);
    htonli(h.streamId);
    // https://datatracker.ietf.org/doc/html/rfc9113#section-4.1-4.8.1
    // reserved bit must be ignored
    h.streamId &= stream_id_t(0x7FFFFFFF);  // (1u << 31) - 1
    return h;
  }

  bool operator==(frame_header const&) const = default;
};

// MUST be followed by a SETTINGS frame which MAY be empty
constexpr inline unsigned char CONNECTION_PREFACE[] = {
    0x50, 0x52, 0x49, 0x20, 0x2a, 0x20, 0x48, 0x54, 0x54, 0x50, 0x2f, 0x32,
    0x2e, 0x30, 0x0d, 0x0a, 0x0d, 0x0a, 0x53, 0x4d, 0x0d, 0x0a, 0x0d, 0x0a,
};

/*

struct data_frame
{
      <header>
      [Pad Length (8)],
      Data (..),
      Padding (..2040),

      client/server writer/reader form/decode this frame directly when required
};

// opens new stream
struct headers_frame
{
      <header>
      [Pad Length (8)],
      [Exclusive (1)],          //
      [Stream Dependency (31)], //
      [Weight (8)],             // all deprecated
      Field Block Fragment (..),
      Padding (..2040),

    do not have 'parse', its in http2_client (because requires decoder, padding
remove etc)

    client/server writer/reader form/decode this frame directly when required
};

// DEPRECATED
struct priority_frame
{
    ignored
};

struct push_promise_frame
{
    this frame always disabled by client settings
};

// extension
https://www.rfc-editor.org/rfc/rfc9218#name-the-priority_update-frame struct
priority_update_frame
{
    ignored
};

*/

// terminates stream
struct rst_stream {
  frame_header header;
  errc_e errorCode = errc_e::NO_ERROR;

  static constexpr inline size_t LEN = FRAME_HEADER_LEN + 4;

  template <std::output_iterator<byte_t> O>
  static O form(stream_id_t streamid, errc_e ec, O out) {
    assert(streamid != 0);
    frame_header header{
        .length = 4,
        .type = frame_e::RST_STREAM,
        .flags = flags::EMPTY_FLAGS,
        .streamId = streamid,
    };
    out = header.form(out);
    htonli(ec);
    return std::copy_n(as_bytes(ec).data(), 4, out);
  }

  [[nodiscard]] static rst_stream parse(frame_header h, std::span<byte_t const> bytes);
};

enum : uint16_t {
  SETTINGS_HEADER_TABLE_SIZE = 0x1,
  SETTINGS_ENABLE_PUSH = 0x2,
  SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
  SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
  SETTINGS_MAX_FRAME_SIZE = 0x5,
  SETTINGS_MAX_HEADER_LIST_SIZE = 0x6,
  // extension https://datatracker.ietf.org/doc/html/rfc9218
  SETTINGS_NO_RFC7540_PRIORITIES = 0x9,
};

struct settings_t {
  static constexpr inline uint32_t MAX_MAX_CONCURRENT_STREAMS = ((uint32_t(1) << 31) - 1);

  uint32_t headerTableSize = 4096;
  bool enablePush = false;
  uint32_t maxConcurrentStreams = MAX_MAX_CONCURRENT_STREAMS;
  // only for stream-level size!
  uint32_t initialStreamWindowSize = 65'535;
  uint32_t maxFrameSize = MIN_MAX_FRAME_LEN;
  uint32_t maxHeaderListSize = uint32_t(-1);
  // https://datatracker.ietf.org/doc/html/rfc9218
  bool deprecatedPriorityDisabled = false;
};

#pragma pack(push, 1)

struct [[gnu::packed]] setting_t {
  uint16_t identifier;
  uint32_t value;

  template <std::output_iterator<byte_t> O>
  static O form(setting_t s, O out) {
    s.identifier = htonl_value(s.identifier);
    s.value = htonl_value(s.value);
    return std::copy_n(as_bytes(s).data(), sizeof(s), out);
  }

  [[nodiscard]] static setting_t parse(std::span<byte_t const, 6> bytes) noexcept {
    setting_t s;
    memcpy(&s, bytes.data(), sizeof(s));
    s.identifier = htonl_value(s.identifier);
    s.value = htonl_value(s.value);
    return s;
  }
};
static_assert(sizeof(setting_t) == 6);

#pragma pack(pop)

// fills settings while parsing 'setting_t' one by one
// client side
struct server_settings_visitor {
  settings_t& settings;  // must be server settings
  bool firstframe = false;

  void operator()(setting_t);
};

// fills settings while parsing 'setting_t' one by one
// server side
struct client_settings_visitor {
  settings_t& settings;  // must be client settings
  bool firstframe = false;

  void operator()(setting_t);
};

struct settings_frame {
  frame_header header;

  /*
    <header>
    Setting (48) ...,

    Setting {
      Identifier (16),
      Value (32),
    }
  */

  template <std::output_iterator<hpack::byte_t> O>
  static O form(settings_t const& settings, O out) noexcept {
    static constexpr settings_t default_;

    // calculate len

    uint32_t len = 0;
    len += settings.headerTableSize != default_.headerTableSize;
    len += settings.enablePush != default_.enablePush;
    len += settings.maxConcurrentStreams != default_.maxConcurrentStreams;
    len += settings.initialStreamWindowSize != default_.initialStreamWindowSize;
    len += settings.maxFrameSize != default_.maxFrameSize;
    len += settings.maxHeaderListSize != default_.maxHeaderListSize;
    len *= sizeof(setting_t);

    // send frame header

    frame_header header{
        .length = len,
        .type = frame_e::SETTINGS,
        .flags = 0,
        .streamId = 0,  // connection related
    };
    out = header.form(out);

    // send settings VLA

    auto insert_setting = [&](setting_t s) { out = setting_t::form(s, out); };
#define PUSH_SETTING(NAME, ENUM_NAME) \
  if (settings.NAME != default_.NAME) \
  insert_setting({ENUM_NAME, settings.NAME})

    PUSH_SETTING(headerTableSize, SETTINGS_HEADER_TABLE_SIZE);
    PUSH_SETTING(enablePush, SETTINGS_ENABLE_PUSH);
    PUSH_SETTING(maxConcurrentStreams, SETTINGS_MAX_CONCURRENT_STREAMS);
    PUSH_SETTING(initialStreamWindowSize, SETTINGS_INITIAL_WINDOW_SIZE);
    PUSH_SETTING(maxFrameSize, SETTINGS_MAX_FRAME_SIZE);
    PUSH_SETTING(maxHeaderListSize, SETTINGS_MAX_HEADER_LIST_SIZE);
#undef PUSH_SETTING

    return out;
  }

  // ordering matters, must be handled in order they received
  static void parse(frame_header header, std::span<byte_t const> bytes,
                    auto&& settingVisitor)  //-V669 //-V2558
  {
    assert(header.type == frame_e::SETTINGS);
    if (header.flags & flags::ACK) {
      if (header.length != 0) {
        throw protocol_error{};
      }
      return;
    }
    if (header.streamId != 0 || (bytes.size() % sizeof(setting_t)) != 0) {
      HTTP2_LOG(ERROR,
                "invalid settings frame, streamid: {}, len: {}, provided data "
                "size: {}",
                header.streamId, header.length, bytes.size());
      throw protocol_error{};
    }
    setting_t s;
    for (auto b = bytes.begin(); b != bytes.end(); b += 6) {
      s = setting_t::parse(std::span<byte_t const, 6>{b, b + 6});
      settingVisitor(s);
    }
  }
};

// consists only of frame header
consteval frame_header accepted_settings_frame() noexcept {
  return frame_header{
      .length = 0,
      .type = frame_e::SETTINGS,
      .flags = flags::ACK,
      .streamId = 0,  // connection related
  };
}

// if ACK not setted, requires ping back
struct ping_frame {
  frame_header header;
  byte_t data[8] = {};

  [[nodiscard]] constexpr uint64_t getData() noexcept {
    return std::bit_cast<uint64_t>(data);
  }

  static constexpr inline size_t LEN = FRAME_HEADER_LEN + 8;

  template <std::output_iterator<byte_t> O>
  static O form(uint64_t data, bool requestAnswer, O out) {
    frame_header h{
        .length = 8,
        .type = frame_e::PING,
        .flags = requestAnswer ? flags_t(0) : flags::ACK,
        .streamId = 0,
    };
    out = h.form(out);
    return std::copy_n((char*)&data, 8, out);
  }

  [[nodiscard]] static ping_frame parse(frame_header h, std::span<byte_t const> bytes);
};

// initiates shutdown on connection.
struct goaway_frame {
  frame_header header;
  stream_id_t lastStreamId;
  errc_e errorCode;
  std::string debugInfo;
  /*
    <header>
    Reserved (1),
    Last-Stream-ID (31),
    Error Code (32),
    Additional Debug Data (..),
  */

  static goaway_frame parse(frame_header header, std::span<byte_t const> bytes);

  [[noreturn]] static void parseAndThrowGoaway(frame_header header, std::span<byte_t const> bytes) {
    goaway_frame f = parse(header, bytes);
    throw goaway_exception(f.lastStreamId, f.errorCode, std::move(f.debugInfo));
  }

  template <std::output_iterator<byte_t> O>
  static O form(stream_id_t lastStreamId, errc_e errorCode, std::string debugInfo, O out) {
    out =
        frame_header{
            .length = 8 + uint32_t(debugInfo.size()),
            .type = frame_e::GOAWAY,
            .flags = 0,
            .streamId = 0,
        }
            .form(out);
    htonli(lastStreamId);
    htonli(errorCode);
    out = std::copy_n(as_bytes(lastStreamId).data(), 4, out);
    out = std::copy_n(as_bytes(errorCode).data(), 4, out);
    return std::copy_n(debugInfo.data(), debugInfo.size(), out);
  }
};

// window size applicable only to DATA frames
struct window_update_frame {
  frame_header header;
  uint32_t windowSizeIncrement = 0;
  /*
    <header>
    Reserved (1),
    Window Size Increment (31),
  */
  static constexpr inline size_t LEN = FRAME_HEADER_LEN + 4;

  // id == 0 for connection-wide
  template <std::output_iterator<byte_t> O>
  static O form(stream_id_t id, uint32_t increment, O out) {
    assert(increment != 0);  // not valid by RFC
    out =
        frame_header{
            .length = 4,
            .type = frame_e::WINDOW_UPDATE,
            .flags = flags::EMPTY_FLAGS,
            .streamId = id,
        }
            .form(out);
    htonli(increment);
    return std::copy_n(as_bytes(increment).data(), sizeof(increment), out);
  }

  [[nodiscard]] static window_update_frame parse(frame_header header, std::span<byte_t const> bytes);
};

// TODO
struct continuation_frame {
  // not yet supported
};

template <std::output_iterator<hpack::byte_t> O>
static O form_connection_initiation(settings_t settings, O out) {
  out = std::copy_n(CONNECTION_PREFACE, sizeof(CONNECTION_PREFACE), out);
  return settings_frame::form(settings, out);
}

// used while handling window_update frames
// throws on protocol errors
// precondition: windowSizeIncrement >= 0 (but zero is protocol error)
inline void increment_window_size(cfint_t& size, int32_t windowSizeIncrement) {
  assert(windowSizeIncrement >= 0);
  if (windowSizeIncrement == 0) {
    HTTP2_LOG(ERROR, "invalid window size increment: zero");
    throw protocol_error{};
  }
  // avoid overflow
  if (int64_t(size) + int64_t(windowSizeIncrement) > int64_t(MAX_WINDOW_SIZE)) {
    HTTP2_LOG(ERROR,
              "invalid window size increment: overflow, current size: {}, "
              "increment: {}",
              uint64_t(size), uint64_t(windowSizeIncrement));
    throw protocol_error{};
  }
  size += windowSizeIncrement;
}

// used when i increase window size, so i can trust myself
inline void increment_window_size_trusted(cfint_t& size, int32_t windowSizeIncrement) noexcept {
  assert(windowSizeIncrement > 0);
  assert(int64_t(size) + int64_t(windowSizeIncrement) <= int64_t(MAX_WINDOW_SIZE));
  size += windowSizeIncrement;
}

// used when receiving or sending DATA frames
// precondition: decrease >= 0
inline void decrease_window_size(cfint_t& size, int32_t decrease) {
  assert(decrease >= 0);
  static_assert(sizeof(cfint_t) > 4);  // for avoiding overflow
  size -= decrease;
  if (size < 0) [[unlikely]] {
    HTTP2_LOG(WARN, "window size is < 0 ( {} ) after decreasing by {}", size, decrease);
  }
  // ignore control flow errors from out side, 'size' undeflow not possible
  // since its int64_t
}

// removes padding for DATA/HEADERS with PADDED flag
inline bool strip_padding(std::span<byte_t>& bytes) {
  if (bytes.empty()) {
    return false;
  }
  size_t padlen = bytes[0];
  if (padlen + 1 > bytes.size()) {
    return false;
  }
  remove_prefix(bytes, 1);
  remove_suffix(bytes, padlen);
  return true;
}

// разбирает все пришедшие хедера, обрабатывая некорретные значения :path,
// дублированные или пропущенные псевдохедеры
void parse_http2_request_headers(hpack::decoder& d, std::span<hpack::byte_t const> bytes, http_request& req);

}  // namespace http2
