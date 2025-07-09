
#pragma once

#include <chrono>
#include <compare>

namespace http2v2 {

using duration_t = std::chrono::steady_clock::duration;

struct deadline_t {
  using time_point_t = std::chrono::steady_clock::time_point;

  time_point_t tp;

  [[nodiscard]] constexpr bool isReached(
      time_point_t point = std::chrono::steady_clock::now()) const noexcept {
    return tp <= point;
  }

  duration_t remainingTime() const noexcept {
    return tp - std::chrono::steady_clock::now();
  }

  static constexpr deadline_t never() noexcept {
    return deadline_t{time_point_t::max()};
  }
  static constexpr deadline_t yesterday() noexcept {
    return deadline_t{time_point_t::min()};
  }
  std::strong_ordering operator<=>(deadline_t const &) const = default;
};

// TODE duration underflow
inline deadline_t deadline_after(duration_t duration) noexcept {
  // avoid overflow
  auto tp = std::chrono::steady_clock::now();
  if (tp.max() - tp <= duration) {
    return deadline_t{tp.max()};
  }
  return deadline_t{tp + duration};
}

} // namespace http2v2
