#include "h2spec_server.hpp"
#include <iostream>

int main() try {
  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 2999);
  http2::h2spec_server server;
  server.listen({ipv4_endpoint});
  std::cout << std::flush;
  server.ioctx().run();
} catch (std::exception& e) {
  std::cout << e.what() << std::endl;
}
