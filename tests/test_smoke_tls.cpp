#include <http2/http2_client.hpp>
#include <http2/asio/factory.hpp>
#include <http2/http2_server.hpp>
#include <http2/fuzzing/fuzzer.hpp>

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

const inline http_headers_t EXPECTED_CONNECT_HEADERS{
    http2::http_header_t{":protocol", "websocket"},
    http2::http_header_t{"some-special-hdr", "chat, superchat"},
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

static streaming_body_t handle_connect_request(memory_queue_ptr q, request_context ctx) {
  while (!q->last_chunk_received()) {
    auto chunk = co_await q->next_chunk();
    co_yield chunk;
  }
}

struct test_server : http2_server {
  using http2_server::http2_server;

  bool answer_before_data(http_request const& r) const noexcept override {
    return r.method == http2::http_method_e::CONNECT;
  }

  dd::task<std::pair<http_response, bistream_body_maker_t>> handle_request_stream(
      http_request req, memory_queue_ptr q, request_context ctx) override {
    error_if(req.headers != EXPECTED_CONNECT_HEADERS);
    error_if(!req.body.data.empty());
    error_if(req.path != "/ada");
    http_response rsp;
    rsp.status = 200;
    rsp.headers = {{"okay", "accepted"}};
    co_return {std::move(rsp), std::bind_front(&handle_connect_request, q)};
  }

  dd::task<http_response> handle_request(http_request req, request_context ctx) override {
    error_if(req.method == http2::http_method_e::CONNECT);
    http_response rsp = answer_req(std::move(req));
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
  req.body.content_type = "text/plain";
  auto body = [](http2::http_headers_t& trailers, request_context) { return makebody(trailers); };
  co_return co_await client.send_streaming_request(std::move(req), body, http2::deadline_t::never());
}

streaming_body_t websocket_connect_test(http_response rsp, memory_queue_ptr q, request_context) {
  error_if(!rsp.body.empty());
  error_if(rsp.status != 200);
  error_if(rsp.headers != http_headers_t{{"okay", "accepted"}});

  std::string data = "hello world!";

  for (size_t i = 0; i < data.size(); ++i) {
    co_yield {(byte_t*)&data[i], 1};
    auto s = co_await q->next_chunk();
    error_if(s.size() != 1);
    error_if(s.front() != data[i]);
  }
}

dd::task<void> make_test_websocket_request(http2_client& client) {
  http_request req;
  req.headers = EXPECTED_CONNECT_HEADERS;
  req.path = "/ada";
  req.method = http2::http_method_e::CONNECT;
  int status = co_await client.send_connect_request(std::move(req), &websocket_connect_test);
  error_if(status != 200);  // server accepted request
}

dd::task<void> main_coro(http2::http2_client& client) {
  http2::http_response rsp = co_await make_test_request(client);
  check_response(rsp);

  rsp = co_await make_test_stream_request(client);
  check_streaming_response(rsp);

  co_await make_test_websocket_request(client);

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

  http2::http2_client client(endpoint("localhost", 8080), {},
                             [](asio::io_context& ctx) { return default_tls_transport_factory(ctx); });

  test_server server(HTTP2_TLS_DIR "/test_server.crt", HTTP2_TLS_DIR "/test_server.key");

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 8080);
  server.listen(http2::server_endpoint{.addr = ipv6_endpoint, .reuse_address = true});

  main_coro(client).start_and_detach();

  fuzzing::fuzzer fuz;
  fuz.run_until([&] { return all_good; }, server.ioctx(), client.ioctx());

  return 0;
} catch (std::exception& e) {
  HTTP2_LOG_ERROR("{}", e.what());
  return 9;
}
