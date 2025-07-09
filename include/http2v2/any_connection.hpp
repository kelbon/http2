
#pragma once

#include "http2v2/errors.hpp"
#include "http2v2/utils/memory.hpp"

#include <span>

#include <anyany/anyany.hpp>
#include <kelcoro/task.hpp>

namespace http2v2 {

struct connection_i {
  virtual void startRead(std::coroutine_handle<> callback,
                         std::span<byte_t> buf, io_error_code &ec) = 0;
  virtual void startWrite(std::coroutine_handle<> callback,
                          std::span<byte_t const> buf, io_error_code &ec,
                          size_t &written) = 0;
  virtual void shutdown() = 0;
  virtual bool isHttps() = 0;
  virtual void deinit() noexcept {
    // nothing by default, used by tls to deinit its callback before shutting
    // down
  }
  virtual ~connection_i() = default;
};

using any_connection_t = std::unique_ptr<connection_i>;

// awaiters for using with .startWrite / .startRead

struct read_awaiter {
  any_connection_t &con;
  io_error_code &ec;
  std::span<byte_t> buf;

  static bool await_ready() noexcept // NOLINT
  {
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) const // NOLINT
  {
    con->startRead(h, buf, ec);
  }
  static void await_resume() noexcept // NOLINT
  {}
};

struct write_awaiter {
  any_connection_t &con;
  io_error_code &ec;
  std::span<byte_t const> buf;
  size_t written = 0;

  static bool await_ready() noexcept // NOLINT
  {
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) // NOLINT
  {
    con->startWrite(h, buf, ec, written);
  }
  size_t await_resume() noexcept // NOLINT
  {
    return written;
  }
};

} // namespace http2v2
