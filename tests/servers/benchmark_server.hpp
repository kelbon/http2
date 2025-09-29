#pragma once

#include <http2/http2_server.hpp>

namespace http2 {

struct bench_server final : http2_server {
  using http2_server::http2_server;

  dd::task<http_response> handle_request(http_request r, request_context) override {
    http_response rsp;
    rsp.status = 200;
    std::string_view answer = "hello world";
    auto* in = answer.data();
    rsp.body.assign(in, in + answer.size());
    co_return rsp;
  }
};

}  // namespace http2
