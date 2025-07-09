
#pragma once

#include "http2v2/utils/deadline.hpp"

#include <coroutine>

#include <seastar/core/future.hh>
#include <seastar/core/make_task.hh>
#include <seastar/core/with_timeout.hh>

namespace http2v2 {

// seastar::future awaiter not usable with other coroutines, this awaiter fixes
// it
template <typename T> struct seastar_future_awaiter {
  seastar::future<T> f;

  seastar_future_awaiter(seastar::future<T> fut) : f(std::move(fut)) {}

  bool await_ready() // NOLINT
  {
    return f.available();
  }

  void await_suspend(std::coroutine_handle<> h) // NOLINT
  {
    // explicit rvalue since seastar lambda task has bad overload sets
    f.set_coroutine(*seastar::make_task(std::coroutine_handle<>(h)));
  }

  std::add_rvalue_reference_t<T> await_resume() // NOLINT
  {
    // seastar devs rly used value_type&& in context where 'void' possible
    if constexpr (std::is_void_v<T>) {
      return f.get0();
    } else {
      return f.get();
    }
  }
};

template <typename T>
struct seastar_future_awaiter_noexcept : seastar_future_awaiter<T> {
  using base_t = seastar_future_awaiter<T>;
  using base_t::base_t;

  using base_t::await_ready;
  using base_t::await_suspend;

  seastar::future<T> &&await_resume() noexcept
  /*[[clang::lifetimebound]]*/ // NOLINT
  {
    return std::move(this->f);
  }
};

template <typename T> auto wait_future_noexcept(seastar::future<T> f) {
  return seastar_future_awaiter_noexcept<T>(std::move(f));
}

template <typename T> auto wait_future(seastar::future<T> foo) {
  return seastar_future_awaiter<T>(std::move(foo));
}

template <typename T>
auto wait_future(seastar::future<T> foo, deadline_t deadline) {
  return seastar_future_awaiter<T>(
      seastar::with_timeout(deadline.tp, std::move(foo)));
}

} // namespace http2v2
