
#pragma once

#include "http2/any_connection.hpp"
#include "http2/errors.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/logger.hpp"

#include <filesystem>
#include <optional>

#include <kelcoro/task.hpp>
#include <boost/asio/ip/tcp.hpp>
#undef DELETE
#undef NO_ERROR
#undef MIN

namespace asio = boost::asio;

namespace http2 {

using endpoint_t = asio::ip::tcp::endpoint;

struct transport_factory_i {
  // postcondition: .has_value() == true
  virtual dd::task<any_connection_t> createConnection(endpoint_t, deadline_t) = 0;

  virtual ~transport_factory_i() = default;
};

using any_transport_factory = std::unique_ptr<transport_factory_i>;

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
  void apply(asio::basic_socket<asio::ip::tcp, E>& tcp_sock) {
    using tcp = asio::ip::tcp;

    tcp_sock.set_option(tcp::no_delay(!merge_small_requests));
    {
      asio::socket_base::send_buffer_size send_sz_option(send_buffer_size);
      tcp_sock.set_option(send_sz_option);
      tcp_sock.get_option(send_sz_option);
      if (send_sz_option.value() != send_buffer_size) {
        HTTP2_LOG_WARN("tcp sendbuf size option not fully applied, requested: {}, actual: {}",
                       send_buffer_size, send_sz_option.value());
      }
    }
    {
      asio::socket_base::receive_buffer_size rsv_sz_option(receive_buffer_size);
      tcp_sock.set_option(rsv_sz_option);
      tcp_sock.get_option(rsv_sz_option);
      if (rsv_sz_option.value() != receive_buffer_size) {
        HTTP2_LOG_WARN("tcp receive buf size option not fully applied, requested: {}, actual: {}",
                       send_buffer_size, rsv_sz_option.value());
      }
    }
  }
};

any_transport_factory default_transport_factory(boost::asio::io_context&);

any_transport_factory default_tls_transport_factory(
    boost::asio::io_context&, std::vector<std::filesystem::path> additional_tls_certificates = {});

}  // namespace http2
