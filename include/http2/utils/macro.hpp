
#pragma once

#include <kelcoro/noexport/macro.hpp>
#include <zal/zal.hpp>

#ifdef __clang__
  #define KELHTTP2_TRIVIAL_ABI [[clang::trivial_abi]]
#else
  #define KELHTTP2_TRIVIAL_ABI
#endif

namespace http2 {

[[noreturn]] inline void unreachable() noexcept {
  assert(false);
  KELCORO_UNREACHABLE;
}

}  // namespace http2
