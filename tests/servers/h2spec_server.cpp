#include "h2spec_server.hpp"
#include <csignal>
#include <iostream>

http2::h2spec_server server;
int main() try {
  std::signal(
      SIGINT, +[](int) {
        std::cout << "interrupted!";
        server.ioctx().stop();
      });
  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 3000);
  server.listen({ipv4_endpoint});

  server.ioctx().run();
} catch (std::exception& e) {
  std::cout << e.what() << std::endl;
}
