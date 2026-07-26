
#include "test_connection.hpp"
#include <moko3/moko3.hpp>

using namespace hidi;
using namespace std::chrono;

CLIENT_TEST("connects") {
  REQUIRE(!client.connected() && !client.connecting());

  http2_client_options opts = client.get_options();
  // чтобы клиент ждал ответа от сервера и соединение не было успешно завершено
  opts.allow_requests_before_server_settings = false;
  client.set_options(std::move(opts));

  // TCP часть соединения, частичный успех
  server_endpoint addr(localhost());
  any_acceptor a = ioctx.create_acceptor(localhost(), /*reuse_address=*/true);
  a.listen();
  // просто для теста фичи устанавливаем локальный адрес
  client.set_local(hidi::internet_address(boost::asio::ip::address_v6::loopback(), 0));
  client.try_connect(a.get_local_endpoint(), deadline_t::never()).start_and_detach();

  REQUIRE(!client.connected() && client.connecting());
  // остановка посередине соединения
  time_point n = steady_clock::now();
  co_await client.graceful_stop();
  // соединение должно быть прерывано даже на половине
  REQUIRE((steady_clock::now() - n) < client.get_options().connection_timeout);
}

CLIENT_TEST("trailers") {
  http2_client_options opts = client.get_options();
  opts.allow_requests_before_server_settings = GENERATE(false, true);
  client.set_options(std::move(opts));

  auto server = co_await fake_server_session(ioctx, {localhost()}, client);
  co_await emulate_server_connection(server);

  http_request req;
  std::string bodydata = "hello world";
  req.body.data.assign(bodydata.begin(), bodydata.end());
  req.method = hidi::http_method_e::GET;
  req.path = "/mypath";
  req.headers.push_back(http_header_t{"name", "value"});
  http_headers_t trailers{{"trail1", "trail_value"}};
  client.send_request_with_trailers(req, trailers, 10s).start_and_detach();

  hdrs_and_data hd = co_await server.receive_req();
  REQUIRE(hd.streamid = 1);
  REQUIRE(hd.body_strview() == bodydata);
  REQUIRE(std::find(hd.headers.begin(), hd.headers.end(), req.headers.front()) != hd.headers.end());
  REQUIRE(hd.trailers && hd.trailers->size() == 1 && hd.trailers->front() == trailers.front());

  // HEADERS + HEADERS (трейлеры без данных)
  req.body = {};
  client.send_request_with_trailers(req, trailers, 10s).start_and_detach();

  hd = co_await server.receive_req();
  REQUIRE(hd.streamid = 3);
  REQUIRE(hd.body_strview() == "");
  REQUIRE(std::find(hd.headers.begin(), hd.headers.end(), req.headers.front()) != hd.headers.end());
  REQUIRE(hd.trailers && hd.trailers->size() == 1 && hd.trailers->front() == trailers.front());
}

SERVER_TEST("server connection drop") {
  auto client = co_await fake_client_connection(ioctx, addr);
  co_await emulate_client_connection(client);
  std::vector<header> hdrs{
      {":method", "GET"},
      {":path", "/README.md"},
      {":scheme", "http"},
      {":authority", addr.address().to_string()},
      {std::string(TERMINATE_THIS_SESSION_HDR), ""},
  };
  co_await client.send_req(1, hdrs);
  co_await client.receive_goaway(1, errc_e::NO_ERROR, ping_e::RESPONSE);
  co_await client.wait_connection_dropped(deadline_t(1s));
}

REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
