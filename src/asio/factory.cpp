#include "http2/asio/factory.hpp"
#include "http2/asio/awaiters.hpp"
#include "http2/logger.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

namespace http2 {

void asio_tls_connection::startRead(std::coroutine_handle<> h, std::span<byte_t> buf, io_error_code& ec) {
  asio::async_read(sock, asio::buffer(buf.data(), buf.size()), [&, h](const io_error_code& e, size_t) {
    ec = e;
    h.resume();
  });
}

void asio_tls_connection::startWrite(std::coroutine_handle<> h, std::span<byte_t const> buf,
                                     io_error_code& ec) {
  asio::async_write(sock, asio::buffer(buf.data(), buf.size()), [&, h](const io_error_code& e, size_t w) {
    ec = e;
    h.resume();
  });
}

static void close_tcp_sock(auto& tcp_sock) {
  if (!tcp_sock.is_open())
    return;
  io_error_code ec;
  ec = tcp_sock.cancel(ec);
  // dont stop on errors, i need to stop connection somehow
  if (ec)
    HTTP2_LOG_ERROR("[HTTP2] [shutdown] TCP socket cancel, err {}", ec.what());
  // Do not do SSL shutdown, useless errors and wasting time
  ec = tcp_sock.shutdown(asio::socket_base::shutdown_both, ec);
  if (ec)
    HTTP2_LOG_ERROR("[HTTP2] [shutdown] TCP socket shutdown, err: {}", ec.what());
  ec = tcp_sock.close(ec);
  if (ec)
    HTTP2_LOG_ERROR("[HTTP2] [shutdown], TCP socket close err: {}", ec.what());
}

void asio_tls_connection::shutdown() noexcept {
  auto& tcp_sock = sock.lowest_layer();
  close_tcp_sock(tcp_sock);
}

void asio_connection::startRead(std::coroutine_handle<> h, std::span<byte_t> buf, io_error_code& ec) {
  asio::async_read(sock, asio::buffer(buf.data(), buf.size()), [&, h](const io_error_code& e, size_t) {
    if (e) [[unlikely]]
      ec = e;
    h.resume();
  });
}

void asio_connection::startWrite(std::coroutine_handle<> h, std::span<const byte_t> buf, io_error_code& ec) {
  asio::async_write(sock, asio::buffer(buf.data(), buf.size()), [&, h](const io_error_code& e, size_t) {
    if (e) [[unlikely]]
      ec = e;
    h.resume();
  });
}

void asio_connection::shutdown() noexcept {
  close_tcp_sock(sock);
}

any_transport_factory default_transport_factory(boost::asio::io_context& ctx) {
  return any_transport_factory(new asio_factory(ctx, {}));
}

any_transport_factory default_tls_transport_factory(boost::asio::io_context& ctx,
                                                    std::vector<std::filesystem::path> certs) {
  tcp_connection_options options;
  options.additional_ssl_certificates = std::move(certs);
  return any_transport_factory(new asio_tls_factory(ctx, std::move(options)));
}

asio_factory::asio_factory(boost::asio::io_context& ctx, tcp_connection_options opts)
    : ioctx(ctx), options(std::move(opts)) {
}

dd::task<any_connection_t> asio_factory::createConnection(endpoint_t endpoint, deadline_t deadline) {
  using tcp = asio::ip::tcp;

  tcp::resolver resolver(ioctx);

  asio::steady_timer timer(ioctx);
  bool timeoutflag = false;

  timer.expires_at(deadline.tp);
  timer.async_wait([&resolver, &timeoutflag](const io_error_code& ec) {
    if (ec != asio::error::operation_aborted) {
      timeoutflag = true;
      resolver.cancel();
    }
  });

  io_error_code ec;
  auto results = co_await net.resolve(resolver, endpoint, ec);
  if (timeoutflag)
    throw timeout_exception();

  if (results.empty() || ec) {
    HTTP2_LOG_ERROR("[TCP] cannot resolve host: {}, err: {}", endpoint.address().to_string(), ec.message());
    throw network_exception(ec);
  }
  tcp::socket tcp_sock(ioctx);

  timer.cancel();
  timer.expires_at(deadline.tp);
  timer.async_wait([&tcp_sock, &timeoutflag](const io_error_code& ec) {
    if (ec != asio::error::operation_aborted) {
      timeoutflag = true;
      close_tcp_sock(tcp_sock);
    }
  });

  co_await net.connect(tcp_sock, results, ec);

  if (ec) {
    HTTP2_LOG_ERROR("[TCP] cannot connect to {}, err: {}", endpoint.address().to_string(), ec.message());
    throw network_exception(ec);
  }
  options.apply(tcp_sock);
  co_return any_connection_t(new asio_connection(std::move(tcp_sock)));
}

asio_tls_factory::asio_tls_factory(asio::io_context& ioctx, tcp_connection_options opts)
    : ioctx(ioctx),
      options(std::move(opts)),
      sslctx(make_ssl_context_for_http2(options.additional_ssl_certificates)) {
}

dd::task<any_connection_t> asio_tls_factory::createConnection(endpoint_t endpoint, deadline_t deadline) {
  namespace ssl = asio::ssl;
  using tcp = asio::ip::tcp;

  tcp::resolver resolver(ioctx);

  asio::steady_timer timer(ioctx);
  bool timeoutflag = false;

  timer.expires_at(deadline.tp);
  timer.async_wait([&resolver, &timeoutflag](const io_error_code& ec) {
    if (ec != asio::error::operation_aborted) {
      timeoutflag = true;
      resolver.cancel();
    }
  });

  io_error_code ec;
  auto results = co_await net.resolve(resolver, endpoint, ec);
  if (timeoutflag)
    throw timeout_exception();
  if (results.empty() || ec) {
    HTTP2_LOG_ERROR("[TCP] cannot resolve host: {}, err: {}", endpoint.address().to_string(), ec.what());
    throw network_exception(ec);
  }
  asio::ip::tcp::socket tcp_sock(ioctx);

  timer.cancel();
  timer.expires_at(deadline.tp);
  timer.async_wait([&tcp_sock, &timeoutflag](const io_error_code& ec) {
    if (ec != asio::error::operation_aborted) {
      timeoutflag = true;
      close_tcp_sock(tcp_sock);
    }
  });

  co_await net.connect(tcp_sock, std::move(results), ec);

  if (timeoutflag)
    throw timeout_exception();
  if (ec) {
    HTTP2_LOG_ERROR("[TCP] cannot connect to {}, err: {}", endpoint.address().to_string(), ec.message());
    throw network_exception(ec);
  }
  options.apply(tcp_sock);
  assert(sslctx);
  std::unique_ptr<asio_tls_connection> res(new asio_tls_connection(std::move(tcp_sock), sslctx));
  if (options.host_for_name_verification) {
    res->sock.set_verify_mode(ssl::verify_peer);
    res->sock.set_verify_callback(asio::ssl::host_name_verification(*options.host_for_name_verification));
  } else {
    res->sock.set_verify_mode(ssl::verify_none);
  }
  if (!options.is_primal_connection)
    SSL_set_mode(res->sock.native_handle(), SSL_MODE_RELEASE_BUFFERS);
  co_await net.handshake(res->sock, ssl::stream_base::handshake_type::client, ec);
  if (timeoutflag)
    throw timeout_exception();
  if (ec) {
    HTTP2_LOG_ERROR("[TCP/SSL] cannot ssl handshake: {}", ec.message());
    throw network_exception(ec);
  }
  co_return any_connection_t(std::move(res));
}

}  // namespace http2
