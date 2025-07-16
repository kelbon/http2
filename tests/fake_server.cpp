#include <http2/http2_server.hpp>
#include <http2/asio/factory.hpp>

#include <iostream>
#include <format>

struct print_server : http2::http2_server {
  using http2::http2_server::http2_server;

  dd::task<http2::http_response> handle_request(http2::http_request req) {
    http2::http_response& rsp = co_await dd::this_coro::return_place;
    rsp.status = 200;
    std::string answer = R"(
{
  "ok": true,
  "result": {
    "id": 123456789,
    "is_bot": true,
    "first_name": "MyBot",
    "username": "MyBotUsername",
    "can_join_groups": true,
    "can_read_all_group_messages": false,
    "supports_inline_queries": false
  }
}
)";
    rsp.body.insert(rsp.body.end(), answer.begin(), answer.end());
    std::cout << std::format("request, path: {}, body {}", req.path,
                             std::string_view((const char*)req.body.data.data(), req.body.data.size()));
    co_return dd::rvo;
  }
};

namespace asio = boost::asio;

int main() {
  print_server server("E:/dev/ssl_test_crt/server.crt", "E:/dev/ssl_test_crt/server.key");

  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 443);
  server.listen({ipv4_endpoint});

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 443);
  server.listen({ipv4_endpoint});

  // TODO when done?
  server.ioctx().run();
}
