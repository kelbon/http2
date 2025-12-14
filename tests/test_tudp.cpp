
#include "http2/asio/asio_executor.hpp"
#include "http2/asio/tudp/tudp.hpp"

#include "http2/asio/awaiters.hpp"
#include "http2/asio/ssl_context.hpp"
#include "moko3/macros.hpp"

using namespace http2;
#if 0
inline bool ok = false;

dd::task<void> tudp_server(boost::asio::io_context& ctx) {
  auto sslctx =
      make_ssl_context_for_server(HTTP2_TLS_DIR "/test_server.crt", HTTP2_TLS_DIR "/test_server.key");
  tudp::tudp_acceptor acceptor(ctx,
                               // NOTE: not loopback! лупбек не эквивалентно этому и ломается
                               boost::asio::ip::udp::endpoint{boost::asio::ip::udp::v4(), 3334});
  acceptor.close();  // testing reuse after stop
  io_error_code ec;
  boost::asio::ssl::stream<tudp::tudp_server_socket> sock(ctx, sslctx->ctx);
  co_await net.accept(acceptor, sock.lowest_layer(), ec);
  REQUIRE(!ec);
  co_await net.handshake(sock, boost::asio::ssl::stream_base::handshake_type::server, ec);
  REQUIRE(!ec);
  char buf[100];
  size_t readen = co_await net.read_some(sock, {(byte_t*)buf, 100}, ec);
  REQUIRE(!ec);
  REQUIRE(std::string_view(buf, readen) == "hello world");
  ok = true;
}

dd::task<void> tudp_client(boost::asio::io_context& ctx) {
  auto sslctx = make_ssl_context_for_http2({});
  boost::asio::ssl::stream<tudp::tudp_client_socket> sock(ctx, sslctx->ctx);
  io_error_code ec;

  boost::asio::ip::udp::endpoint ep(http2::asio::ip::address_v4::loopback(), 3334);
  co_await sock.lowest_layer().connect(ep, ec);
  REQUIRE(!ec);
  co_await net.handshake(sock, boost::asio::ssl::stream_base::handshake_type::client, ec);
  REQUIRE(!ec);
  char bytes[] = "hello world";
  size_t written = co_await net.write(sock, {(byte_t*)+bytes, sizeof(bytes) - 1}, ec);
  REQUIRE(!ec);
  REQUIRE(written == sizeof(bytes) - 1);
}

inline bool ok2 = false;

// Note: используем уже порт 3335, а не 3334 как предыдущая часть теста, потому что линукс не даёт быстро
// пересоздать сокет (кажется что-то лежит в errqueue и это невозможно исправить нормально)
// на том же адресе+порте и начинает спамить connection_refused на стороне клиента (втф это
// udp сокет..)

dd::task<void> tudp_server_notls(boost::asio::io_context& ctx) {
  tudp::tudp_acceptor acceptor(ctx, boost::asio::ip::udp::endpoint{boost::asio::ip::udp::v4(), 3335});
  io_error_code ec;
  tudp::tudp_server_socket sock(ctx);

  co_await net.accept(acceptor, sock, ec);
  REQUIRE(!ec);
  char buf[100];
  size_t readen = co_await net.read_some(sock, {(byte_t*)buf, 100}, ec);
  REQUIRE(!ec);
  REQUIRE(std::string_view(buf, readen) == "hello world");
  ok2 = true;
}

dd::task<void> tudp_client_notls(boost::asio::io_context& ctx) {
  tudp::tudp_client_socket sock(ctx);
  io_error_code ec;

  boost::asio::ip::udp::endpoint ep(http2::asio::ip::address_v4::loopback(), 3335);
  co_await sock.connect(ep, ec);
  REQUIRE(!ec);
  char bytes[] = "hello world";
  size_t written = co_await net.write(sock, {(byte_t*)+bytes, sizeof(bytes) - 1}, ec);
  REQUIRE(!ec);
  REQUIRE(written == sizeof(bytes) - 1);
}
#endif
inline bool ok3 = false;

dd::task<void> tudp_accept_from1(boost::asio::io_context& ctx) {
  tudp::tudp_acceptor acceptor(ctx, boost::asio::ip::udp::endpoint{boost::asio::ip::udp::v4(), 3336});
  io_error_code ec;
  tudp::tudp_server_socket sock =
      co_await acceptor.accept_from({http2::asio::ip::address_v4::loopback(), 3337}, ec);
  REQUIRE(!ec);
  char buf[100];
  size_t readen = co_await net.read_some(sock, {(byte_t*)buf, 100}, ec);
  REQUIRE(!ec);
  REQUIRE(std::string_view(buf, readen) == "hello world");
  ok3 = true;
}

dd::task<void> tudp_accept_from2(boost::asio::io_context& ctx) {
  co_await net.sleep(ctx, std::chrono::milliseconds(500));
  tudp::tudp_acceptor acceptor(ctx, boost::asio::ip::udp::endpoint{boost::asio::ip::udp::v4(), 3337});
  io_error_code ec;
  tudp::tudp_server_socket sock =
      co_await acceptor.accept_from({http2::asio::ip::address_v4::loopback(), 3336}, ec);
  REQUIRE(!ec);
  char bytes[] = "hello world";
  size_t written = co_await net.write(sock, {(byte_t*)+bytes, sizeof(bytes) - 1}, ec);
  REQUIRE(!ec);
  REQUIRE(written == sizeof(bytes) - 1);
}

int main() {
  boost::asio::io_context ctx;
#if 0
  tudp_server(ctx).start_and_detach();
  tudp_client(ctx).start_and_detach();
  while (!ok) {
    ctx.poll();
  }
  tudp_server_notls(ctx).start_and_detach();
  tudp_client_notls(ctx).start_and_detach();
  while (!ok2) {
    ctx.poll();
  }
#endif

  tudp_accept_from1(ctx).start_and_detach();
  tudp_accept_from2(ctx).start_and_detach();
  while (!ok3) {
    ctx.poll();
  }
  ctx.poll();
  return 0;
}
