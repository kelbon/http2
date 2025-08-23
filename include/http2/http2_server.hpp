#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http_base.hpp"
#include "http2/transport_factory.hpp"
#include "http2/request_context.hpp"

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
  // creates non-tls server
  explicit http2_server(http2_server_options options = {}) : http2_server(nullptr, std::move(options)) {
  }

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

  // invoked when only headers for request received and data will be received
  // if `true` returned, `handle_request_stream` invoked instead of `handle_request`,
  // Note: request.body is empty, but body.contentType may be setted
  virtual bool answer_before_data(http_request const& r) const noexcept {
    return false;
  }

  // invoked only after `answer_before_data` returned true
  // request body.data.empty() == true
  // Note: request.body is empty, but body.contentType may be setted
  // returned response must not contain body data
  // if returned `stream_body_maker_t` is empty, just sends HEADERS, e.g. unaccepted websocket stream
  // Note: ctx.stream_response must not be used in this function
  virtual dd::task<std::pair<http_response, stream_body_maker_t>> handle_request_stream(http_request,
                                                                                        memory_queue_ptr,
                                                                                        request_context) {
    throw std::runtime_error("handle_request_stream not overriden");
  }

  // precondition: returned coro must not wait for sever shutdown / terminate (deadlock)
  // if exception thrown from 'handle_request', server will RST_STREAM (PROTOCOL_ERROR)
  // request_context lighweight object, easy to copy. It will be valid while request in progress, even if
  // .stream_response used
  // if http2::stream_error thrown, its error code used in RST_STREAM
  virtual dd::task<http_response> handle_request(http_request, request_context) = 0;

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

 private:
  friend struct http2_tester;
};

}  // namespace http2
