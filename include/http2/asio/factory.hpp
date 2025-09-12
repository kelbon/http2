#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/transport_factory.hpp"

namespace http2 {

struct asio_connection : connection_i {
  static constexpr size_t readen_capacity = (1 << 14) + 9;
  unsigned char readen[readen_capacity];
  unsigned char* readen_start = readen;
  unsigned char* readen_end = readen;
  asio::ip::tcp::socket sock;

  explicit asio_connection(asio::ip::tcp::socket s) : sock(std::move(s)) {
    sock.non_blocking(true);
  }

  bool tryRead(std::span<byte_t> buf) noexcept override;
  void startRead(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) override;
  size_t tryWrite(std::span<const byte_t>, io_error_code&) noexcept override;
  void startWrite(std::coroutine_handle<> callback, std::span<byte_t const> buf, io_error_code& ec) override;
  void shutdown() noexcept override;
  bool isHttps() override {
    return false;
  }
};

struct asio_factory : transport_factory_i {
  asio::io_context& ioctx;
  tcp_connection_options options;

  explicit asio_factory(boost::asio::io_context&, tcp_connection_options = {});
  dd::task<any_connection_t> createConnection(endpoint, deadline_t);
};

struct asio_tls_connection : connection_i {
  static constexpr size_t readen_capacity = (1 << 14) + 9;
  unsigned char readen[readen_capacity];
  unsigned char* readen_start = readen;
  unsigned char* readen_end = readen;
  asio::ssl::stream<asio::ip::tcp::socket> sock;
  ssl_context_ptr sslctx;

  // precondition: ctx != nullptr
  explicit asio_tls_connection(asio::ip::tcp::socket s, ssl_context_ptr ctx)
      // Note: order
      : sock(std::move(s), ctx->ctx), sslctx(std::move(ctx)) {
    sock.lowest_layer().non_blocking(true);
  }

  bool tryRead(std::span<byte_t>) noexcept override;
  void startRead(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) override;
  size_t tryWrite(std::span<const byte_t>, io_error_code&) noexcept override;
  void startWrite(std::coroutine_handle<> callback, std::span<byte_t const> buf, io_error_code& ec) override;
  void shutdown() noexcept override;
  bool isHttps() override {
    return true;
  }
};

struct asio_tls_factory : transport_factory_i {
  asio::io_context& ioctx;
  tcp_connection_options options;
  ssl_context_ptr sslctx;  // never null

  explicit asio_tls_factory(asio::io_context&, tcp_connection_options = {});
  dd::task<any_connection_t> createConnection(endpoint, deadline_t) override;
};

}  // namespace http2
