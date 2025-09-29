#pragma once

#include "http2/asio/aio_context.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http2_errors.hpp"
#include "http2/http_base.hpp"
#include "http2/utils/memory_queue.hpp"
#include "kelcoro/task.hpp"

namespace http2 {

// Note: its NOT thread safe to copy on other thread
struct request_context {
 private:
  node_ptr node = nullptr;

  friend struct http2_client;

 public:
  explicit request_context(request_node& n) noexcept : node(&n) {
  }

  explicit operator bool() const noexcept {
    return node != nullptr;
  }

  stream_id_t streamid() const noexcept;
  // true if remote endpoint already sent RST_STREAM for this request
  // or connection dropped already
  bool canceled() const noexcept;

  // must be returned from `handle_request` to send stream response.
  // For example if server want to send big file. Also can send trailers (by settings headers passed into
  // channel)
  // precondition: makebody.has_value(), status > 0
  [[nodiscard("return it")]] http_response stream_response(int status, http_headers_t,
                                                           stream_body_maker_t makebody);

  [[nodiscard("return it")]] http_response stream_response(int status, http_headers_t hdrs,
                                                           streaming_body_t body) {
    return stream_response(status, std::move(hdrs), streaming_body_without_trailers(std::move(body)));
  }

  // precondition: status is informational (in range [100, 199])
  dd::task<void> send_interim_response(int status, http_headers_t hdrs = {});

  boost::asio::io_context* owner_ioctx();
};

}  // namespace http2
