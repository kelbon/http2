#include "http2/asio/tudp/stun.hpp"
#include "http2/http2_client.hpp"
#include "kelcoro/algorithm.hpp"

namespace tudp {

/*
https://datatracker.ietf.org/doc/html/rfc5389#page-11
(те кто писал rfc должны быть уволены, неужели нельзя всё это изложить на одной странице текста... Столько
бесполезных рассуждений прямо в rfc, единственное нормальное описание это собственно пример)

"For example, a Binding request has class=0b00 (request) and
method=0b000000000001 (Binding) and is encoded into the first 16 bits
as 0x0001.  A Binding response has class=0b10 (success response) and
method=0b000000000001, and is encoded into the first 16 bits as
0x0101."
*/
inline constexpr uint16_t STUN_BINDING_REQUEST = 0x0001;
inline constexpr uint16_t STUN_BINDING_RESPONSE = 0x0101;
inline constexpr uint16_t STUN_MAPPED_ADDRESS = 0x0001;
inline constexpr uint16_t STUN_XOR_MAPPED_ADDRESS = 0x0020;
inline constexpr uint32_t STUN_MAGIC_COOKIE = 0x2112A442U;

static uint16_t read_be16(const byte_t* p) {
  return static_cast<uint16_t>(p[0]) << 8 | static_cast<uint16_t>(p[1]);
}
static uint32_t read_be32(const byte_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | (static_cast<uint32_t>(p[3]));
}
static void write_be32(byte_t* p, uint32_t v) {
  p[0] = static_cast<byte_t>((v >> 24) & 0xFF);
  p[1] = static_cast<byte_t>((v >> 16) & 0xFF);
  p[2] = static_cast<byte_t>((v >> 8) & 0xFF);
  p[3] = static_cast<byte_t>(v & 0xFF);
}

// Generate random TID (12 bytes)
stun_tid_t make_random_stun_tid() noexcept {
  static thread_local std::minstd_rand gen(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 255);
  stun_tid_t tid;
  for (auto& b : tid)
    b = static_cast<byte_t>(dist(gen));
  return tid;
}

// Build minimal Binding Request (no attributes)
std::array<byte_t, 20> stun_build_binding_request(stun_tid_t tid) {
  std::array<byte_t, 20> req;
  // type: Binding Request
  req[0] = 0x00;
  req[1] = 0x01;
  // Length = 0
  req[2] = 0;
  req[3] = 0;
  // Magic cookie
  req[4] = 0x21;
  req[5] = 0x12;
  req[6] = 0xA4;
  req[7] = 0x42;
  // Transaction ID (12 bytes)
  std::memcpy(req.data() + 8, tid.data(), tid.size());
  return req;
}

// Parse MAPPED-ADDRESS (legacy). `data` is attribute value (attr_len bytes), without type/len header.
std::optional<udp::endpoint> parse_mapped_address_attr(std::span<const byte_t> data) {
  if (data.size() < 4)
    return std::nullopt;  // need at least family/port header
  // format: 0x00, family, port(2), addr...
  if (data[0] != 0x00)
    return std::nullopt;
  uint8_t family = data[1];
  uint16_t port = read_be16(data.data() + 2);

  if (family == 0x01) {  // IPv4
    if (data.size() < 8)
      return std::nullopt;
    boost::asio::ip::address_v4::bytes_type bytes{};
    std::memcpy(bytes.data(), data.data() + 4, 4);
    return udp::endpoint(boost::asio::ip::make_address_v4(bytes), port);
  } else if (family == 0x02) {  // IPv6
    if (data.size() < 20)
      return std::nullopt;
    boost::asio::ip::address_v6::bytes_type bytes{};
    std::memcpy(bytes.data(), data.data() + 4, 16);
    return udp::endpoint(boost::asio::ip::make_address_v6(bytes), port);
  }
  return std::nullopt;
}

// Parse XOR-MAPPED-ADDRESS attribute
std::optional<udp::endpoint> parse_xor_mapped_address_attr(std::span<const byte_t> data,
                                                           const stun_tid_t& tid) {
  if (data.size() < 4)
    return std::nullopt;
  if (data[0] != 0x00)
    return std::nullopt;
  uint8_t family = data[1];
  // read X-Port (network order), then XOR with high 16 bits of magic cookie
  uint16_t x_port = read_be16(data.data() + 2);
  uint16_t port = static_cast<uint16_t>(x_port ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16));

  if (family == 0x01) {  // IPv4
    if (data.size() < 8)
      return std::nullopt;
    // read 4 bytes (X-Address), XOR each byte with magic cookie bytes
    byte_t xor_magic[4];
    write_be32(xor_magic, STUN_MAGIC_COOKIE);  // magic cookie in network order bytes
    boost::asio::ip::address_v4::bytes_type addr_bytes{};
    for (size_t i = 0; i < 4; ++i) {
      addr_bytes[i] = data[4 + i] ^ xor_magic[i];
    }
    return udp::endpoint(boost::asio::ip::make_address_v4(addr_bytes), port);
  } else if (family == 0x02) {  // IPv6
    if (data.size() < 20)
      return std::nullopt;
    // XOR key = magic cookie (4 bytes) || TID (12 bytes)
    std::array<byte_t, 16> xor_key{};
    write_be32(xor_key.data(), STUN_MAGIC_COOKIE);
    std::memcpy(xor_key.data() + 4, tid.data(), 12);

    boost::asio::ip::address_v6::bytes_type addr_bytes{};
    for (size_t i = 0; i < 16; ++i) {
      addr_bytes[i] = data[4 + i] ^ xor_key[i];
    }
    return udp::endpoint(boost::asio::ip::make_address_v6(addr_bytes), port);
  }

  return std::nullopt;
}

std::optional<udp::endpoint> parse_stun_response(std::span<const byte_t> response, stun_tid_t tid) {
  if (response.size() < 20)
    return std::nullopt;

  uint16_t msg_type = read_be16(response.data());
  uint16_t msg_length = read_be16(response.data() + 2);
  // verify full message present
  if (response.size() < static_cast<size_t>(20 + msg_length))
    return std::nullopt;

  if (msg_type != STUN_BINDING_RESPONSE) {
    // not a Binding Success Response; ignore
    return std::nullopt;
  }

  // magic cookie
  uint32_t cookie = read_be32(response.data() + 4);
  if (cookie != STUN_MAGIC_COOKIE)
    return std::nullopt;

  // transaction id must match
  if (!std::equal(tid.begin(), tid.end(), response.begin() + 8))
    return std::nullopt;

  // iterate attributes within msg_length
  size_t offset = 20;
  size_t end = 20 + msg_length;
  while (offset + 4 <= end) {  // need at least type+len
    uint16_t attr_type = read_be16(response.data() + offset);
    uint16_t attr_len = read_be16(response.data() + offset + 2);
    offset += 4;
    if (offset + attr_len > end)
      break;  // malformed -> stop

    std::span<const byte_t> attr_value(response.data() + offset, attr_len);

    if (attr_type == STUN_XOR_MAPPED_ADDRESS) {
      auto ep = parse_xor_mapped_address_attr(attr_value, tid);
      if (ep)
        return ep;
    } else if (attr_type == STUN_MAPPED_ADDRESS) {
      auto ep = parse_mapped_address_attr(attr_value);
      if (ep)
        return ep;
    }

    // advance by padded length (4-byte boundary)
    size_t padded_len = (attr_len + 3) & ~static_cast<size_t>(3);
    offset += padded_len;
  }

  return std::nullopt;
}

std::optional<stun_tid_t> parse_stun_binding_request(std::span<const byte_t> packet) {
  if (packet.size() < 20)
    return std::nullopt;

  uint16_t msg_type = read_be16(packet.data());
  uint16_t msg_len = read_be16(packet.data() + 2);
  if (packet.size() < 20 + msg_len)
    return std::nullopt;

  if (msg_type != STUN_BINDING_REQUEST)
    return std::nullopt;

  uint32_t cookie = read_be32(packet.data() + 4);
  if (cookie != STUN_MAGIC_COOKIE)
    return std::nullopt;

  stun_tid_t tid{};
  std::memcpy(tid.data(), packet.data() + 8, 12);

  // аттрибуты не поддерживаются
  if (msg_len != 0)
    return std::nullopt;

  return tid;
}

std::vector<byte_t> stun_build_binding_success_response(stun_tid_t tid, udp::endpoint mapped_ep) {
  bool ipv6 = mapped_ep.address().is_v6();
  uint16_t attr_len = ipv6 ? 20 : 8;  // value length only

  uint16_t msg_len = 4 + attr_len;  // 4 = type+len
  std::vector<byte_t> buf(20 + msg_len);
  byte_t* p = buf.data();

  // MESSAGE TYPE
  p[0] = 0x01;
  p[1] = 0x01;  // 0x0101 Binding Success Response
  p[2] = static_cast<byte_t>(msg_len >> 8);
  p[3] = static_cast<byte_t>(msg_len & 0xFF);

  // Magic cookie
  write_be32(p + 4, STUN_MAGIC_COOKIE);

  // TID
  std::memcpy(p + 8, tid.data(), 12);

  // XOR-MAPPED-ADDRESS attribute
  size_t off = 20;
  p[off + 0] = 0x00;
  p[off + 1] = 0x20;  // XOR-MAPPED-ADDRESS
  p[off + 2] = static_cast<byte_t>(attr_len >> 8);
  p[off + 3] = static_cast<byte_t>(attr_len & 0xFF);

  byte_t* v = p + off + 4;
  v[0] = 0x00;
  v[1] = ipv6 ? 0x02 : 0x01;

  uint16_t x_port = mapped_ep.port() ^ static_cast<uint16_t>(STUN_MAGIC_COOKIE >> 16);
  v[2] = static_cast<byte_t>(x_port >> 8);
  v[3] = static_cast<byte_t>(x_port & 0xFF);

  if (!ipv6) {
    byte_t magic[4];
    write_be32(magic, STUN_MAGIC_COOKIE);

    auto bytes = mapped_ep.address().to_v4().to_bytes();
    for (int i = 0; i < 4; i++)
      v[4 + i] = bytes[i] ^ magic[i];

  } else {
    std::array<byte_t, 16> key{};
    write_be32(key.data(), STUN_MAGIC_COOKIE);
    std::memcpy(key.data() + 4, tid.data(), 12);

    auto bytes = mapped_ep.address().to_v6().to_bytes();
    for (int i = 0; i < 16; i++)
      v[4 + i] = bytes[i] ^ key[i];
  }

  return buf;
}

static std::optional<endpoint> parse_host_port_pair(std::string_view str) noexcept {
  auto pos = str.rfind(':');
  if (pos == str.npos || pos == 0)
    return std::nullopt;
  std::string_view hoststr = str.substr(0, pos);
  std::string_view portstr = str.substr(pos + 1);
  if (hoststr.empty() || portstr.empty())
    return std::nullopt;
  uint16_t port;
  auto [ptr, err] = std::from_chars(portstr.data(), portstr.data() + portstr.size(), port);
  if (ptr != str.data() + str.size() || err != std::errc{})
    return std::nullopt;
  return endpoint(std::string(hoststr), port);
}

// парсит список из файла valid_hosts.txt скачанного с гитхаба
// список выглядит примерно так:
// stun.abcd.net:3478
// stun.ddea.es:3478
// stun.fdsfsd.com:3478
static std::vector<endpoint> parse_valid_hosts_list(std::string_view input) {
  std::vector<endpoint> result;
  while (!input.empty()) {
    auto pos = input.find('\n');
    std::string_view hostport_pair = input.substr(0, pos);
    std::optional endpoint = parse_host_port_pair(hostport_pair);
    if (endpoint)  // ignore unparsable
      result.push_back(*endpoint);
    if (pos == input.npos)
      break;
    input.remove_prefix(pos + 1 /*\n*/);
    while (!input.empty() && input.starts_with('\n'))
      input.remove_prefix(1);
  }
  return result;
}

static dd::task<void> do_get_valid_stun_servers_list(http2::http2_client& client, std::string& out) try {
  // GET на файл с ежечаснообновляемыми валидными STUN серверами (RAW)
  // https://raw.githubusercontent.com/pradt2/always-online-stun/master/valid_hosts.txt
  http2::http_request req{.path = "/pradt2/always-online-stun/master/valid_hosts.txt",
                          .method = http2::http_method_e::GET};
  req.authority = "raw.githubusercontent.com";
  http2::http_response rsp = co_await client.send_request(req, deadline_after(std::chrono::seconds(5)));
  out = rsp.body_strview();
} catch (...) {
}

std::vector<endpoint> get_valid_stun_servers_list() {
  http2::http2_client cli(
      endpoint{"raw.githubusercontent.com", 443},
      http2::http2_client_options{.allow_requests_before_server_settings = true},
      [](boost::asio::io_context& ctx) { return http2::default_tls_transport_factory(ctx); });
  std::string html_stun_list;
  dd::chain(do_get_valid_stun_servers_list(cli, html_stun_list), cli.graceful_stop()).start_and_detach();
  cli.ioctx().run();
  return parse_valid_hosts_list(html_stun_list);
}

dd::task<std::optional<udp::endpoint>> stun_request(tudp_acceptor& acceptor,
                                                    move_only_fn_soos<endpoint()> get_stun_server_addr,
                                                    deadline_t deadline) try {
  assert(get_stun_server_addr);
  udp::resolver resolver(acceptor.get_executor());
  io_error_code ec;

  boost::asio::steady_timer timer(acceptor.get_executor());
  std::optional<udp::endpoint> result;
  // раз в 100ms отправляем запрос пока не получится или пока не дедлайн
  // или пока не кинется исключение, например потому что серверы кончились
  for (;;) {
    auto endpoints = co_await net.resolve(resolver, get_stun_server_addr(), ec);
    if (ec || endpoints.empty()) {
      if (!deadline.isReached())
        continue;
      else
        co_return std::nullopt;
    }

    timer.expires_at(
        std::min(deadline.tp, std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
    timer.async_wait([&](const io_error_code& ec) {
      if (ec)
        return;
      acceptor.cancel_stun_request();
    });

    co_await dd::suspend_and_t{[&](std::coroutine_handle<> me) {
      acceptor.async_stun_request(*endpoints.begin(), [&, me](std::optional<udp::endpoint> p) {
        result = p;
        me.resume();
      });
    }};
    timer.cancel();
    if (result)
      co_return result;
    if (deadline.isReached())
      co_return std::nullopt;
  }
  http2::unreachable();
} catch (...) {
  co_return std::nullopt;
}

dd::job maintain_nat_hole(tudp_acceptor& acceptor, duration_t period,
                          move_only_fn_soos<endpoint()> get_stun_server_addr) try {
  assert(get_stun_server_addr);
  std::weak_ptr guard = acceptor.weak_impl();
  assert(guard.lock());
  assert(period.count() > 0);

  udp::resolver resolver(acceptor.get_executor());
  boost::asio::steady_timer timer(acceptor.get_executor());
  io_error_code ec;
  for (;;) {
    std::shared_ptr a = guard.lock();
    if (!a)
      co_return;
    // поддержание имеет смысл только пока нет соединений. Соединения пингуют сами
    if (acceptor.active_connections_count() == 0) {
      auto endpoints = co_await net.resolve(resolver, get_stun_server_addr(), ec);
      if (ec || endpoints.empty())
        continue;
      acceptor.async_stun_request(
          *endpoints.begin(), [](std::optional<udp::endpoint>) {}, /*force_rehash=*/true);
      acceptor.cancel_stun_request();  // игнорируем результат, просто отправляем запрос
    }
    co_await net.sleep(timer, period, ec);
    (void)ec;
  }
  http2::unreachable();
} catch (std::exception& e) {
  HTTP2_LOG_ERROR("maintaining NAT hole failed with error: {}", e.what());
}

}  // namespace tudp
