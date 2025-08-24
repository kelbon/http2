#pragma once

#include <source_location>
#include <iostream>
#include <format>

namespace http2::fuzzing {

inline void fuzzing_require(bool b, std::source_location l = std::source_location::current()) {
  if (!b) {
    std::cout << std::format("REQUIRE failed in function {} ({}:{}:{})\n", l.function_name(), l.file_name(),
                             l.line(), l.column());
    // TODO print stacktrace
    std::exit(EXIT_FAILURE);
  }
}

#define REQUIRE(...) ::http2::fuzzing::fuzzing_require(bool(__VA_ARGS__))

}  // namespace http2::fuzzing
