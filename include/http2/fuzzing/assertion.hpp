#pragma once

#include <source_location>
#include <iostream>
#include <format>

namespace http2::fuzzing {

inline void fuzzing_require(bool b, std::string msg = {},
                            std::source_location l = std::source_location::current()) {
  if (!b) {
    std::cout << std::format("REQUIRE failed in function {} ({}:{}:{})\n", l.function_name(), l.file_name(),
                             l.line(), l.column());
    if (!msg.empty())
      std::cout << msg << std::endl;
    std::abort();
  }
}

#define REQUIRE(...) ::http2::fuzzing::fuzzing_require(bool(__VA_ARGS__), "Expr: " #__VA_ARGS__)

#define FAIL(MSG) ::http2::fuzzing::fuzzing_require(false, std::string(MSG))

#define REQUIRE_NOTHROW(...)                                                         \
  try {                                                                              \
    __VA_ARGS__;                                                                     \
  } catch (std::exception & e) {                                                     \
    FAIL(std::format("expected to be nothrow, but throwed {}", e.what()));           \
  } catch (...) {                                                                    \
    FAIL(std::format("expected to be nothrow, but smth throwed unknown exception")); \
  }

}  // namespace http2::fuzzing
