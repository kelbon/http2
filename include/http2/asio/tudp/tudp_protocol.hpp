#pragma once

#include <cstdint>
#include <http2/utils/memory.hpp>
#include <http2/http_body.hpp>
#include <http2/utils/fn_ref.hpp>
#include <http2/asio/awaiters.hpp>

#include <random>
#include <chrono>

#include <boost/asio/ip/udp.hpp>
#undef NO_ERROR

namespace tudp {

using http2::byte_t;
using http2::bytes_t;
using http2::deadline_after;
using http2::deadline_t;
using http2::duration_t;
using http2::endpoint;
using http2::io_error_code;
using http2::move_only_fn_soos;
using http2::net;

using udp = boost::asio::ip::udp;

enum struct tudp_datagram_type : byte_t {
  // первый бит всегда 1 для совместимости с STUN (у него первые 2 бита всегда 00)
  DATA = 0b1000'0000,
  ACK = 0b1000'0001,
  UNORDERED_DATA = 0b1000'0010,
  PING = 0b1000'0011,
  PONG = 0b1000'0100,
};
// TODO use hpack ints / strings, optimize datagrams

using cid_t = uint64_t;

struct tudp_data_datagram {
  cid_t scid = 0;
  cid_t dcid = 0;
  uint64_t packet_nmb = size_t(-1);
  std::span<const byte_t> payload;
};
struct tudp_ack_datagram {
  cid_t scid = 0;
  cid_t dcid = 0;
  // should be filled with 8-byte little endian integers - packet numbers
  // note: not span<uint64_t> because of alignment
  // if successfully parsed, its guaranteed, that payload contains 8*N count bytes
  std::span<const byte_t> payload;
};
struct tudp_unordered_data_datagram {
  cid_t scid = 0;
  cid_t dcid = 0;
  std::span<const byte_t> payload;
};

// -1 используется чтобы не ломать последовательность DATA фреймов
inline constexpr uint64_t TUDP_CONNECT_PACKET_NMB = uint64_t(-1);

inline std::array<byte_t, 17> unordered_data_prefix(cid_t scid, cid_t dcid) {
  std::array<byte_t, 17> r;
  r[0] = byte_t(tudp_datagram_type::UNORDERED_DATA);
  memcpy(r.data() + 1, &scid, sizeof(cid_t));
  memcpy(r.data() + 9, &dcid, sizeof(cid_t));
  return r;
}

inline bytes_t form_data_datagram(cid_t scid, cid_t dcid, uint64_t packet_nmb,
                                  std::span<const byte_t> payload) {
  static_assert(sizeof(scid) == 8 && sizeof(dcid) == 8 && sizeof(packet_nmb) == 8);
  static_assert(std::endian::native == std::endian::little);
  // TYPE[1], SCID[8], DCIT[8], PACKET_NUM[8], PAYLOAD_SIZE[2], PAYLOAD...
  // TYPE: DATA, ACK
  bytes_t bytes(1 /*type*/ + 8 * 3 /* scid, dcid, packet num*/ + 2 /*payload size*/ + payload.size());
  bytes[0] = byte_t(tudp_datagram_type::DATA);
  byte_t* pos = bytes.data() + 1;
  auto push_uint64 = [&](uint64_t v) {
    memcpy(pos, (void*)&v, 8);
    pos += 8;
  };
  push_uint64(scid);
  push_uint64(dcid);
  push_uint64(packet_nmb);
  assert(payload.size() <= std::numeric_limits<uint16_t>::max());
  uint16_t payload_sz = uint16_t(payload.size());
  memcpy(pos, (void*)&payload_sz, 2);
  pos += 2;
  memcpy(pos, payload.data(), payload.size());
  return bytes;
}

inline bytes_t form_ack_datagram(cid_t scid, cid_t dcid, uint64_t packet_nmb) {
  static_assert(sizeof(scid) == 8 && sizeof(dcid) == 8 && sizeof(packet_nmb) == 8);
  static_assert(std::endian::native == std::endian::little);
  // TYPE[1], SCID[8], DCIT[8], PACKETS_NUM[2], PACKET_NUM[8]
  bytes_t bytes(1 /*type*/ + 8 * 3 /*scid, dcid, packet num*/ + 2 /*packets_num*/);
  bytes[0] = byte_t(tudp_datagram_type::ACK);
  byte_t* pos = bytes.data() + 1;
  memcpy(pos, (void*)&scid, 8);
  pos += 8;
  memcpy(pos, (void*)&dcid, 8);
  pos += 8;
  uint16_t num = 1;  // one packet ack
  memcpy(pos, (void*)&num, 2);
  pos += 2;
  memcpy(pos, (void*)&packet_nmb, 8);
  pos += 8;

  return bytes;
}

inline std::array<byte_t, 17> form_ping_datagram(cid_t scid, cid_t dcid) {
  std::array<byte_t, 17> buf;
  buf[0] = byte_t(tudp_datagram_type::PING);
  memcpy(&buf[1], (void*)&scid, 8);
  memcpy(&buf[9], (void*)&dcid, 8);
  return buf;
}

inline std::array<byte_t, 17> form_pong_datagram(cid_t scid, cid_t dcid) {
  std::array<byte_t, 17> buf;
  buf[0] = byte_t(tudp_datagram_type::PONG);
  memcpy(&buf[1], (void*)&scid, 8);
  memcpy(&buf[9], (void*)&dcid, 8);
  return buf;
}

inline uint64_t take_uint64(std::span<const byte_t>& data) {
  uint64_t res;
  assert(data.size() >= 8);
  memcpy(&res, data.data(), 8);
  data = data.subspan(8);
  return res;
}

inline uint16_t take_uint16(std::span<const byte_t>& data) {
  uint16_t res;
  assert(data.size() >= 2);
  memcpy(&res, data.data(), 2);
  data = data.subspan(2);
  return res;
}

// returns false on error. `out.payload` refers to `data`, so it must stay alive
[[nodiscard]] inline bool parse_data_datagram(std::span<const byte_t> data, tudp_data_datagram& out) {
  static_assert(std::endian::native == std::endian::little);
  if (data.size() < 8 * 3 + 2) [[unlikely]]
    return false;
  out.scid = take_uint64(data);
  out.dcid = take_uint64(data);
  out.packet_nmb = take_uint64(data);
  uint16_t payload_size = take_uint16(data);

  if (out.scid == 0) [[unlikely]]
    return false;
  if (out.dcid == 0) [[unlikely]] {
    // Note: позволяем dcid == 0, потому что клиент мог не получить ACK во время коннекта к серверу
    // но тогда это должен быть connect запрос
    if (out.packet_nmb != TUDP_CONNECT_PACKET_NMB || !out.payload.empty())
      return false;
  }
  if (payload_size != data.size())
    return false;  // part of datagram / invalid datagram
  out.payload = data;
  return true;
}

[[nodiscard]] inline bool parse_ack_datagram(std::span<const byte_t> data, tudp_ack_datagram& out) {
  static_assert(std::endian::native == std::endian::little);
  // allow zero ACK packet numbers as ping
  if (data.size() < 8 * 2 + 2) [[unlikely]]
    return false;
  out.scid = take_uint64(data);
  out.dcid = take_uint64(data);
  uint16_t count = take_uint16(data);
  if (out.scid == 0 || out.dcid == 0 || count * 8 != data.size()) [[unlikely]]
    return false;
  out.payload = data;
  return true;
}

[[nodiscard]] inline bool parse_unordered_data_datagram(std::span<const byte_t> data,
                                                        tudp_unordered_data_datagram& out) {
  // первый байт уже распаршен
  if (data.size() < 2 * sizeof(cid_t)) [[unlikely]]
    return false;
  out.scid = take_uint64(data);
  out.dcid = take_uint64(data);
  out.payload = data;
  return true;
}

struct tudp_ping_datagram {
  cid_t scid = 0;
  cid_t dcid = 0;
};

inline bool parse_ping_datagram(std::span<const byte_t> data, tudp_ping_datagram& out) {
  if (data.size() != 16)
    return false;
  out.scid = take_uint64(data);
  out.dcid = take_uint64(data);
  return true;
}

struct tudp_pong_datagram {
  cid_t scid = 0;
  cid_t dcid = 0;
};

inline bool parse_pong_datagram(std::span<const byte_t> data, tudp_pong_datagram& out) {
  if (data.size() != 16)
    return false;
  out.scid = take_uint64(data);
  out.dcid = take_uint64(data);
  return true;
}

// never generates 0
inline size_t generate_connection_id() noexcept {
  size_t res;
  do {
    res = std::random_device{}();
  } while (res == 0);
  return res;
}

// пакет, который является частью потока
struct sent_packet {
  uint64_t n = size_t(-1);  // номер в последовательности
  bytes_t payload;          // включает служебные данные

  // mutable for storing in set, not key value
  // момент, когда последний раз этот пакет посылался
  mutable std::chrono::steady_clock::time_point when_sent;
  mutable size_t retries_done = 0;

  struct packet_hash {
    using is_transparent = int;

    size_t operator()(const sent_packet& p) const noexcept {
      return p.n;  // `n` should be unique, ideal hash
    }
    size_t operator()(size_t p) const noexcept {
      return p;
    }
  };
  struct packet_equal {
    using is_transparent = int;
    bool operator()(const sent_packet& a, const sent_packet& b) const noexcept {
      assert(a.n != b.n);
      return false;  // all packets are unique
    }
    bool operator()(const sent_packet& a, uint64_t b) const noexcept {
      return a.n == b;
    }
    bool operator()(uint64_t b, const sent_packet& a) const noexcept {
      return a.n == b;
    }
  };
};

static inline std::chrono::steady_clock::time_point timestamp() noexcept {
  return std::chrono::steady_clock::now();
}

[[nodiscard]] inline bool is_connect_request(tudp_data_datagram const& dg) noexcept {
  return dg.payload.size() == 0 /* connect запрос */
         && dg.dcid == 0        /* клиент ещё не знает номер dcid */
         && dg.scid != 0;
}

// UDP packet size max + запас под служебные байты
inline constexpr size_t TUDP_MAX_DATAGRAM_SIZE = 70'000;

inline void notify_reader(auto&& exe, auto reader, size_t readen) {
  assert(readen > 0);
  boost::asio::post(exe, [reader = std::move(reader), readen]() mutable { reader(io_error_code{}, readen); });
}

// visits packet, вызывает ОДНО ИЗ receive_data_dg, receive_ack_dg
// возвращает false, если пакет unparsable
[[nodiscard]] bool visit_packet(std::span<const byte_t> data, auto&& receive_data_dg, auto&& receive_ack_dg,
                                auto&& receive_unordered_data_dg, auto&& receive_ping_dg,
                                auto&& receive_pong_dg) noexcept {
  if (data.empty())
    return false;
  switch (tudp_datagram_type(data.front())) {
    case tudp_datagram_type::DATA: {
      tudp_data_datagram dg;
      if (parse_data_datagram(data.subspan(1), dg))
        return receive_data_dg(dg);
    } break;
    case tudp_datagram_type::ACK: {
      tudp_ack_datagram dg;
      if (parse_ack_datagram(data.subspan(1), dg))
        return receive_ack_dg(dg);
    } break;
    case tudp_datagram_type::UNORDERED_DATA: {
      tudp_unordered_data_datagram dg;
      if (parse_unordered_data_datagram(data.subspan(1), dg))
        return receive_unordered_data_dg(dg);
    } break;
    case tudp_datagram_type::PING: {
      tudp_ping_datagram dg;
      if (parse_ping_datagram(data.subspan(1), dg))
        return receive_ping_dg(dg);
    } break;
    case tudp_datagram_type::PONG: {
      tudp_pong_datagram dg;
      if (parse_pong_datagram(data.subspan(1), dg))
        return receive_pong_dg(dg);
    } break;
    default:
      return false;
  }
  return false;
}

}  // namespace tudp
