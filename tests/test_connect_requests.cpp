
#include <http2/http2_server.hpp>
#include <http2/fuzzing/assertion.hpp>

using namespace http2;
using http2::fuzzing::REQUIRE;

// TODO и для сервера и для клиента протестировать отправку/получение

struct connect_test_server : http2_server {
  using http2_server::http2_server;

  bool answer_before_data(http_request const& r) const noexcept override {
    return r.method == http2::http_method_e::CONNECT;
  }

  dd::task<http_response> handle_request(http_request req, request_context ctx) override {
    REQUIRE(ctx.answered_before_data());
    // TODO всё же надо получать мемк до ... чтобы определять статус
    ////TODO return ctx.connect_response(int status, http_headers_t hdrs,
    //                       move_only_fn<streaming_body_t(memory_queue_ptr)> makestream);
    // TODO interim rspns, stream response smthmth
    // TODO accepting stream by parts (may be co_await next_chunk?)
    assert(req.method != http_method_e::CONNECT);
    // TODO connect
    http_response rsp;
    rsp.status = 200;
    if (!req.body.contentType.empty()) {
      rsp.headers.emplace_back("content-type", req.body.contentType);
    }
    rsp.headers.insert(rsp.headers.end(), req.headers.begin(), req.headers.end());
    rsp.body = std::move(req.body.data);
    co_return rsp;
  }
};

int main() {
  // TODO;
}
