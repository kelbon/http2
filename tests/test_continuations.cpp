
#include "test_connection.hpp"

using namespace http2;

SERVER_TEST("server continuations") {
  auto client = co_await fake_client_connection(ioctx, addr, /*tls=*/false);
  co_await emulate_client_connection(client);
  std::vector<header> hdrs{
      {":method", "GET"},
      {":path", "/README.md"},
      {":scheme", "http"},
      {":authority", addr.address().to_string()},
  };
  auto bytes = client.encode_headers(hdrs);
  co_await client.sendRawHdr(1, bytes, /*end_stream=*/true, /*split=*/true);
  auto rsp = co_await client.receiveRsp();
  REQUIRE(rsp.endStream);
  REQUIRE(rsp.body.empty());
  REQUIRE(rsp.headers.size() == 1 && rsp.headers[0] == header{":status", "200"});
}

CLIENT_TEST("client continuations") {
  auto server = co_await fake_server_session(ioctx, {localhost()}, client);
  co_await emulate_server_connection(server);

  http_request req;
  req.method = http_method_e::GET;
  req.path = "/abc";
  req.scheme = scheme_e::HTTP;
  req.authority = client.get_host().to_string();
  std::coroutine_handle handle =
      client.send_request(std::move(req), deadline_after(5s)).start_and_detach(/*stop_at_end=*/true);
  on_scope_exit {
    handle.destroy();
  };
  hdrs_and_data hd = co_await server.receiveReq();

  std::vector<header> hdrs{
      {":status", "200"},
      {"X-customhdr", std::string(MIN_MAX_FRAME_LEN, 'A')},
  };
  auto bytes = server.encode_headers(hdrs);
  co_await server.sendRawHdr(1, bytes, /*end_stream=*/true, /*split=*/true);
  co_await wait_until([&] { return handle.done(); }, ioctx);
  auto rsp = handle.promise().result_or_rethrow();
  REQUIRE(rsp.status == 200 && rsp.headers.size() == 1 && rsp.headers.front() == hdrs[1]);
}

int main(int argc, char* argv[]) try {
  auto& box = http2::get_testbox();
  box.config = cli::parse_or_exit(argc, argv);
  return box.run_tests();
} catch (std::exception& e) {
  std::cout << "running tests failed with err: " << e.what() << '\n';
  return -1;
}
