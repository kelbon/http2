
#pragma once

#include "http2v2/http2_errors.hpp"
#include "http2v2/utils/macro.hpp"

#include <exception>

#include <fmt/core.h>

namespace http2v2 {

enum struct io_error_code_e {
  ok,                  // NOLINT
  operation_aborted,   // NOLINT
  network_unreachable, // NOLINT
};

struct io_error_code {
  io_error_code_e e = io_error_code_e::ok;

  explicit operator bool() const noexcept { return e != io_error_code_e::ok; }
  io_error_code &operator=(io_error_code_e f) noexcept {
    e = f;
    return *this;
  }

  std::string_view what() const noexcept {
    switch (e) {
    case io_error_code_e::ok:
      return "<no error>";
    case io_error_code_e::network_unreachable:
      return "network error";
    case io_error_code_e::operation_aborted:
      return "operation aborted";
    }
    unreachable();
  }

  bool operator==(io_error_code_e f) const noexcept { return f == e; }
};

struct network_exception : std::exception {
  std::string data;

  template <typename... ARGS>
  explicit network_exception(fmt::format_string<ARGS...> fmtStr, ARGS &&...args)
      : data(fmt::format(fmtStr, std::forward<ARGS>(args)...)) // -V1067
  {}
  explicit network_exception(io_error_code const &ec)
      : data(fmt::format("{}", ec.what())) // -V1067
  {}
  explicit network_exception(std::string s) noexcept : data(std::move(s)) {}
  char const *what() const noexcept KELCORO_LIFETIMEBOUND override {
    return data.c_str();
  }
};

struct timeout_exception : std::exception {
  char const *what() const noexcept override { return "timeout"; }
};

struct http_exception : std::exception {
  int status = 0;

  explicit http_exception(int s) noexcept : status(s) {}
  char const *what() const noexcept override { return "http_exception"; }
};

struct bad_request : http_exception {
  std::string description;

  explicit bad_request(std::string s) noexcept
      : http_exception(404), description(std::move(s)) {}

  char const *what() const noexcept override { return description.c_str(); }
};

struct unimplemented : std::exception {
  char const *msg = nullptr;
  unimplemented(char const *m) noexcept : msg(m) {}
  char const *what() const noexcept override { return msg; }
};

} // namespace http2v2
