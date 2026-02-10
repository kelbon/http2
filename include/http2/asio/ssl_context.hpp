#pragma once

#include "http2/asio/aio_context.hpp"
#include "http2/utils/unique_name.hpp"

#include <boost/intrusive_ptr.hpp>

#include <filesystem>
#include <span>

namespace asio = boost::asio;

namespace http2 {

struct ssl_context;
// must be used only in one thread, multithread using of ssl_context is not safe bcs of open ssl
using ssl_context_ptr = boost::intrusive_ptr<ssl_context>;

struct ssl_context {
 private:
  size_t refcount = 0;

  // private, must not be created on stack
  ~ssl_context() = default;

 public:
  asio::ssl::context ctx;

  explicit ssl_context(asio::ssl::context_base::method);

  ssl_context(ssl_context&&) = delete;
  void operator=(ssl_context&&) = delete;

  friend void intrusive_ptr_add_ref(ssl_context* p) noexcept {
    ++p->refcount;
  }
  friend void intrusive_ptr_release(ssl_context* p) noexcept {
    --p->refcount;
    if (p->refcount == 0)
      delete p;
  }
};

ssl_context_ptr make_ssl_context_for_http2(std::span<const std::filesystem::path> additional_certs,
                                           const log_context& = empty_log_context);

ssl_context_ptr make_ssl_context_for_http11(std::span<const std::filesystem::path> additional_certs,
                                            const log_context& = empty_log_context);

// returns null on error
ssl_context_ptr make_ssl_context_for_server(std::filesystem::path certificate,
                                            std::filesystem::path server_private_key,
                                            const log_context& = empty_log_context);

}  // namespace http2
