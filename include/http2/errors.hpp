
#pragma once

#include "http2/http2_errors.hpp"
#include "http2/utils/macro.hpp"

#include <exception>

#include <format>

#include <boost/system/error_code.hpp>

#undef NO_DATA
#undef NO_ERROR
#undef Yield
#undef min
#undef max
#undef DELETE

namespace http2 {

using io_error_code = boost::system::error_code;

struct network_exception : std::exception {
  std::string data;

  template <typename... ARGS>
  explicit network_exception(std::format_string<ARGS...> fmtStr, ARGS&&... args)
      : data(std::format(fmtStr, std::forward<ARGS>(args)...))  // -V1067
  {
  }
  explicit network_exception(io_error_code const& ec)
      : data(std::format("{}", ec.what()))  // -V1067
  {
  }
  explicit network_exception(std::string s) noexcept : data(std::move(s)) {
  }
  char const* what() const noexcept KELCORO_LIFETIMEBOUND override {
    return data.c_str();
  }
};

struct timeout_exception : std::exception {
  char const* what() const noexcept override {
    return "timeout";
  }
};

struct http_exception : std::exception {
  int status = 0;

  explicit http_exception(int s) noexcept : status(s) {
  }
  char const* what() const noexcept override {
    return "http_exception";
  }
};

struct bad_request : http_exception {
  std::string description;

  explicit bad_request(std::string s) noexcept : http_exception(404), description(std::move(s)) {
  }

  char const* what() const noexcept override {
    return description.c_str();
  }
};

struct unimplemented : std::exception {
  char const* msg = nullptr;
  unimplemented(char const* m) noexcept : msg(m) {
  }
  char const* what() const noexcept override {
    return msg;
  }
};

}  // namespace http2
