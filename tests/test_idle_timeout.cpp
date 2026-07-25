#include "test_connection.hpp"
#include <moko3/moko3.hpp>

using namespace hidi;
using namespace std::chrono_literals;

SERVER_TEST("idle timeout") {
  // проверка происходит раз в 100 мс, так что таймаут по бездействию не может быть слишком маленьким
  server.get_options().idle_timeout = 50ms;
  auto client = co_await fake_client_connection(ioctx, addr);
  co_await emulate_client_connection(client);

  bool with_request = GENERATE(false, true);
  if (with_request) {
    std::vector<header> hdrs{
        {":method", "GET"},
        {":path", "/README.md"},
        {":scheme", "http"},
        {":authority", addr.address().to_string()},
        {std::string(ANSWER_AFTER_MS_SPECIAL_HDR), "1000"},
    };
    co_await client.send_req(1, std::move(hdrs));
    co_await net.sleep(ioctx, 1000ms);  // idle таймаута нет
    auto rsp = co_await client.receive_rsp(deadline_after(300ms));
    REQUIRE(rsp.end_stream && rsp.streamid == 1 && rsp.body.empty() && rsp.headers.size() == 2 &&
            rsp.headers[1].name == ANSWER_AFTER_MS_SPECIAL_HDR);
  }
  // должен получить graceful shutdown по таймауту
  co_await client.receive_goaway(with_request ? 1 : 0, errc_e::NO_ERROR, ping_e::ERROR,
                                 deadline_after(500ms));
}

REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
