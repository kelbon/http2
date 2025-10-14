#include <thread>

#include <http2/http2_client.hpp>
#include "servers/echo_server.hpp"
#include "http2/fuzzing/any_request_template.hpp"
#include "http2/fuzzing/emulated_client.hpp"
#include "http2/fuzzing/fuzzer.hpp"
#include <kelcoro/algorithm.hpp>

using namespace http2::fuzzing;
namespace asio = boost::asio;

using namespace std::chrono_literals;

int main() try {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    HTTP2_LOG_ERROR("test timeout!");
    std::exit(-1);
  }).detach();

  fuzzer fuz;
  http2::http2_server_options opts;
  opts.maxConcurrentStreams = 10;
  // make CONTINUATIONS possible
  opts.maxReceiveFrameSize = http2::MIN_MAX_FRAME_LEN;
  http2::echo_server server(opts);

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 8080);
  server.listen(http2::server_endpoint{.addr = ipv6_endpoint, .reuse_address = true});

  http2::http2_client client1(
      ipv6_endpoint, {// avoid sending too many requests because server sets max concurrent streams == 10
                      .allow_requests_before_server_settings = false});

  clone_reqtem tem;
  auto& r = tem.req;
  r.is_valid = true;
  r.deadline = http2::deadline_t::never();
  r.trailers = {};
  auto& rr = tem.req.request;
  rr.path = "/abc";
  for (int i = 0; i < 5; ++i) {
    std::string bigstr = fuz.rstring(http2::MIN_MAX_FRAME_LEN + fuz.rint(1, 100));
    rr.headers.emplace_back(http2::http_header_t(std::format("header{}", i), std::move(bigstr)));
  }
  std::string bd = "hello world";
  rr.body.content_type = "text/plain";
  rr.body.data.assign(bd.begin(), bd.end());
  bool done = false;
  dd::chain(emulate_client_n(fuz, client1, tem, 100, 10, {.stream = 0, .connect = 0}), [&] {
    done = true;
    return std::suspend_never{};
  }).start_and_detach();
  fuz.run_until([&] { return done; }, server.ioctx(), client1.ioctx());
  HTTP2_LOG_INFO("FUZZING TEST: SUCCESS");
  return 0;
} catch (...) {
  HTTP2_LOG_ERROR("unknown error");
  return 9;
}
