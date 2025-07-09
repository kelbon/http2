
#pragma once

#include <kelcoro/noexport/macro.hpp>

#ifdef __clang__
#define YACORE_TRIVIAL_ABI [[clang::trivial_abi]]
#else
#define YACORE_TRIVIAL_ABI
#endif

namespace http2v2 {
// forbids move for type without breaking 'is_aggregate'
struct pin {
  pin() = default;
  pin(pin &&) = delete;
  void operator=(pin &&) = delete;
};

[[noreturn]] static void unreachable() noexcept {
  assert(false);
  KELCORO_UNREACHABLE;
}

} // namespace http2v2

// must be used as field in type, makes it unmovable without breaking
// is_aggregate
#define YACORE_PIN KELCORO_NO_UNIQUE_ADDRESS ::http2v2::pin _pin_ = {}
