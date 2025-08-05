#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http_base.hpp"
#include "http2/transport_factory.hpp"
#include "http2/utils/memory_queue.hpp"

#include <kelcoro/task.hpp>

namespace http2 {

struct server_endpoint {
  endpoint_t addr;
  bool reuse_address = true;
};

// Note: its NOT thread safe to copy on other thread
struct request_context {
 private:
  node_ptr node;

 public:
  explicit request_context(request_node& n) noexcept : node(&n) {
  }

  stream_id_t streamid() const noexcept;
  // true if client already sent RST_STREAM for this request
  bool canceled_by_client() const noexcept;

  // must be returned from `handle_request` to send stream response.
  // For example if server want to send big file. Also can send trailers (by settings headers passed into
  // channel)
  // precondition: makebody.has_value(), status > 0
  [[nodiscard("return it")]] http_response stream_response(
      int status, http_headers_t, move_only_fn<streaming_body_t(http_headers_t& trailers)> makebody);

  [[nodiscard("return it")]] http_response stream_response(int status, http_headers_t hdrs,
                                                           streaming_body_t body) {
    return stream_response(status, std::move(hdrs), streaming_body_without_trailers(std::move(body)));
  }

  // make sense only for :method == CONNECT requests, e.g. websockets / proxy
  // `makestream` will be called once and will be alive while request exist
  // yield from this channel will send data to client, memory_queue may be used to receive data
  [[nodiscard("return it")]] http_response bidirectional_stream_response(
      int status, http_headers_t hdrs, move_only_fn<streaming_body_t(memory_queue_ptr)> makestream);
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

  // precondition: 'handle_request' must not wait for sever shutdown / terminate (deadlock)
  // if exception thrown from 'handle_request', server will RST_STREAM (PROTOCOL_ERROR)
  // request_context lighweight object, easy to copy. It will be valid while request in progress, even if
  // .stream_response used
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
