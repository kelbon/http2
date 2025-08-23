
#include <http2/http2_server.hpp>
#include <http2/fuzzing/assertion.hpp>
#include "http2/asio/asio_executor.hpp"
#include "http2/http2_client.hpp"

using namespace http2;

// TODO и для сервера и для клиента протестировать отправку/получение

struct bistream_test_server : http2_server {
  using http2_server::http2_server;

  bool answer_before_data(http_request const&) const noexcept override {
    return true;
  }

  static streaming_body_t stream_body(memory_queue_ptr q, http_headers_t& trailers, request_context ctx) {
    while (!q->last_chunk_received()) {
      auto c = co_await q->next_chunk();
      co_yield c;
    }
    trailers = {{"trailer-header", "abc"}};
  }

  dd::task<std::pair<http_response, stream_body_maker_t>> handle_request_stream(
      http_request req, memory_queue_ptr q, request_context ctx) override {
    co_await ctx.send_interim_response(100);
    http_response rsp;
    rsp.status = 200;
    rsp.headers = std::move(req.headers);
    rsp.headers.push_back({"ok", "accepted"});
    co_return {std::move(rsp), std::bind_front(&stream_body, q)};
  }

  dd::task<http_response> handle_request(http_request req, request_context ctx) override {
    REQUIRE(false);
    throw 0;
  }
};

inline size_t requests_done = 0;

static streaming_body_t do_request(http_response rsp, memory_queue_ptr q, request_context) {
  REQUIRE(rsp.status == 200);
  REQUIRE(rsp.headers == http_headers_t{{"abc", "edf"}, {"ok", "accepted"}});
  std::string data = "hello world data word big liloaf";

  for (char& c : data) {
    co_yield {(byte_t*)&c, 1};
  }
  std::string received;
  while (data != received) {
    auto c = co_await q->next_chunk();
    received.insert(received.end(), c.begin(), c.end());
  }
  REQUIRE(received == data);
}

std::atomic<bool> done = false;

dd::task<void> run_one_request(http2_client& c) {
  http_request req;
  req.authority = "abcd";
  req.method = http2::http_method_e::CONNECT;
  req.headers.push_back({"abc", "edf"});
  int status = co_await c.send_connect_request(std::move(req), &do_request);
  REQUIRE(status == 200);
  ++requests_done;
}

dd::task<void> run_requests(http2_client& client, size_t count, asio::ip::tcp::endpoint endpoint) {
  on_scope_exit {
    done = true;
  };
  size_t i = 0;
  while (requests_done != count) {
    if (i < count) {
      ++i;
      run_one_request(client).start_and_detach();
    }
    co_await yield_on_ioctx(client.ioctx());
  }
}

int main() {
  bistream_test_server server(http2_server_options{.idleTimeout = std::chrono::seconds(50000)});

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 8080);
  server.listen(server_endpoint{.addr = ipv6_endpoint, .reuse_address = true});
  http2_client client(ipv6_endpoint,
                      {.pingInterval = duration_t::max(), .allow_requests_before_server_settings = true});
  // run_requests(client, 100, ipv6_endpoint).start_and_detach();
  std::thread([&] {
    run_requests(client, 100, ipv6_endpoint).start_and_detach();
    client.ioctx().run();
  }).detach();

  while (!done) {
    server.ioctx().poll_one();
  }
}
