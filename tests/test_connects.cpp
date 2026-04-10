
#include "test_connection.hpp"
#include <moko3/moko3.hpp>

using namespace http2;
using namespace std::chrono;

CLIENT_TEST("connects") {
  REQUIRE(!client.connected() && !client.connecting());

  http2_client_options opts = client.get_options();
  // чтобы клиент ждал ответа от сервера и соединение не было успешно завершено
  opts.allow_requests_before_server_settings = false;
  client.set_options(std::move(opts));

  // TCP часть соединения, частичный успех
  server_endpoint addr(localhost());
  asio::ip::tcp::acceptor a(ioctx, addr.addr, addr.reuse_address);
  a.listen();
  asio::ip::tcp::socket socket(ioctx);
  client.try_connect(a.local_endpoint(), deadline_t::never()).start_and_detach();

  REQUIRE(!client.connected() && client.connecting());
  // остановка посередине соединения
  time_point n = steady_clock::now();
  co_await client.graceful_stop();
  // соединение должно быть прерывано даже на половине
  REQUIRE((steady_clock::now() - n) < client.get_options().connectionTimeout);
}

REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
