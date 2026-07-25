#pragma once

#include "http2/asio/aio_context.hpp"
#include "http2/utils/unique_name.hpp"

#include <boost/intrusive_ptr.hpp>

#include <filesystem>
#include <span>

namespace http2 {

namespace asio = boost::asio;

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

// tag
struct client_ssl_context_ptr {
  ssl_context_ptr p;

  explicit client_ssl_context_ptr(ssl_context_ptr p) noexcept : p(std::move(p)) {
  }

  client_ssl_context_ptr(nullptr_t) noexcept : p(nullptr) {
  }

  explicit operator bool() const noexcept {
    return p != nullptr;
  }
};

// tag
struct server_ssl_context_ptr {
  ssl_context_ptr p;

  explicit server_ssl_context_ptr(ssl_context_ptr p) noexcept : p(std::move(p)) {
  }
  server_ssl_context_ptr(nullptr_t) noexcept : p(nullptr) {
  }

  explicit operator bool() const noexcept {
    return p != nullptr;
  }
};

// throw on error
client_ssl_context_ptr make_ssl_context_for_client(std::span<const std::filesystem::path> additional_certs,
                                                   const log_context& = empty_log_context);

// throw on error
server_ssl_context_ptr make_ssl_context_for_server(std::filesystem::path certificate,
                                                   std::filesystem::path server_private_key,
                                                   const log_context& = empty_log_context);

}  // namespace http2
