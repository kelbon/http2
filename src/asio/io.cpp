#include "hidi/asio/io.hpp"

#include "hidi/asio/io_impl.hpp"

namespace hidi {

any_io_context make_asio_io_context(asio::io_context& ctx, tcp_connection_options opts) {
  return any_io_context(aa::inplaced{[&] { return asio_ref_io(ctx, std::move(opts)); }});
}

any_io_context make_asio_io_context(tcp_connection_options opts) {
  return any_io_context(aa::inplaced{[&] { return asio_io(std::move(opts)); }});
}

any_io_context make_asio_tls_io_context(asio::io_context& ctx, client_ssl_context_ptr ssl,
                                        tcp_connection_options opts) {
  if (ssl) {
    return any_io_context(
        aa::inplaced{[&] { return asio_tls_ref_io(ctx, std::move(ssl), std::move(opts)); }});
  } else
    return make_asio_io_context(ctx, std::move(opts));
}

any_io_context make_asio_tls_io_context(asio::io_context& ctx, server_ssl_context_ptr ssl,
                                        tcp_connection_options opts) {
  if (ssl) {
    return any_io_context(
        aa::inplaced{[&] { return asio_tls_ref_io(ctx, std::move(ssl), std::move(opts)); }});
  } else
    return make_asio_io_context(ctx, std::move(opts));
}

any_io_context make_asio_tls_io_context(client_ssl_context_ptr ssl, tcp_connection_options opts) {
  if (ssl)
    return any_io_context(aa::inplaced{[&] { return asio_tls_io(std::move(ssl), std::move(opts)); }});
  else
    return make_asio_io_context(std::move(opts));
}

any_io_context make_asio_tls_io_context(server_ssl_context_ptr ssl, tcp_connection_options opts) {
  if (ssl)
    return any_io_context(aa::inplaced{[&] { return asio_tls_io(std::move(ssl), std::move(opts)); }});
  else
    return make_asio_io_context(std::move(opts));
}

}  // namespace hidi
