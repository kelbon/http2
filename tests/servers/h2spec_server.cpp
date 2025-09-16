#include "h2spec_server.hpp"

#include <iostream>
#include <csignal>

bool stop = false;
int main() try {
  std::signal(SIGINT, +[](int) { stop = true; });
  std::signal(SIGTERM, +[](int) { stop = true; });
  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 2999);
  http2::h2spec_server server;
  server.listen({ipv4_endpoint});
  std::cout << std::flush;
  while (!stop)
    server.ioctx().poll();
} catch (std::exception& e) {
  std::cout << e.what() << std::endl;
}
