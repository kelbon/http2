#include "h2spec_server.hpp"

int main() {
  http2::h2spec_server server;

  asio::ip::tcp::endpoint ipv4_endpoint(asio::ip::address_v4::loopback(), 3000);
  server.listen({ipv4_endpoint});

  server.ioctx().run();
}
