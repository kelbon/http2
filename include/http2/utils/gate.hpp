#pragma once

#include <kelcoro/gate.hpp>
#include <kelcoro/task.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#undef NO_ERROR
#undef Yield
#undef NO_DATA
#undef socket
#undef DELETE

namespace http2 {

// wrapper for dd::gate, correcly handles .close to make sure gate closer may finish its job
struct gate : dd::gate {
  dd::task<void> close(boost::asio::io_context& ctx) {
    co_await dd::gate::close();
    // give someone who last decrements gate counter to get job done (for example destructor)
    co_await dd::suspend_and_t([&](std::coroutine_handle<> h) { boost::asio::post(ctx, h); });
  }
};

}  // namespace http2
