#include "http2/fuzzing/emulated_client.hpp"
#include "http2/asio/awaiters.hpp"

using namespace std::chrono_literals;

namespace http2::fuzzing {

static void validate_echo_request(const hreq& req, http_response rsp) {
  if (!req.request.body.contentType.empty()) {
    // content-type should be encoded first (after pseudoheaders, until custom headers)
    // so it should be retunred first, but request.headers does not contain it (must not)
    REQUIRE(!rsp.headers.empty() &&
            rsp.headers.front() == http_header_t{"content-type", req.request.body.contentType});
    rsp.headers.erase(rsp.headers.begin());
  }
  REQUIRE(rsp.headers == req.request.headers);
  REQUIRE(rsp.status == 200);
  REQUIRE(rsp.body == req.request.body.data);
}

dd::task<void> send_echo_request(fuzzer& fuz, http2_client& c, hreq req) {
  assert(req.is_valid);  // TODO not supported yet?
  if (!req.trailers.empty())
    co_return co_await send_echo_request_as_stream(fuz, c, std::move(req));

  http_response rsp = co_await c.sendRequest(req.request, req.deadline);

  validate_echo_request(req, std::move(rsp));
}

// sends requests, but body will be splitted into random chunks
dd::task<void> send_echo_request_as_stream(fuzzer& fuz, http2_client& c, hreq req) {
  auto sleepcb = [&c](duration_t d, io_error_code& ec) -> dd::task<void> {
    boost::asio::steady_timer timer(c.ioctx());
    co_await net.sleep(timer, d, ec);
  };
  auto bodystr =
      streaming_body_with_trailers(fuz.chunks_delayed(req.request.body.data, 0, 30, sleepcb), req.trailers);
  req.request.body = {};
  http_response rsp = co_await c.send_streaming_request(req.request, std::move(bodystr), req.deadline);
  validate_echo_request(req, std::move(rsp));
}

static move_only_fn<streaming_body_t(http_response, memory_queue_ptr)> do_makestream(fuzzer& fuz,
                                                                                     http_headers_t hdrs) {
  return [&fuz, hdrs = std::move(hdrs)](http_response rsp, memory_queue_ptr q) -> streaming_body_t {
    REQUIRE(rsp.status == 200);
    REQUIRE(hdrs == rsp.headers);
    REQUIRE(rsp.body.empty());
    std::string sent;
    std::string received;

    size_t count = fuz.rindex(200);
    for (size_t i = 0; i < count; ++i) {
      if (fuz.rbool()) {
        std::string s = fuz.rstring(fuz.rint(1, 200));
        co_yield {(byte_t*)s.data(), s.size()};
      } else {
        if (received.size() != sent.size()) {
          std::vector s = co_await q->read();
          received.append(s.begin(), s.end());
        }
      }
    }
    while (received.size() != sent.size()) {
      std::vector s = co_await q->read();
      received.append(s.begin(), s.end());
    }
    // echo server
    REQUIRE(received == sent);
    co_return;
  };
}

// sends request, but body will be splitted into random chunks + expects server answers stream
dd::task<void> send_echo_request_connect(fuzzer& fuz, http2_client& c, hreq req, bool websocket) {
  if (websocket) {
    auto& hdrs = req.request.headers;
    req.request.method = http_method_e::CONNECT;
    hdrs.insert(hdrs.begin(), http_header_t{{":protocol", "websocket"}});
  } else {
    req.request.path = {};
    req.request.method = http_method_e::CONNECT;
    req.request.authority = "some.api";
  }
  req.request.body.data = {};  // do_makestream ignores data and sent smth random generated
  int x = co_await c.send_connect_request(req.request, do_makestream(fuz, req.request.headers));
  REQUIRE(x == 200);
}

// precondition: tasks not empty
template <std::invocable<> U>
auto chain(dd::task<void> t1, U t2) -> dd::task<std::invoke_result_t<U>> {
  assert(t1);
  co_await t1;
  if constexpr (std::is_void_v<std::invoke_result_t<U>>) {
    t2();
    co_return;
  } else {
    co_return t2();
  }
}

template <typename T, std::invocable<T> U>
auto chain(dd::task<T> t1, U t2) -> dd::task<std::invoke_result_t<U, T>> {
  assert(t1);
  co_return t2(co_await t1);
}

// TODO run context fuzzer + client + stats + etc?
dd::task<void> emulate_client_n(fuzzer& fuz, http2_client& client, any_reqtem tem, size_t request_count,
                                size_t max_active_streams, req_weights weights) {
  size_t done = 0;
  std::discrete_distribution<int> dist({weights.regular, weights.stream, weights.connect});
  asio::steady_timer timer(client.ioctx());
  size_t planned = 0;
  io_error_code ec;
  // receive server settings before (to get correct max_count_requests_allowed)
  bool b = co_await client.tryConnect();
  REQUIRE(b);
  while (request_count != done) {
    while (request_count != planned && client.count_active_requests() < client.max_count_requests_allowed()) {
      dd::task<void> task;
      ++planned;
      switch (dist(fuz.g)) {
        case 0:
          task = send_echo_request(fuz, client, tem.generate_request(fuz));
          break;
        case 1:
          task = send_echo_request_as_stream(fuz, client, tem.generate_request(fuz));
          break;
        case 2:
          task =
              send_echo_request_connect(fuz, client, tem.generate_request(fuz), /*websocket=*/fuz.rbool(0.3));
          break;
      }
      chain(std::move(task), [&] { ++done; }).start_and_detach();
    }
    co_await net.sleep(timer, std::chrono::microseconds(100), ec);
  }
  // TODO print stats (done etc)
  co_await client.coStop();
  co_return;
}

dd::task<void> emulate_client(fuzzer& fuz, http2_client& client, any_reqtem tem, duration_t dur,
                              size_t max_active_streams, req_weights weights) {
  size_t done = 0;
  std::discrete_distribution<int> dist({weights.regular, weights.stream, weights.connect});
  asio::steady_timer timer(client.ioctx());
  deadline_t deadline = deadline_after(dur);
  io_error_code ec;
  // receive server settings before (to get correct max_count_requests_allowed)
  bool b = co_await client.tryConnect();
  REQUIRE(b);
  // TODO клиент должен давать информацию о том сколько разрешает сервер, сколько сейчас активно
  while (!deadline.isReached()) {
    while (!deadline.isReached() && client.count_active_requests() < client.max_count_requests_allowed()) {
      dd::task<void> task;
      switch (dist(fuz.g)) {
        case 0:
          task = send_echo_request(fuz, client, tem.generate_request(fuz));
          break;
        case 1:
          task = send_echo_request_as_stream(fuz, client, tem.generate_request(fuz));
          break;
        case 2:
          task =
              send_echo_request_connect(fuz, client, tem.generate_request(fuz), /*websocket=*/fuz.rbool(0.3));
          break;
      }
      chain(std::move(task), [&] { ++done; }).start_and_detach();
    }
    co_await net.sleep(timer, std::chrono::microseconds(100), ec);
  }
  // TODO print stats (done etc)
  co_await client.coStop();
  co_return;
}

}  // namespace http2::fuzzing
