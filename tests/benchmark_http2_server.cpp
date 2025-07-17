#include <http2/http2_server.hpp>

#include <iostream>

using namespace http2;
struct bench_server : http2_server {
  using http2_server::http2_server;

  dd::task<http_response> handle_request(http_request&& r) override {
    http_response& rsp = co_await dd::this_coro::return_place;
    // std::cout << std::string_view((char*)r.body.data.data(), r.body.data.size());
    rsp.status = 200;
    co_return dd::rvo;
  }
};

int main() {
  bench_server server;

  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 80);
  server.listen({ipv4_endpoint});

  while (!server.ioctx().stopped())
    server.ioctx().run();
}
