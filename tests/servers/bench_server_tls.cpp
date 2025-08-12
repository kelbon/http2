
#include <http2/http2_server.hpp>
#include <iostream>
#include <http2/asio/awaiters.hpp>

using namespace http2;

struct bench_server : http2_server {
  using http2_server::http2_server;

  bench_server(http2_server_options o) : http2_server(http2_server_options{.singlethread = true}) {
  }

  dd::task<http_response> handle_request(http_request r, request_context) override {
    http_response rsp;
    rsp.status = 200;
    std::string_view answer = "hello world";
    auto* in = answer.data();
    rsp.body.assign(in, in + answer.size());
    co_return rsp;
  }
};

int main() try {
  bench_server server(HTTP2_TLS_DIR "/test_server.crt", HTTP2_TLS_DIR "/test_server.key");
  std::cout << "benchmark server tls run" << std::endl;
  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 3000);
  server.listen({ipv4_endpoint});

  server.ioctx().run();
} catch (std::exception& e) {
  std::cout << e.what() << std::endl;
}
