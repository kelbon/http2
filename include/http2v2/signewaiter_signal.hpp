
#pragma once

#include <cassert>
#include <coroutine>
#include <utility>

namespace http2v2 {

struct singlewaiter_signal {
  std::coroutine_handle<> waiter = nullptr;

  [[nodiscard]] bool hasWaiter() const noexcept { return waiter != nullptr; }

  bool notify() {
    if (!waiter) {
      return false;
    }
    std::coroutine_handle<> w = std::exchange(waiter, nullptr);
    w.resume();
    return true;
  }

  struct signal_awaiter {
    singlewaiter_signal &sig;

    bool await_ready() noexcept // NOLINT
    {
      assert(sig.waiter == nullptr && "only one waiter at one time allowed");
      return false;
    }
    void await_suspend(std::coroutine_handle<> w) noexcept // NOLINT
    {
      sig.waiter = w;
    }
    void await_resume() noexcept // NOLINT
    {
      assert(sig.waiter == nullptr &&
             "waiter should be awakened by 'notify()'");
    }
  };

  [[nodiscard]] auto wait() { return signal_awaiter{*this}; }
};

} // namespace http2v2
