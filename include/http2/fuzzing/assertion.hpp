#pragma once

#include <source_location>
#include <iostream>
#include <format>

namespace http2::fuzzing {

inline void REQUIRE(bool b, std::source_location l = std::source_location::current()) {
  if (!b) {
    std::cout << std::format("REQUIRE failed in function {} ({}:{}:{})\n", l.function_name(), l.file_name(),
                             l.line(), l.column());
    // TODO print stacktrace
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace http2::fuzzing
