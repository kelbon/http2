#pragma once

#include <http2/http2_server.hpp>
#include <http2/asio/awaiters.hpp>
#include <charconv>

namespace http2 {

inline dd::channel<std::span<const byte_t>> streambody() {
  std::string_view answer = "hello world";
  for (char const& c : answer) {
    co_yield {(const byte_t*)&c, 1};
  }
}

struct h2spec_server : http2_server {
  using http2_server::http2_server;
  asio::steady_timer t;
  bool answer_stream = false;

  explicit h2spec_server(log_context ctx = log_context{})
      : http2_server(http2_server_options{
            .maxReceiveFrameSize = MIN_MAX_FRAME_LEN,  // enables FRAME_SIZE tests
            .maxConcurrentStreams = 10,                // enables h2spec test for it
            .logctx = std::move(ctx),
        }),
        t(ioctx()) {
  }

  dd::task<http_response> handle_request(http_request r, request_context ctx) override {
    answer_stream = !answer_stream;
    // some specific h2 test for content-length, which i dont want to handle in server
    auto hdr = std::ranges::find(r.headers, "content-length", &http2::http_header_t::hname);
    if (hdr != r.headers.end()) {
      std::string_view len = hdr->hvalue;
      size_t value;
      auto [ptr, ec] = std::from_chars(len.data(), len.data() + len.size(), value);
      if (ec != std::errc{} || value != r.body.data.size())
        throw http2::stream_error(errc_e::PROTOCOL_ERROR, ctx.streamid(),
                                  "\"content-length\" does not equal to DATA len");
    }
    if (answer_stream) {
      co_return ctx.stream_response(200, {}, streambody());
    }
    http_response rsp;
    rsp.status = 200;
    std::string_view answer = "hello world";
    auto* in = answer.data();
    rsp.body.assign(in, in + answer.size());
    co_return rsp;
  }
};

}  // namespace http2
