#pragma once

#include "http2/asio/ssl_context.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http_base.hpp"
#include "http2/transport_factory.hpp"
#include "http2/request_context.hpp"

#include <kelcoro/task.hpp>
#include <kelcoro/thread_pool.hpp>

namespace http2 {

struct server_endpoint {
  internet_address addr;
  bool reuse_address = true;
};

// single threaded interface of server
// user must inherit http2_server and implement virtual methods, then use http2_server itself as
// signlethreaded or use http2::server as multithreaded
struct http2_server {
 private:
  struct impl;
  std::unique_ptr<impl> m_impl;

  friend struct mt_server;
  // used by http2::server
  void set_accept_callback(move_only_fn<void(asio::ip::tcp::socket)>);

 public:
  // creates non-tls server
  explicit http2_server(http2_server_options options = {}) : http2_server(nullptr, std::move(options)) {
  }

  // if ssl context ptr is nullptr, then its http server (not https)
  explicit http2_server(ssl_context_ptr, http2_server_options = {}, tcp_connection_options = {});

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
  // Note: request.body is empty, but body.content_type may be setted
  virtual bool answer_before_data(http_request const& r) const noexcept {
    return false;
  }

  // invoked only after `answer_before_data` returned true
  // request body.data.empty() == true
  // Note: request.body is empty, but body.content_type may be setted
  // returned response must not contain body data
  // if returned `stream_body_maker_t` is empty, just sends HEADERS, e.g. unaccepted websocket stream
  // Note: ctx.stream_response must not be used in this function
  virtual dd::task<std::pair<http_response, bistream_body_maker_t>> handle_request_stream(http_request,
                                                                                          memory_queue_ptr,
                                                                                          request_context) {
    HTTP2_LOG_ERROR("handle_request_stream is not overriden, but `answer_before_data` returns true");
    std::terminate();
  }

  // precondition: returned coro must not wait for sever shutdown / terminate (deadlock)
  // if exception thrown from 'handle_request', server will RST_STREAM (PROTOCOL_ERROR)
  // request_context lighweight object, easy to copy. It will be valid while request in progress, even if
  // .stream_response used
  // if http2::stream_error thrown, its error code used in RST_STREAM
  virtual dd::task<http_response> handle_request(http_request, request_context) = 0;

  [[nodiscard]] size_t sessions_count() const noexcept;

  void listen(server_endpoint);

  // shutdown server softly, all responses will be sent after this method calling
  dd::task<void> shutdown();
  // server termination, stops server sessions without waiting for responses
  // but it can't stop response processing, and server are not going to send them
  dd::task<void> terminate();

  // used to run server tasks
  asio::io_context& ioctx();

  void request_stop();
  // similar to ioctx().run(), for common interface with mt_server
  void run();

 private:
  friend struct http2_tester;
};

// multithreaded version
struct mt_server {
 private:
  struct local_server_ctx {
    std::unique_ptr<http2_server> server;
    asio::executor_work_guard<asio::io_context::executor_type>* work_guard;
  };
  std::vector<local_server_ctx> servers;
  size_t last_selected_server = 0;
  std::optional<dd::thread_pool> pool;
  bool running = false;

  // listen starts always on main thread io_context
  local_server_ctx& listen_server() {
    return servers[0];
  }

  local_server_ctx& next_server() noexcept {
    local_server_ctx& s = servers[last_selected_server];
    last_selected_server = (last_selected_server + 1) % servers.size();
    return s;
  }
  void initialize();

 public:
  // creates server with default thread count, constructs S(args...) on each thread
  template <std::derived_from<http2_server> S, typename... Args>
  explicit mt_server(std::in_place_type_t<S> t, Args&&... args)
      : mt_server(std::thread::hardware_concurrency(), t, std::forward<Args>(args)...) {
  }

  template <std::derived_from<http2_server> S>
  explicit mt_server(size_t threadcount, std::in_place_type_t<S>, auto&&... args) {
    if (threadcount == 0) {
      threadcount = std::thread::hardware_concurrency();
      if (threadcount == 0)
        threadcount = 1;
    }
    if (threadcount > 1)  // main thread also works
      pool.emplace(threadcount - 1);
    for (; threadcount; --threadcount) {
      // Note: not perfect forward
      servers.push_back(local_server_ctx(std::unique_ptr<http2_server>(new S(args...))));
    }
    initialize();
  }

  mt_server(mt_server&&) = delete;
  void operator=(mt_server&&) = delete;

  ~mt_server() = default;

  void listen(server_endpoint);

  // runs until .stop called. Must not be invoked when `run` is active already
  // run may be called only once!
  void run();

  // prevents new requests and sessions, when requests on active sessions are done `run` call will end
  // server may be stopped only once!
  void request_stop();
};

}  // namespace http2
