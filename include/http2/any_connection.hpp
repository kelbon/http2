
#pragma once

#include "http2/errors.hpp"
#include "http2/utils/memory.hpp"
#include "http2/utils/boost_intrusive.hpp"

#include <span>

#include <anyany/anyany.hpp>

#include <kelcoro/task.hpp>
#include <kelcoro/gate.hpp>

#include <boost/intrusive/slist_hook.hpp>

namespace http2 {

struct writer_node : bi::slist_base_hook<> {
  std::coroutine_handle<> callback;
  std::span<byte_t const> data;
  io_error_code* ec = nullptr;
  dd::gate::holder holder;  // may be setted by startWrite
  ZAL_PIN;

  writer_node(std::span<byte_t const> data1, io_error_code& ec1) noexcept : data(data1), ec(&ec1) {
  }
};

struct connection_i {
  // returns false if not enough bytes available
  [[nodiscard]] virtual bool try_read(std::span<byte_t> buf) noexcept = 0;
  // precondition: try_read returns false!
  virtual void start_read(std::coroutine_handle<> callback, std::span<byte_t> buf, io_error_code& ec) = 0;
  // tries to write buffer,
  // returns number of written bytes (0 on error)
  virtual size_t try_write(std::span<const byte_t>, io_error_code&) noexcept = 0;
  // pre: node != nullptr
  virtual void start_write(writer_node*) = 0;
  virtual dd::task<void> shutdown() = 0;
  // TODO abort (отменяет текущие операции, затем следует shutdown)
  virtual bool is_https() = 0;

  virtual ~connection_i() = default;
};

using any_connection_t = std::unique_ptr<connection_i>;

// awaiters for using with .start_write / .start_read

struct read_awaiter {
  any_connection_t& con;
  io_error_code& ec;
  std::span<byte_t> buf;

  bool await_ready() noexcept {
    return con->try_read(buf);
  }

  void await_suspend(std::coroutine_handle<> h) const {
    con->start_read(h, buf, ec);
  }
  static void await_resume() noexcept {
  }
};

struct write_awaiter : writer_node {
  any_connection_t& con;

  write_awaiter(any_connection_t& con2, io_error_code& ec, std::span<byte_t const> buf)
      : writer_node(buf, ec), con(con2) {
  }

  bool await_ready() noexcept {
    size_t written = con->try_write(data, *ec);
    if (written == data.size() || ec)
      return true;
    remove_prefix(data, written);
    return false;
  }

  void await_suspend(std::coroutine_handle<> h) {
    callback = h;
    con->start_write(this);
  }
  static void await_resume() noexcept {
  }
};

}  // namespace http2
