#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/transport_factory.hpp"

#include <boost/intrusive/slist.hpp>

#include <kelcoro/job.hpp>

namespace http2 {

using starter_t = move_only_fn<dd::task<void>(boost::asio::ip::tcp::socket&, deadline_t) const>;

namespace noexport {

struct single_writer_guarantee {
  // boost::asio запрещает более одного async_write одновременно
  // https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/async_write/overload1.html
  // поэтому для того чтобы send_ping не сломал большой запрос нужно создать очередь на отправку
  bi::slist<writer_node, bi::cache_last<true>> writersqueue;
  std::coroutine_handle<> writer;
  bool allow_write = true;

  [[nodiscard]] bool writer_done() const noexcept {
    // writer ожидает новой работы, но её никогда не будет
    return !allow_write && writersqueue.empty() && writer != nullptr;
  }

  void notify_writer() {
    if (writer)
      std::exchange(writer, nullptr).resume();
  }
};

}  // namespace noexport

struct asio_connection : connection_i {
  static constexpr size_t readen_capacity = (1 << 14) + 9;
  unsigned char readen[readen_capacity];
  unsigned char* readen_start = readen;
  unsigned char* readen_end = readen;
  asio::ip::tcp::socket sock;
  noexport::single_writer_guarantee writedata;

  explicit asio_connection(asio::ip::tcp::socket);

  bool try_read(std::span<byte_t> buf) noexcept override;
  void start_read(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) override;
  size_t try_write(std::span<const byte_t>, io_error_code&) noexcept override;
  void start_write(writer_node*) override;
  dd::task<void> shutdown() noexcept override;
  bool is_https() override {
    return false;
  }
};

struct asio_factory : transport_factory_i {
  asio::io_context& ioctx;
  tcp_connection_options options;
  // invoked after tcp handshake, may set socket options etc
  starter_t starter;

  explicit asio_factory(boost::asio::io_context&, tcp_connection_options = {}, starter_t = {});

  dd::task<any_connection_t> create_connection_client(endpoint, deadline_t) override;
  any_acceptor create_acceptor(internet_address, bool reuse_address) override;
};

struct asio_tls_connection : connection_i {
  static constexpr size_t readen_capacity = (1 << 14) + 9;
  unsigned char readen[readen_capacity];
  unsigned char* readen_start = readen;
  unsigned char* readen_end = readen;
  asio::ssl::stream<asio::ip::tcp::socket> sock;
  ssl_context_ptr sslctx;
  noexport::single_writer_guarantee writedata;

  // precondition: ctx != nullptr
  explicit asio_tls_connection(asio::ip::tcp::socket s, ssl_context_ptr ctx);

  bool try_read(std::span<byte_t>) noexcept override;
  void start_read(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) override;
  size_t try_write(std::span<const byte_t>, io_error_code&) noexcept override;
  void start_write(writer_node*) override;
  dd::task<void> shutdown() noexcept override;
  bool is_https() override {
    return true;
  }
};

struct asio_tls_factory : transport_factory_i {
  asio::io_context& ioctx;
  tcp_connection_options options;
  ssl_context_ptr sslctx;  // never null
  // invoked after tcp handshake (before TLS), may set socket options etc
  starter_t starter;

  // by default creates context for http2 client
  explicit asio_tls_factory(asio::io_context&, tcp_connection_options = {}, starter_t = {});
  // pre: ctx != nullptr
  asio_tls_factory(asio::io_context&, ssl_context_ptr ctx, tcp_connection_options = {}, starter_t = {});

  dd::task<any_connection_t> create_connection_client(endpoint, deadline_t) override;
  any_acceptor create_acceptor(internet_address, bool reuse_address) override;
};

}  // namespace http2
