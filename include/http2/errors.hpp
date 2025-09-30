
#pragma once

#include "http2/http2_errors.hpp"
#include "http2/utils/macro.hpp"

#include <exception>

#include <format>

#include <boost/system/error_code.hpp>

namespace http2 {

using io_error_code = boost::system::error_code;

struct network_exception : std::exception {
  std::string data;

  template <typename... ARGS>
  explicit network_exception(std::format_string<ARGS...> fmtStr, ARGS&&... args)
      : data(std::format(fmtStr, std::forward<ARGS>(args)...)) {
  }
  explicit network_exception(io_error_code const& ec) : data(std::format("{}", ec.message())) {
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

}  // namespace http2
