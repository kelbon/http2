#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/transport_factory.hpp"

namespace http2 {

struct asio_connection : connection_i {
  asio::ip::tcp::socket sock;

  explicit asio_connection(asio::ip::tcp::socket s) : sock(std::move(s)) {
  }

  void startRead(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) override;
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
  dd::task<any_connection_t> createConnection(endpoint_t, deadline_t);
};

struct asio_tls_connection : connection_i {
  asio::ssl::stream<asio::ip::tcp::socket> sock;
  ssl_context_ptr sslctx;

  // precondition: ctx != nullptr
  explicit asio_tls_connection(asio::ip::tcp::socket s, ssl_context_ptr ctx)
      // Note: order
      : sock(std::move(s), ctx->ctx), sslctx(std::move(ctx)) {
  }

  void startRead(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) override;
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
  dd::task<any_connection_t> createConnection(endpoint_t, deadline_t) override;
};

}  // namespace http2
