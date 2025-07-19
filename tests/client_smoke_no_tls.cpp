#include <http2/http2_client.hpp>
#include <http2/asio/factory.hpp>
#include <http2/http2_server.hpp>

constexpr std::string_view expected_response = "hello world";

// all noinlines here is workaround gcc-12 bug (miscompilation)
#define TGBM_GCC_WORKAROUND [[gnu::noinline]]

TGBM_GCC_WORKAROUND void fail(int i) {
  std::exit(i);
}

TGBM_GCC_WORKAROUND http2::http_response answer_req(http2::http_request req) {
  http2::http_response rsp;
  if (req.path == "/abc" && req.method == http2::http_method_e::GET) {
    rsp.status = 200;
    rsp.headers.push_back(http2::http_header_t{.hname = "content-type", .hvalue = "text/plain"});
    rsp.body.insert(rsp.body.end(), expected_response.begin(), expected_response.end());
    if (!req.body.data.empty()) {
      HTTP2_LOG_ERROR("incorrect body, size: {}", req.body.data.size());
      fail(1);
    }
    return rsp;
  } else {
    HTTP2_LOG_ERROR("incorrect path! Path is: {}", req.path);
    fail(2);
    return rsp;
  }
}

struct test_server : http2::http2_server {
  using http2::http2_server::http2_server;

  dd::task<http2::http_response> handle_request(http2::http_request&& req) override {
    http2::http_response rsp = answer_req(std::move(req));
    co_return rsp;
  };
};

inline bool all_good = false;

TGBM_GCC_WORKAROUND dd::task<http2::http_response> make_test_request(http2::http2_client& client) {
  http2::http_request req{
      .path = "/abc",
      .method = http2::http_method_e::GET,
  };
  return client.sendRequest(std::move(req), http2::deadline_t::never());
}

TGBM_GCC_WORKAROUND void check_response(http2::http2_client& client, const http2::http_response& rsp) {
  if (rsp.headers.size() != 1) {  // status and content-type
    HTTP2_LOG_ERROR("incorrect count of headers: {}", rsp.headers.size());
    fail(3);
  }
  if (rsp.headers[0].name() != "content-type" || rsp.headers[0].value() != "text/plain") {
    HTTP2_LOG_ERROR("incorrect header: {}, value: {}", rsp.headers[0].name(), rsp.headers[0].value());
    fail(4);
  }
  if (!std::ranges::equal(expected_response, rsp.body)) {
    HTTP2_LOG_ERROR("incorrect body");
    fail(5);
  }
  all_good = true;
  HTTP2_LOG_INFO("success");
}

dd::task<void> main_coro(http2::http2_client& client) {
  dd::task test_req = make_test_request(client);
  http2::http_response rsp = co_await test_req;
  check_response(client, rsp);
  // TODO? client.stop();
}

int main() try {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    HTTP2_LOG_ERROR("timeout!");
    fail(4);
  }).detach();

  namespace asio = boost::asio;

  http2::http2_client_options opts{};
  // TODO нужно чёт сделать с тем что клиент по дефолту https, а сервер http
  // TODO http клиент (дефолт заменить)
  http2::http2_client client(http2::endpoint_t(asio::ip::address_v6::loopback(), 80), std::move(opts));

  test_server server(nullptr /*no https*/, {});

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 80);
  server.listen(http2::server_endpoint{.addr = ipv6_endpoint, .reuse_address = true});

  main_coro(client).start_and_detach();

  while (!all_good) {
    client.ioctx().poll();
    server.ioctx().poll();
  }

  if (!all_good)
    return 8;

  return 0;
} catch (...) {
  HTTP2_LOG_ERROR("unknown error");
  return 9;
}
