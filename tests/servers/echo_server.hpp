#pragma once

#include "http2/http2_server.hpp"

namespace http2 {

// TODO также проверять в тестах expected SERVER settings
// и на стороне сервера проверять что expected CLIENT settings
//
struct echo_server : http2_server {
  using http2_server::http2_server;

  dd::task<http_response> handle_request(http_request req, request_context ctx) override {
    // TODO interim rspns, stream response smthmth
    // TODO accepting stream by parts (may be co_await next_chunk?)
    assert(req.method != http_method_e::CONNECT);
    // TODO connect
    http_response rsp;
    rsp.status = 200;
    if (!req.body.content_type.empty()) {
      rsp.headers.emplace_back("content-type", req.body.content_type);
    }
    rsp.headers.insert(rsp.headers.end(), req.headers.begin(), req.headers.end());
    rsp.body = std::move(req.body.data);
    co_return rsp;
  }
};

}  // namespace http2
