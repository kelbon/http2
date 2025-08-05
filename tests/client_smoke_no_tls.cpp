#include <http2/http2_client.hpp>
#include <http2/asio/factory.hpp>
#include <http2/http2_server.hpp>

#include <iostream>

#define error_if(...)                                       \
  if ((__VA_ARGS__)) {                                      \
    std::cout << "ERROR ON LINE " << __LINE__ << std::endl; \
    std::exit(__LINE__);                                    \
  }

using namespace http2;

// all noinlines here is workaround gcc-12 bug (miscompilation)
#define TGBM_GCC_WORKAROUND [[gnu::noinline]]

constexpr inline std::string_view STREAM_REQUEST_PATH = "/bcd";
constexpr inline std::string_view REQUEST_PATH = "/abc";

const inline http_headers_t EXPECTED_HEADERS{
    http2::http_header_t{"hash", "55555"},
    http2::http_header_t{"ok", "yes"},
    http2::http_header_t{"content-type", "mycontent/type"},
};

constexpr inline std::string_view EXPECTED_DATA = "hello world!";

TGBM_GCC_WORKAROUND http2::http_response answer_req(http2::http_request req) {
  http2::http_response rsp;
  if (req.path == REQUEST_PATH && req.method == http2::http_method_e::GET) {
    rsp.status = 200;
    rsp.headers = EXPECTED_HEADERS;
    rsp.body.insert(rsp.body.end(), EXPECTED_DATA.begin(), EXPECTED_DATA.end());
    error_if(!req.body.data.empty());
  } else if (req.path == STREAM_REQUEST_PATH) {
    rsp.status = 200;
    error_if(req.headers != EXPECTED_HEADERS);
    error_if(std::string(req.body.strview()) != EXPECTED_DATA);
    // echo
    rsp.headers = req.headers;
    rsp.body = req.body.data;
  } else {
    error_if(true);
  }

  return rsp;
}

struct test_server : http2::http2_server {
  using http2::http2_server::http2_server;

  dd::task<http2::http_response> handle_request(http2::http_request req, http2::request_context) override {
    http2::http_response rsp = answer_req(std::move(req));
    co_return rsp;
  }
};

inline bool all_good = false;

TGBM_GCC_WORKAROUND dd::task<http2::http_response> make_test_request(http2::http2_client& client) {
  http2::http_request req{
      .path = std::string(REQUEST_PATH),
      .method = http2::http_method_e::GET,
  };
  return client.sendRequest(std::move(req), http2::deadline_t::never());
}

TGBM_GCC_WORKAROUND void check_response(const http2::http_response& rsp) {
  error_if(rsp.headers != EXPECTED_HEADERS);
  error_if(!std::ranges::equal(EXPECTED_DATA, rsp.body));
}

void check_streaming_response(const http2::http_response& rsp) {
  error_if(rsp.status != 200);
  error_if(rsp.headers != EXPECTED_HEADERS);
  error_if(rsp.body_strview() != EXPECTED_DATA);
}

http2::streaming_body_t makebody(http2::http_headers_t& trailers) {
  for (const char& c : EXPECTED_DATA) {
    co_yield {(const http2::byte_t*)&c, 1};
  }
  trailers = EXPECTED_HEADERS;
}

dd::task<http2::http_response> make_test_stream_request(http2::http2_client& client) {
  http2::http_request req{
      .path = std::string(STREAM_REQUEST_PATH),
      .method = http2::http_method_e::PUT,
  };
  req.body.contentType = "text/plain";
  auto body = [](http2::http_headers_t& trailers) { return makebody(trailers); };
  co_return co_await client.send_streaming_request(std::move(req), body, http2::deadline_t::never());
}

dd::task<void> main_coro(http2::http2_client& client) {
  http2::http_response rsp = co_await make_test_request(client);
  check_response(rsp);

  rsp = co_await make_test_stream_request(client);
  check_streaming_response(rsp);

  all_good = true;
  HTTP2_LOG_INFO("success");

  // TODO? client.stop();
}

int main() try {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    HTTP2_LOG_ERROR("test timeout!");
    std::exit(-1);
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
