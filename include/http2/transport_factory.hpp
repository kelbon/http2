
#pragma once

#include "http2/any_connection.hpp"
#include "http2/errors.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/logger.hpp"

#include <filesystem>
#include <optional>
#include <variant>

#include <kelcoro/task.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace asio = boost::asio;

namespace http2 {

// ip + port
using internet_address = asio::ip::tcp::endpoint;

// [ip or fqdn] and port
// fqdn - Fully Qualified Domain Name
struct endpoint {
  std::variant<std::string, asio::ip::address> addr;
  asio::ip::port_type port = 0;

  endpoint() = default;

  // if port setted to 0, binds correct port
  endpoint(asio::ip::address a, asio::ip::port_type port = 0) : addr(std::move(a)), port(port) {
  }
  // Note: ignores if `fqdn` contains port already
  explicit endpoint(std::string fqdn, asio::ip::port_type port = 0) : addr(std::move(fqdn)), port(port) {
  }
  endpoint(internet_address a) : endpoint(a.address(), a.port()) {
  }

  // sets both addr and port
  void set_endpoint(internet_address a) noexcept {
    set_addr(a.address());
    set_port(a.port());
  }
  // returns address only if resolved already
  std::optional<internet_address> get_endpoint() const noexcept {
    if (auto* addr = ipaddr())
      return internet_address(*addr, get_port());
    else
      return std::nullopt;
  }

  void set_addr(asio::ip::address a) noexcept {
    addr = std::move(a);
  }
  void set_fqdn(std::string s) noexcept {
    addr = std::move(s);
  }
  void set_port(asio::ip::port_type p) noexcept {
    port = p;
  }
  asio::ip::port_type get_port() const noexcept {
    return port;
  }

  std::string* fqdn() noexcept {
    return std::get_if<std::string>(&addr);
  }
  const std::string* fqdn() const noexcept {
    return std::get_if<std::string>(&addr);
  }

  asio::ip::address* ipaddr() noexcept {
    return std::get_if<asio::ip::address>(&addr);
  }
  const asio::ip::address* ipaddr() const noexcept {
    return std::get_if<asio::ip::address>(&addr);
  }

  std::string fqdn_str() const noexcept {
    if (auto* x = fqdn()) {
      return *x;
    } else {
      return ipaddr()->to_string();
    }
  }

  std::string to_string() const {
    if (auto* x = fqdn()) {
      if (port == 0)
        return *x;
      else
        return std::format("{}:{}", *x, port);
    } else {
      if (port == 0)
        return ipaddr()->to_string();
      else
        return std::format("{}:{}", ipaddr()->to_string(), port);
    }
  }
};

struct transport_factory_i {
  // postcondition: .has_value() == true
  virtual dd::task<any_connection_t> createConnection(endpoint, deadline_t) = 0;

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
  void apply(asio::basic_socket<asio::ip::tcp, E>& tcp_sock) try {
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
  } catch (std::exception& e) {
    HTTP2_LOG_WARN("Cannot apply tcp settings to socket, err: {}", e.what());
  }
};

any_transport_factory default_transport_factory(boost::asio::io_context&);

any_transport_factory default_tls_transport_factory(
    boost::asio::io_context&, std::vector<std::filesystem::path> additional_tls_certificates = {});

// binds tcp options to factory for passing tcp options to http2_client
// `Factory` must be constructible from asio::io_context and tcp_connection_options
// usage:
//   http2_client(host,
//                client_options,
//                factory_with_tcp_options<SomeFactory>(tcp_options)
//   )
template <typename Factory>
auto factory_with_tcp_options(tcp_connection_options opts) {
  return [opts = std::move(opts)](asio::io_context& ctx) -> any_transport_factory {
    return any_transport_factory(new Factory(ctx, opts));
  };
}

}  // namespace http2
