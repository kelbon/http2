
#include <iostream>
#include <string>

#include <boost/process.hpp>

namespace bp = boost::process;

int main() {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(20));
    std::cout << "TEST TIMEOUT" << std::endl;
    std::exit(-1);
  }).detach();
  try {
    std::future<std::string> foutput;

    bp::ipstream pipe_stream;
    auto cwd = boost::filesystem::current_path();
#ifdef _WIN32
    bp::child server(cwd / "servers/h2spec_server.exe");
#else
    bp::child server(cwd / "servers/h2spec_server");
#endif
    std::this_thread::sleep_for(std::chrono::seconds(1));  // time for server starting
    (void)bp::system("h2spec --port 2999", bp::std_out > foutput, bp::std_err > bp::null);
    std::string output = foutput.get();
    if (output.find("146 tests, 145 passed, 0 skipped, 1 failed") == output.npos) {
      std::cout << "146 tests NOT FOUND, output: " << output;
      return EXIT_FAILURE;
    }
    if (output.find("4.2. Maximum Table Size") == output.npos) {
      std::cout << "4.2. Maximum Table Size NOT FOUND, output: " << output;
      return EXIT_FAILURE;
    }
    server.terminate();
    std::cout << "SUCCESS" << std::endl;
    return EXIT_SUCCESS;
  } catch (const bp::process_error& e) {
    std::cerr << "BOOST PROCESS ERROR: " << e.what() << std::endl;
    return 1;
  }
}
