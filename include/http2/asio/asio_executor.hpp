#pragma once

#include "http2/asio/aio_context.hpp"

#include <boost/asio/post.hpp>

#include <kelcoro/executor_interface.hpp>
#include "kelcoro/common.hpp"

namespace http2 {

struct asio_executor {
  boost::asio::io_context& ctx;

  void attach(dd::task_node* n) {
    boost::asio::post(ctx, n->task);
  }
};

// schedules coroutine to be executed on `ctx`
// if not yet on it
// Note: must not be used as `yield`, since it will never suspend when running in this thread!
struct jump_on_ioctx {
  boost::asio::io_context& ctx;

  bool await_ready() const noexcept {
    return ctx.get_executor().running_in_this_thread();
  }

  void await_suspend(std::coroutine_handle<> h) {
    boost::asio::post(ctx, h);
  }

  static void await_resume() noexcept {
  }
};

// schedules coroutine to be executed on `ctx`
inline auto yield_on_ioctx(boost::asio::io_context& ctx) {
  return dd::suspend_and_t([&](std::coroutine_handle<> h) { boost::asio::post(ctx, h); });
}

#ifndef NDEBUG
  #define HTTP2_ASSUME_THREAD_UNCHANGED_START \
    ::std::thread::id _debug_thread_id = ::std::this_thread::get_id()
// if assertion failed, its likely user callback (handle_request / channel) goes on another thread.
// can be fixed by jump_on_ioctx(request_context.owner_ioctx()) in `handle_request` body
  #define HTTP2_ASSUME_THREAD_UNCHANGED_END assert(_debug_thread_id == ::std::this_thread::get_id())
#else
  #define HTTP2_ASSUME_THREAD_UNCHANGED_START (void)0
  #define HTTP2_ASSUME_THREAD_UNCHANGED_END (void)0
#endif

}  // namespace http2
