#include <thread>

#include <http2/http2_client.hpp>
#include "servers/echo_server.hpp"
#include "http2/fuzzing/any_request_template.hpp"
#include "http2/fuzzing/emulated_client.hpp"
#include "http2/fuzzing/fuzzer.hpp"

using namespace http2::fuzzing;
namespace asio = boost::asio;

// TODO rm (into kelcoro)
template <std::invocable<> U>
auto chain(dd::task<void> t1, U t2) -> dd::task<std::invoke_result_t<U>> {
  assert(t1);
  co_await t1;
  t2();
  co_return;
}

using namespace std::chrono_literals;

int main() try {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    HTTP2_LOG_ERROR("test timeout!");
    // std::exit(-1);
  }).detach();

  fuzzer fuz;
  http2::http2_server_options opts;
  opts.maxConcurrentStreams = 10;
  http2::echo_server server(opts);

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 80);
  server.listen(http2::server_endpoint{.addr = ipv6_endpoint, .reuse_address = true});

  http2::http2_client client1(
      ipv6_endpoint, {// avoid sending too many requests because server sets max concurrent streams == 10
                      .allow_requests_before_server_settings = false});

  clone_reqtem tem;
  auto& r = tem.req;
  r.is_valid = true;
  r.deadline = http2::deadline_t::never();  // http2::deadline_after(std::chrono::seconds(10));
  r.trailers = {};
  auto& rr = tem.req.request;
  rr.path = "/abc";
  rr.headers = {{"header1", "value1"}};
  std::string bd = "hello world";
  rr.body.contentType = "text/plain";
  rr.body.data.assign(bd.begin(), bd.end());
  bool done = false;
  chain(emulate_client_n(fuz, client1, tem, 100, 10, {.stream = 0, .connect = 0}), [&] {
    done = true;
  }).start_and_detach();

  while (!done) {
    server.ioctx().poll();
    // poll one to avoid endless work while server cannot done anything to answer
    client1.ioctx().poll_one();
  }
  HTTP2_LOG_INFO("FUZZING TEST: SUCCESS");
  return 0;
} catch (...) {
  HTTP2_LOG_ERROR("unknown error");
  return 9;
}
