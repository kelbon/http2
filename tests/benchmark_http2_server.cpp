#include <http2/http2_server.hpp>
#include <iostream>
#include <http2/asio/awaiters.hpp>

using namespace http2;
struct bench_server : http2_server {
  using http2_server::http2_server;
  asio::steady_timer t;
  io_error_code ec;

  bench_server(http2_server_options o) : http2_server(o), t(ioctx()) {
  }

  dd::task<http_response> handle_request(http_request&& r) override {
    http_response& rsp = co_await dd::this_coro::return_place;
    rsp.status = 200;
    std::string_view answer = "hello world";
    auto* in = answer.data();
    rsp.body.assign(in, in + answer.size());
    co_await net.sleep(t, std::chrono::milliseconds(100), ec);
    co_return dd::rvo;
  }
};

int main() try {
  // several h2spec tests require small max frame size
  http2_server_options options{.maxReceiveFrameSize = 15'000, .singlethread = true};
  bench_server server(options);

  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 3000);
  server.listen({ipv4_endpoint});

  server.ioctx().run();
} catch (std::exception& e) {
  std::cout << e.what() << std::endl;
}
