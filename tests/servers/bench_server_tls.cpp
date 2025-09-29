
#include "benchmark_server.hpp"
#include <http2/asio/awaiters.hpp>

#include <csignal>
#include <iostream>

using namespace http2;

mt_server server(std::in_place_type<bench_server>, HTTP2_TLS_DIR "/test_server.crt",
                 HTTP2_TLS_DIR "/test_server.key");

void on_ctrl_c(int) {
  server.request_stop();
}

int main() try {
  std::signal(SIGINT, &on_ctrl_c);
  asio::ip::tcp::endpoint addr(asio::ip::address_v4::loopback(), 2999);

  server.listen({addr});
  server.run();
} catch (std::exception& e) {
  std::cout << e.what() << std::endl;
}
