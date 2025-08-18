#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

#include <kelcoro/executor_interface.hpp>

namespace http2 {

struct asio_executor {
  boost::asio::io_context& ctx;

  void attach(dd::task_node* n) {
    boost::asio::post(ctx, n->task);
  }
};

}  // namespace http2
