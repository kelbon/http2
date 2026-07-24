
#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/utils/any_io_context.hpp"

#include <boost/intrusive/slist_hook.hpp>

#include <filesystem>
#include <optional>

#include <kelcoro/task.hpp>

#include <anyany/anyany.hpp>
#include <anyany/anyany_macro.hpp>

namespace http2 {

namespace asio = boost::asio;

struct tcp_connection_options {
  uint32_t send_buffer_size = 1024 * 1024 * 4;     // 4 MB
  uint32_t receive_buffer_size = 1024 * 1024 * 4;  // 4 MB
  std::vector<std::filesystem::path> additional_ssl_certificates;
  // adds delay (waiting for new requests to merge them)
  bool merge_small_requests = false;
  bool is_primal_connection = true;
  /*
    if unset, SSL host name verification disabled.
    On windows it (likely) will produce errors until you set
    'additional_ssl_certificates'

    if you are receiving error with ssl hanfshake,
    add verify path for your certificate, specially on windows, where default path may be unreachable
    you can download default cerifiers here: (https://curl.se/docs/caextract.html)
  */
  std::optional<std::string> host_for_name_verification = std::nullopt;

  template <typename E>
  void apply(asio::basic_socket<asio::ip::tcp, E>& tcp_sock) try {
    using tcp = asio::ip::tcp;

    tcp_sock.set_option(tcp::no_delay(!merge_small_requests));
    {
      asio::socket_base::send_buffer_size send_sz_option(send_buffer_size);
      tcp_sock.set_option(send_sz_option);
      tcp_sock.get_option(send_sz_option);
      // if (send_sz_option.value() != send_buffer_size) {
      //   HTTP2_LOG_WARN("tcp sendbuf size option not fully applied, requested: {}, actual: {}",
      //                  send_buffer_size, send_sz_option.value());
      // }
    }
    {
      asio::socket_base::receive_buffer_size rsv_sz_option(receive_buffer_size);
      tcp_sock.set_option(rsv_sz_option);
      tcp_sock.get_option(rsv_sz_option);
      // if (rsv_sz_option.value() != receive_buffer_size) {
      //   HTTP2_LOG_WARN("tcp receive buf size option not fully applied, requested: {}, actual: {}",
      //                  send_buffer_size, rsv_sz_option.value());
      // }
    }
  } catch (std::exception& /*e*/) {
    // its not critical if options are not applied
    // HTTP2_LOG_WARN("Cannot apply tcp settings to socket, err: {}", e.what());
  }
};

// creates 'ref' version (do not own 'ctx')
any_io_context make_asio_io_context(asio::io_context& ctx, tcp_connection_options = {});

any_io_context make_asio_io_context(tcp_connection_options = {});

// if ssl == nullptr creates non ssl version
// creates 'ref' version (do not own 'ctx')
any_io_context make_asio_tls_io_context(asio::io_context& ctx, ssl_context_ptr ssl,
                                        tcp_connection_options = {});

// if ssl == nullptr creates non ssl version
any_io_context make_asio_tls_io_context(ssl_context_ptr ssl, tcp_connection_options = {});

any_io_context make_asio_tls_io_context(std::vector<std::filesystem::path> additional_tls_certificates = {});

}  // namespace http2
