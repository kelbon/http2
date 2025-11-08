#pragma once

#include <source_location>
#include <string_view>

#include <hpack/hpack.hpp>
#include <moko3/moko3.hpp>

#include <map>
#include <set>

#include <http2/http2_protocol.hpp>
#include <http2/utils/deadline.hpp>
#include <http2/http_body_bytes.hpp>
#include <http2/utils/unique_name.hpp>
#include <http2/asio/ssl_context.hpp>
#include <http2/asio/awaiters.hpp>
#include <http2/asio/factory.hpp>
#include <http2/http2_client.hpp>
#include <http2/http2_connection.hpp>
#include <http2/http2_server.hpp>
#include <http2/asio/asio_executor.hpp>
#include "fuzzer.hpp"

#include <kelcoro/task.hpp>

#include "servers/echo_server.hpp"

#define program_options_file "tests_cli.def"
#include <clinok/cli_interface.hpp>

using namespace std::chrono_literals;

namespace http2 {

inline constexpr auto DEFAULT_CONN_TIMEOUT = std::chrono::seconds(10);

struct h2frame {
  frame_header hdr = {};
  http_body_bytes data = {};

  // parses frame. First 9 bytes are header, all other - data
  static h2frame fromBytes(std::span<byte_t const> bytes) {
    h2frame f;
    f.hdr = frame_header::parse({bytes.data(), FRAME_HEADER_LEN});
    f.data.assign(bytes.begin() + FRAME_HEADER_LEN, bytes.end());
    return f;
  }
};

enum struct ping_e { RESPONSE = 1, ERROR = 2 };

enum struct window_e { RETURN = 0, SKIP = 1 };

using setting_id_e = settings_identifier_e;

struct header {
  std::string name;
  std::string value;
  bool indexed = true;

  bool operator==(const header& h) const noexcept {
    return name == h.name && value == h.value;
  }
  bool operator==(const http_header_t& h) const noexcept {
    return name == h.name() && value == h.value();
  }
};

inline deadline_t testdeadline(duration_t d) {
  // TODO if debugger preset - never
  return deadline_after(d);
}

// represents received HEADER + DATA frame
struct hdrs_and_data {
  uint32_t streamId = 0;   // streamid for both HEADERS and DATA frame
  bool endStream = false;  // if END_STREAM flag was setted
  std::vector<header> headers = {};
  http_body_bytes body = {};

  std::string_view findHdr(std::string_view name) {
    auto it = std::find_if(headers.begin(), headers.end(), [name](auto& v) { return v.name == name; });
    if (it != headers.end()) {
      return it->value;
    }

    return {};
  }

  bool isHdrIndexed(std::string_view name) {
    auto it = std::find_if(headers.begin(), headers.end(), [name](auto& v) { return v.name == name; });
    REQUIRE(it != headers.end());
    return it->indexed;
  }
};

inline http_body_bytes body_from_sv(std::string_view view) {
  byte_t const* b = (byte_t const*)view.data();
  return http_body_bytes(b, b + view.size());
}

struct test_h2connection {
 private:
  h2connection_ptr con = nullptr;
  bool is_client_con = false;
  // local SETTINGS_MAX_FRAME_SIZE
  uint32_t m_maxFrameSize = FRAME_LEN_MAX;
  // local SETTINGS_HEADER_TABLE_SIZE
  std::optional<uint32_t> m_headerTabSize;

 public:
  bool is_client() const noexcept {
    return is_client_con;
  }

  bool is_server() const noexcept {
    return !is_client_con;
  }

  test_h2connection() = default;
  test_h2connection(h2connection_ptr con, bool client) noexcept;

  test_h2connection(test_h2connection&& other) noexcept
      : con(std::exchange(other.con, nullptr)),
        is_client_con(other.is_client_con),
        m_maxFrameSize(other.m_maxFrameSize),
        m_headerTabSize(other.m_headerTabSize) {
  }
  test_h2connection& operator=(test_h2connection&& other) noexcept {
    std::destroy_at(this);
    std::construct_at(this, std::move(other));
    return *this;
  }
  ~test_h2connection() {
    close();
  }

  dd::task<void> receiveClientMagic(std::source_location = std::source_location::current());
  dd::task<void> sendClientMagic();

  // ignores frame correctness, allowing to send incorrect frames for tests
  dd::task<void> sendFrame(h2frame);
  dd::task<void> sendRawFrame(std::vector<byte_t>);
  // ignores any logic like control flow, window update, ping answer etc
  dd::task<h2frame> receiveFrame(deadline_t, std::source_location = std::source_location::current());

  dd::task<h2frame> nextFrame(deadline_t, ping_e, window_e = window_e::RETURN,
                              std::source_location = std::source_location::current());

  // handles SETTINGS_HEADER_TABLE_SIZE, sets m_encoder and m_decoder into correct state
  dd::task<void> receiveAndCheckSettings(std::map<setting_id_e, uint32_t> expected,
                                         std::set<setting_id_e> unexpected, deadline_t = deadline_after(5s),
                                         std::source_location = std::source_location::current());

  // gets and validates settings frame, returns them.
  // handles only SETTINGS_HEADER_TABLE_SIZE, sets m_encoder and m_decoder into correct state
  dd::task<h2frame> receiveSettings(deadline_t = deadline_after(5s),
                                    std::source_location = std::source_location::current());
  dd::task<void> receiveSettingsAck(deadline_t = deadline_after(5s),
                                    std::source_location = std::source_location::current());
  dd::task<void> sendSettingsAck();
  // sends default SETTINGS frame
  dd::task<void> sendSettings();

  // waits ping request (not ACK ping frame), ignores WINDOW_UPDATE
  dd::task<uint64_t> receivePing(deadline_t deadline = deadline_after(5s),
                                 std::source_location = std::source_location::current());
  // sends PING frame with request pong (ACK == false)
  dd::task<void> sendPing(uint64_t opaqueData);
  // sends PING frame answer (ACK == true)
  dd::task<void> sendPong(uint64_t opaqueData);

  dd::task<void> receiveGoAway(stream_id_t lastStreamId, errc_e errorCode, ping_e ping,
                               deadline_t deadline = deadline_after(5s),
                               std::source_location = std::source_location::current());
  dd::task<void> sendGoAway(stream_id_t lastStreamId, errc_e error, std::string_view debug = {});

  dd::task<void> receiveRstStream(stream_id_t streamId, errc_e, deadline_t = deadline_after(5s),
                                  std::source_location = std::source_location::current());
  dd::task<void> sendRstStream(stream_id_t streamId, errc_e);

  dd::task<void> sendHeaders(stream_id_t streamid, std::vector<header> headers, bool endstream);

  // sends HEADERS frame and if `body` present - DATA frame. Sends END_STREAM only if `endstream` == true
  dd::task<void> sendReq(stream_id_t streamid, std::vector<header> headers, http_body_bytes body = {},
                         bool endstream = true);
  // sends HEADERS frame and if `body` present - DATA frame. Sends END_STREAM only if `endstream` == true
  dd::task<void> sendRsp(stream_id_t streamid, std::vector<header> headers, http_body_bytes body = {},
                         bool endstream = true);

  // receives HEADERS frame and, if required, DATA frame.
  // returns streamid, if marked endStream, decoded headers, untouched body bytes
  dd::task<hdrs_and_data> receiveReq(deadline_t deadline = deadline_after(5s),
                                     std::source_location = std::source_location::current());
  // same as receiveReq, name different for better code readability
  dd::task<hdrs_and_data> receiveRsp(deadline_t deadline = deadline_after(5s),
                                     std::source_location loc = std::source_location::current()) {
    REQUIRE(is_client());
    return receiveReq(deadline, loc);
  }

  dd::task<void> sendData(stream_id_t streamId, std::string_view body, bool end_stream);
  // sends HEADERS frame with raw `headers` bytes, does not check `headers` correctness
  // if `split` is true headers will be splitted into random count of CONTINUATION frames
  dd::task<void> sendRawHdr(stream_id_t streamId, std::span<const byte_t> headers, bool end_stream = true,
                            bool split = false);
  dd::task<void> sendRawContinuation(stream_id_t streamId, std::span<byte_t> headers, bool end_headers);

  // caller must send header (or encoder dyntab will be invalid)
  std::vector<byte_t> encode_headers(std::vector<header> hdrs);

  // receives data, handles DATA padding etc.
  // Note: hdr.length may be not equal to data.size(). data.size() - actual data, hdr.length includes padding
  // for control flow
  dd::task<h2frame> receiveData(stream_id_t streamId, deadline_t deadline = deadline_after(5s));

  dd::task<void> sendWindowSizeIncrement(stream_id_t streamId, uint32_t windIncr);

  dd::task<void> waitConnectionDropped(deadline_t deadline = deadline_after(5s),
                                       std::source_location = std::source_location::current());

  void close();

  dd::task<void> recvFrame(deadline_t deadline = deadline_after(5s),
                           std::source_location = std::source_location::current());

  // makes sense only before sending SETTINGS frame, sets SETTINGS_MAX_FRAME_SIZE
  void setMaxFrameSize(uint32_t size) noexcept {
    m_maxFrameSize = size;
  }
  uint32_t getMaxFrameSize() const noexcept {
    return m_maxFrameSize;
  }

  // makes sense only before sending SETTINGS frame, sets SETTINGS_HEADER_TABLE_SIZE
  void setHeaderTableSize(uint32_t size) {
    con->decoder.dyntab.set_user_protocol_max_size(size);
    m_headerTabSize = size;
  }

  h2connection_ptr get_inner_connection() const noexcept {
    return con;
  }
};

// connects to `addr`, returns connection before http2 or tls handshake
inline dd::task<test_h2connection> fake_client_connection(
    asio::io_context& ctx, endpoint addr, bool tls,
    deadline_t deadline = deadline_after(DEFAULT_CONN_TIMEOUT),
    std::source_location = std::source_location::current()) {
  // connection do not attached to factory, so factory may be deleted after createConnection
  if (!tls) {
    asio_factory f(ctx);
    auto c = co_await f.createConnection(addr, deadline);
    co_return test_h2connection(new h2connection(std::move(c), ctx), /*client=*/true);
  } else {
    asio_tls_factory f(ctx);
    auto c = co_await f.createConnection(addr, deadline);
    co_return test_h2connection(new h2connection(std::move(c), ctx), /*client=*/true);
  }
}

inline internet_address localhost() noexcept {
  return internet_address(asio::ip::address_v4::loopback(), 0);
}

// precondition: `client` is not connected
// returns fake server
// connects `client` into fake server and returns BEFORE http2 connection establishment
// and AFTER tls handshake
inline dd::task<test_h2connection> fake_server_session(asio::io_context& ctx, server_endpoint addr,
                                                       http2_client& client,
                                                       ssl_context_ptr servertls = nullptr,
                                                       deadline_t deadline = deadline_after(10s)) {
  timer_t timer(ctx);
  timer.set_callback([] {
    std::cout << "fake server session cannot be established, deadline reached!" << std::endl;
    std::abort();
  });
  timer.arm(deadline.tp);
  asio::ip::tcp::acceptor a(ctx, addr.addr, addr.reuse_address);
  a.listen();
  asio::ip::tcp::socket socket(ctx);
  client.try_connect(a.local_endpoint(), deadline).start_and_detach();
  io_error_code ec;
  co_await net.accept(a, socket, ec);
  REQUIRE(!ec);
  if (servertls) {
    any_connection_t tcpcon(new asio_tls_connection(std::move(socket), servertls));
    co_await net.handshake(static_cast<asio_tls_connection*>(tcpcon.get())->sock,
                           asio::ssl::stream_base::server, ec);
    REQUIRE(!ec);
    h2connection_ptr con = new h2connection(std::move(tcpcon), ctx);
    co_return test_h2connection(std::move(con), /*client=*/false);
  } else {
    any_connection_t tcpcon(new asio_connection(std::move(socket)));
    h2connection_ptr con = new h2connection(std::move(tcpcon), ctx);
    co_return test_h2connection(std::move(con), /*client=*/false);
  }
}

inline dd::task<void> emulate_server_connection(test_h2connection& conn) {
  REQUIRE(conn.is_server());

  co_await conn.receiveClientMagic();

  (void)co_await conn.receiveSettings();
  co_await conn.sendSettings();

  co_await conn.receiveSettingsAck();
  co_await conn.sendSettingsAck();
}

inline dd::task<void> emulate_client_connection(test_h2connection& conn) {
  REQUIRE(conn.is_client());
  co_await conn.sendClientMagic();

  co_await conn.sendSettings();
  (void)co_await conn.receiveSettings();

  co_await conn.sendSettingsAck();
  co_await conn.receiveSettingsAck();
}

inline std::string source_location_msg_str(std::source_location loc) {
  return std::format("{}:{}:{}", loc.file_name(), loc.line(), loc.column());
}

inline void on_timeout_test_failure(std::source_location loc) {
  REQUIRE(false);
  // std::format("deadline expired in {}", source_location_msg_str(loc)));
}

// ioctx used only for yield
template <std::invocable PRED, typename ON_TIMEOUT = decltype(&on_timeout_test_failure)>
dd::task<void> wait_until(PRED pred, asio::io_context& ctx, deadline_t deadline = deadline_after(5s),
                          std::source_location loc = std::source_location::current(),
                          ON_TIMEOUT onTimeout = &on_timeout_test_failure) {
  for (;;) {
    if constexpr (dd::co_awaitable<std::invoke_result_t<PRED>>) {
      if (co_await pred())
        co_return;
    } else {
      if (pred())
        co_return;
    }
    if (deadline.isReached()) [[unlikely]] {
      onTimeout(loc);
      co_return;
    }
    co_await yield_on_ioctx(ctx);
  }
}

inline dd::job run_test(std::string_view testname, dd::task<void> test, bool& ended, std::exception_ptr& e) {
  on_scope_exit {
    ended = true;
  };
  try {
    co_await test;
  } catch (...) {
    e = std::current_exception();
  }
}

template <auto* Foo>
void server_test_impl(std::string_view name, moko3::top_lvl_section* toplvl_section) {
  echo_server server;
  internet_address addr(asio::ip::address_v4::loopback(), /*port_num=*/0);
  addr = server.listen({.addr = addr, .reuse_address = true});
  bool test_ended = false;
  std::exception_ptr ex;
  (void)run_test(name, Foo(server, addr, server.ioctx(), toplvl_section), test_ended, ex);
  deadline_t deadline = deadline_after(moko3::get_testbox().test_timeout(name));
  fuzzing::fuzzer fuz(moko3::get_testbox().randg());
  fuz.run_until(deadline, test_ended, server.ioctx());
  if (ex)
    std::rethrow_exception(std::move(ex));
}
// TODO tls?
template <auto* Foo>
void client_test_impl(std::string_view name, moko3::top_lvl_section* toplvl_section) {
  http2::http2_client client;
  bool test_ended = false;
  std::exception_ptr ex;
  (void)run_test(name, Foo(client, client.ioctx(), toplvl_section), test_ended, ex);
  deadline_t deadline = deadline_after(moko3::get_testbox().test_timeout(name));
  fuzzing::fuzzer fuz(moko3::get_testbox().randg());
  fuz.run_until(deadline, test_ended, client.ioctx());
  if (ex)
    std::rethrow_exception(std::move(ex));
}

#define UNIQUE_TEST_NAME LOGIC_GUARDS_CONCAT(_test, __LINE__, __LINE__)
// after this macro expected function scope, which will use `server`, `addr`, `ioctx`
// and return dd::task<void>
#define SERVER_TEST(NAME)                                                                                  \
  ::dd::task<void> UNIQUE_TEST_NAME(::http2::echo_server& server, ::http2::internet_address addr,          \
                                    ::boost::asio::io_context& ioctx, ::moko3::top_lvl_section* _section); \
  TEST(NAME) {                                                                                             \
    ::http2::server_test_impl<&UNIQUE_TEST_NAME>(NAME, _section);                                          \
  }                                                                                                        \
  ::dd::task<void> UNIQUE_TEST_NAME(::http2::echo_server& server, ::http2::internet_address addr,          \
                                    ::boost::asio::io_context& ioctx, ::moko3::top_lvl_section* _section)

// after this macro expected function scope, which will use `client`, `ioctx`
// and return dd::task<void>
#define CLIENT_TEST(NAME)                                                                            \
  ::dd::task<void> UNIQUE_TEST_NAME(::http2::http2_client& client, ::boost::asio::io_context& ioctx, \
                                    ::moko3::top_lvl_section* _section);                             \
  TEST(NAME) {                                                                                       \
    ::http2::client_test_impl<&UNIQUE_TEST_NAME>(NAME, _section);                                    \
  }                                                                                                  \
  ::dd::task<void> UNIQUE_TEST_NAME(::http2::http2_client& client, ::boost::asio::io_context& ioctx, \
                                    ::moko3::top_lvl_section* _section)

}  // namespace http2
