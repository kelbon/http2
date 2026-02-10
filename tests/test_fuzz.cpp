#include <thread>

#include <http2/http2_client.hpp>
#include "http2/asio/factory.hpp"
#include "servers/echo_server.hpp"
#include "any_request_template.hpp"
#include "emulated_client.hpp"
#include "fuzzer.hpp"

#include <kelcoro/task.hpp>
#include <kelcoro/algorithm.hpp>

#include <iostream>

using namespace http2::fuzzing;
namespace asio = boost::asio;

using namespace std::chrono_literals;

int main() try {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::cout << "TEST TIMEOUT" << std::endl;
    std::exit(-1);
  }).detach();

  fuzzer fuz;
  http2::http2_server_options opts;
  opts.maxConcurrentStreams = 10;
  http2::echo_server server(opts);

  asio::ip::tcp::endpoint ipv6_endpoint(asio::ip::address_v6::loopback(), 8080);
  server.listen(http2::server_endpoint{.addr = ipv6_endpoint, .reuse_address = true});

  http2::http2_client client1(
      ipv6_endpoint,
      {// avoid sending too many requests because server sets max concurrent streams == 10
       .allow_requests_before_server_settings = false},
      http2::factory_with_tcp_options<http2::asio_factory>({}));

  clone_reqtem tem;
  auto& r = tem.req;
  r.is_valid = true;
  r.deadline = http2::deadline_t::never();
  r.trailers = {};
  auto& rr = tem.req.request;
  rr.path = "/abc";
  rr.headers = {{"header1", "value1"}};
  std::string bd = "hello world";
  rr.body.content_type = "text/plain";
  rr.body.data.assign(bd.begin(), bd.end());
  bool done = false;
  dd::chain(emulate_client_n(fuz, client1, tem, 100, 10, {.stream = 0, .connect = 0}), [&] {
    done = true;
    return std::suspend_never{};
  }).start_and_detach();

  fuz.run_until([&] { return done; }, server.ioctx(), client1.ioctx());
  std::cout << "FUZZING TEST: SUCCESS" << std::endl;
  return 0;
} catch (...) {
  std::cout << "UNKNOWN ERROR" << std::endl;
  return 9;
}
