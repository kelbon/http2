
#include <iostream>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/process/v2.hpp>
#include <boost/process/v2/environment.hpp>

#include <kelcoro/task.hpp>
#include <http2/asio/awaiters.hpp>

namespace bp = boost::process;

static dd::task<void> read_until_mark(asio::readable_pipe& pipe, std::string& output, std::string mark = {}) {
  using http2::io_error_code;
  using http2::net;
  http2::byte_t buf[128];
  io_error_code ec;
  for (;;) {
    size_t sz = co_await net.read_some(pipe, buf, ec);
    std::cout << std::string_view((const char*)buf, sz);
    if (ec) {
      std::cout << ec.what() << std::endl;
      break;
    }
    output += std::string_view((const char*)buf, sz);
    if (!mark.empty() && output.find(mark) != output.npos)
      break;
  }
}

int main() try {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(20));
    std::cout << "TEST TIMEOUT" << std::endl;
    std::exit(-1);
  }).detach();

  asio::io_context ctx;

  asio::readable_pipe server_pipe(ctx);

#ifdef _WIN32
  auto serverpath = "servers/h2spec_server.exe";
#else
  auto serverpath = "servers/h2spec_server";
#endif
  bp::process server(ctx, serverpath, {}, bp::process_stdio{.out = server_pipe});

  asio::readable_pipe h2spec_pipe(ctx);
  bp::process h2spec(ctx, bp::environment::find_executable("h2spec"), {"--port", "2999", "-o", "1"},
                     bp::process_stdio{.out = h2spec_pipe});
  std::string h2specstr;
  std::string serverstr;
  // h2spec_server flushes output after this log to avoid deadlock
  auto h =
      read_until_mark(server_pipe, serverstr, "Server listening on").start_and_detach(/*stop_at_end=*/true);
  on_scope_exit {
    h.destroy();
  };
  while (!h.done())
    ctx.run_one();

  // read server output to avoid deadlock on std::cout, when pipe if full
  auto h2 = read_until_mark(server_pipe, serverstr).start_and_detach(true);
  on_scope_exit {
    h2.destroy();
  };
  auto h3 = read_until_mark(h2spec_pipe, h2specstr).start_and_detach(true);
  on_scope_exit {
    h3.destroy();
  };
  while (h2spec.running())
    ctx.poll();
  try {
    h2spec.wait();
  } catch (std::exception& e) {
    std::cout << "wait failed with " << e.what() << std::endl;
  }
  server_pipe.cancel();
  h2spec_pipe.cancel();
  server.terminate();

  // h2spec end running, but only here writes last chunks of output
  ctx.poll();
  if (auto i = h2spec.exit_code(); i != 1) {
    std::cout << "invalid error code, expected 1, actual: " << i << std::endl;
    return EXIT_FAILURE;
  }
  if (h2specstr.find("146 tests, 145 passed, 0 skipped, 1 failed") == h2specstr.npos) {
    std::cout << "146 tests NOT FOUND, output: " << h2specstr << std::endl;
    return EXIT_FAILURE;
  }
  if (h2specstr.find("4.2. Maximum Table Size") == h2specstr.npos) {
    std::cout << "4.2. Maximum Table Size NOT FOUND, output: " << h2specstr << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "SUCCESS" << std::endl;
  return EXIT_SUCCESS;
} catch (const std::exception& e) {
  std::cerr << "BOOST PROCESS ERROR: " << e.what() << std::endl;
  return 1;
}
