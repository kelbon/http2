
#pragma once

#include "http2/errors.hpp"
#include "http2/utils/memory.hpp"

#include <span>

#include <anyany/anyany.hpp>
#include <kelcoro/task.hpp>

namespace http2 {

struct connection_i {
  virtual void startRead(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) = 0;
  virtual void startWrite(std::coroutine_handle<> callback, std::span<byte_t const> buf,
                          io_error_code& ec) = 0;
  virtual void shutdown() = 0;
  virtual bool isHttps() = 0;

  virtual ~connection_i() = default;
};

using any_connection_t = std::unique_ptr<connection_i>;

// awaiters for using with .startWrite / .startRead

struct read_awaiter {
  any_connection_t& con;
  io_error_code& ec;
  std::span<byte_t> buf;

  static bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) const {
    con->startRead(h, buf, ec);
  }
  static void await_resume() noexcept {
  }
};

struct write_awaiter {
  any_connection_t& con;
  io_error_code& ec;
  std::span<byte_t const> buf;

  static bool await_ready() noexcept {
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) {
    con->startWrite(h, buf, ec);
  }
  static void await_resume() noexcept {
  }
};

}  // namespace http2
