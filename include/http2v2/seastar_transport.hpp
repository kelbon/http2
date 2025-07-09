
#pragma once

#include "http2v2/any_connection.hpp"
#include "http2v2/transport_factory.hpp"
#include "http2v2/utils/deadline.hpp"

#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <kelcoro/task.hpp>
#include <seastar/net/api.hh>
#include <yacore/transport/TlsTransport.hpp>

namespace http2v2 {

struct seastar_connection_options {
  uint32_t sendBufferSize = 1024 * 1024 * 4;    // 4 MB
  uint32_t receiveBufferSize = 1024 * 1024 * 4; // 4 MB
  std::vector<std::filesystem::path> additionalSslCertificates;
  // adds delay (waiting for new requests to merge them)
  bool mergeSmallRequests = false;
};

struct seastar_connection : connection_i {
  struct impl;

private:
  seastar::shared_ptr<impl> m_impl;

  // non virtual for dstor
  seastar::future<> doshutdown() noexcept;

public:
  seastar_connection() = default;
  // precondition: 's' is connected socket
  seastar_connection(seastar::connected_socket s,
                     seastar_connection_options opts);

  seastar_connection(seastar_connection &&) = default;
  seastar_connection &operator=(seastar_connection &&) = default;

  ~seastar_connection();

  // creates client connection
  static dd::task<seastar_connection> create(sockaddr_or_fqdn_t,
                                             seastar::socket_address srcaddr,
                                             seastar_connection_options,
                                             deadline_t);

  // any_connection_t interface:

  void startRead(std::coroutine_handle<> callback, std::span<byte_t>,
                 io_error_code &) override;
  void startWrite(std::coroutine_handle<> callback, std::span<byte_t const>,
                  io_error_code &, size_t &written) override;

  void shutdown() noexcept override;

  bool isHttps() override { return false; }
};

struct seastar_transport : transport_factory_i {
  seastar_connection_options options;
  // aborts sleep
  seastar::abort_source abortsrc;

  explicit seastar_transport(seastar_connection_options opts = {})
      : options(opts) {}

  seastar_transport(seastar_transport &&) = delete;
  void operator=(seastar_transport &&) = delete;

  dd::task<any_connection_t> createConnection(sockaddr_or_fqdn_t,
                                              seastar::socket_address srcaddr,
                                              deadline_t) override;
  dd::task<void> sleep(duration_t d, io_error_code &ec) override;
  void abortSleeps() noexcept override {
    abortsrc.request_abort();
    // renew abort source, since old may not be used anymore
    abortsrc = seastar::abort_source{};
  }
};

using tlserrcb_t = std::function<void(transport::tls_connection_error const &)>;

struct seastar_tls_connection : connection_i {
private:
  std::shared_ptr<transport::TlsTransport> m_impl;
  seastar::shared_ptr<tlserrcb_t> m_tlserrcb;

  // non virtual for dstor
  seastar::future<> doshutdown() noexcept;

public:
  seastar_tls_connection() = default;
  // precondition: 't' != nullptr
  seastar_tls_connection(std::unique_ptr<transport::TlsTransport> t,
                         tlserrcb_t cb) {
    assert(t);
    m_impl = std::move(t);
    if (cb) {
      m_tlserrcb = seastar::make_shared<tlserrcb_t>(std::move(cb));
    }
  }

  seastar_tls_connection(seastar_tls_connection &&) = default;
  seastar_tls_connection &operator=(seastar_tls_connection &&) = default;

  ~seastar_tls_connection();

  // creates client connection
  static dd::task<any_connection_t>
  create(sockaddr_or_fqdn_t, seastar::socket_address srcaddr, deadline_t,
         transport::ClientSecurity params, tlserrcb_t);

  // for server
  static seastar::future<any_connection_t> accept(transport::server_tls_params,
                                                  tlserrcb_t);

  // any_connection_t interface:

  void startRead(std::coroutine_handle<> callback, std::span<byte_t>,
                 io_error_code &) override;
  void startWrite(std::coroutine_handle<> callback, std::span<byte_t const>,
                  io_error_code &, size_t &written) override;

  void shutdown() noexcept override;

  bool isHttps() override { return true; }
  void deinit() noexcept override {
    // avoid calling user callback after work done (and callback may be deleted)
    if (m_tlserrcb) {
      *m_tlserrcb = nullptr;
    }
  }
};

struct seastar_tls_transport : transport_factory_i {
  transport::ClientSecurity secparams;
  tlserrcb_t cb;
  // aborts sleep
  seastar::abort_source abortsrc;

  // creates for client
  explicit seastar_tls_transport(tlserrcb_t tlscb, transport::ClientSecurity cs)
      : secparams(std::move(cs)), cb(std::move(tlscb)) {}

  seastar_tls_transport(seastar_tls_transport &&) = delete;
  void operator=(seastar_tls_transport &&) = delete;

  dd::task<any_connection_t> createConnection(sockaddr_or_fqdn_t,
                                              seastar::socket_address srcaddr,
                                              deadline_t) override;
  dd::task<void> sleep(duration_t d, io_error_code &ec) override;
  void abortSleeps() noexcept override {
    abortsrc.request_abort();
    // renew abort source, since old may not be used anymore
    abortsrc = seastar::abort_source{};
  }
};

} // namespace http2v2
