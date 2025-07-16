#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http_base.hpp"
#include "http2/transport_factory.hpp"
#include "http2/utils/fn_ref.hpp"

#include <kelcoro/task.hpp>

namespace http2 {

struct server_endpoint {
  endpoint_t addr;
  bool reuse_address = true;
};

// NOTE! this class is made to be used with seastar::sharded<T>
struct http2_server {
  struct impl;
  std::unique_ptr<impl> m_impl;

 public:
  // if ssl context ptr is nullptr, then its http server (not https)
  explicit http2_server(ssl_context_ptr, http2_server_options = {});

  http2_server(std::filesystem::path certificate, std::filesystem::path server_private_key,
               http2_server_options opts = {})
      : http2_server(make_ssl_context_for_server(std::move(certificate), std::move(server_private_key)),
                     std::move(opts)) {
  }

  http2_server(http2_server&&) = delete;
  void operator=(http2_server&&) = delete;

  virtual ~http2_server();

  // precondition: 'handle_request' must not wait for sever shutdown / terminate (deadlock)
  virtual dd::task<http_response> handle_request(http_request) = 0;

  [[nodiscard]] size_t sessionsCount() const noexcept;

  void listen(server_endpoint);

  // shutdown server softly, all responses will be sent after this method calling
  dd::task<void> shutdown();
  // server termination, stops server sessions without waiting for responses
  // but it can't stop response processing, and server are not going to send them
  dd::task<void> terminate();

  // used to run server tasks
  // TODO test behavior with several threads running .run()
  asio::io_context& ioctx();
};

}  // namespace http2
