
#pragma once

#include "hidi/asio/ssl_context.hpp"
#include "hidi/utils/any_io_context.hpp"
#include "hidi/tcp_connection_options.hpp"

namespace hidi {

// creates 'ref' version (do not own 'ctx')
any_io_context make_asio_io_context(asio::io_context& ctx, tcp_connection_options = {});

any_io_context make_asio_io_context(tcp_connection_options = {});

// if ssl == nullptr creates non ssl version
// creates 'ref' version (do not own 'ctx')
any_io_context make_asio_tls_io_context(asio::io_context& ctx, client_ssl_context_ptr ssl,
                                        tcp_connection_options = {});
// if ssl == nullptr creates non ssl version
// creates 'ref' version (do not own 'ctx')
any_io_context make_asio_tls_io_context(asio::io_context& ctx, server_ssl_context_ptr ssl,
                                        tcp_connection_options = {});

// if ssl == nullptr creates non ssl version
any_io_context make_asio_tls_io_context(client_ssl_context_ptr ssl, tcp_connection_options = {});
any_io_context make_asio_tls_io_context(server_ssl_context_ptr ssl, tcp_connection_options = {});

}  // namespace hidi
