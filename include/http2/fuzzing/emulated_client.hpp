#pragma once

#include "http2/fuzzing/any_request_template.hpp"

namespace http2::fuzzing {

// all here expects server to be echo server

// also validates result
dd::task<void> send_echo_request(fuzzer& fuz, http2_client& c, hreq req);

// sends requests, but body will be splitted into random chunks
// also validates result
dd::task<void> send_echo_request_as_stream(fuzzer& fuz, http2_client& c, hreq req);

// sends request, but body will be splitted into random chunks + expects server answers stream
// also validates result
dd::task<void> send_echo_request_connect(fuzzer& fuz, http2_client& c, hreq req, bool websocket);

struct req_weights {
  double regular = 6;
  double stream = 3;
  double connect = 1;
};
// emulates non tls client, stops when all N requests are done somehow or test fail occurs
// assumes server is echo server
dd::task<void> emulate_client_n(fuzzer&, http2_client&, any_reqtem tem, size_t request_count,
                                size_t max_active_streams = 10, req_weights = {});

// emulates non-tls client, stops after `dur` (but waits all requests finish)
// assume server is echo server
dd::task<void> emulate_client(fuzzer&, http2_client&, any_reqtem tem, duration_t dur,
                              size_t max_active_streams = 10, req_weights = {});

inline dd::async_task<void> on_another_thread(dd::task<void> emulated_client_task) {
  dd::schedule_status s = co_await dd::jump_on(dd::new_thread_executor);
  REQUIRE(!!s);

  co_await emulated_client_task;
}

}  // namespace http2::fuzzing
