#include "servers/h2spec_server.hpp"

#include <latch>
#include <string>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

namespace bp = boost::process;

inline std::latch g_test_started(2);
inline std::atomic<bool> g_test_done = false;

static void run_h2spec() try {
  g_test_started.arrive_and_wait();

  std::string command = std::format("h2spec --port 3000 -o 1");
  std::future<std::string> out;
  bp::system(command, bp::std_out > out, bp::std_err > bp::null);
  std::string output = out.get();

  if (output.find("146 tests, 145 passed, 0 skipped, 1 failed") == output.npos) {
    std::exit(EXIT_FAILURE);
  }
  if (output.find("4.2. Maximum Table Size") == output.npos) {
    std::exit(EXIT_FAILURE);
  }
  g_test_done.store(true, std::memory_order::release);
} catch (bp::process_error const& e) {
  std::exit(EXIT_FAILURE);
}

int main() {
  std::thread h2spec(&run_h2spec);

  http2::h2spec_server server(
      http2::log_context{.lvl = http2::log_level_e::INFO, .dolog = &http2::noop_log_function});
  server.listen({.addr = {asio::ip::address_v4::loopback(), 3000}});
  g_test_started.count_down();
  while (!g_test_done.load(std::memory_order::acquire))
    server.ioctx().poll();

  h2spec.join();

  return 0;
}
