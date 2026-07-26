#pragma once

#include "hidi/h2server.hpp"

namespace hidi {

struct bench_server final : h2server {
  using h2server::h2server;

  dd::task<http_response> handle_request(http_request r, request_context) override {
    http_response rsp;
    rsp.status = 200;
    std::string_view answer = "hello world";
    auto* in = answer.data();
    rsp.body.assign(in, in + answer.size());
    co_return rsp;
  }
};

}  // namespace hidi
