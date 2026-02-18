#pragma once

#include "http2/asio/awaiters.hpp"
#include "http2/http2_server.hpp"

namespace http2 {

// если отправлен серверу, то он будет отвечать только через некоторое время, указанное в value как
// миллисекунды
constexpr inline std::string_view ANSWER_AFTER_MS_SPECIAL_HDR = "x-x-answer-after-ms";

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
    co_await handle_special_headers(req);
    http_response rsp;
    rsp.status = 200;
    if (!req.body.content_type.empty()) {
      rsp.headers.emplace_back("content-type", req.body.content_type);
    }
    rsp.headers.insert(rsp.headers.end(), req.headers.begin(), req.headers.end());
    rsp.body = std::move(req.body.data);
    co_return rsp;
  }

 private:
  dd::task<void> handle_special_headers(http_request const& req) {
    for (auto& [n, v] : req.headers) {
      if (n == ANSWER_AFTER_MS_SPECIAL_HDR) {
        size_t count;
        auto res = std::from_chars(v.data(), v.data() + v.size(), count);
        if (res.ec != std::errc{} || res.ptr != v.data() + v.size())
          std::terminate();  // используется в тестах, это означает неверно написанный тест
        co_await net.sleep(ioctx(), std::chrono::milliseconds(count));
      }
    }
  }
};

}  // namespace http2
