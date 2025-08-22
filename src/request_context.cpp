#include "http2/request_context.hpp"

#include "http2/http2_connection.hpp"

namespace http2 {

stream_id_t request_context::streamid() const noexcept {
  return node->streamid;
}

bool request_context::canceled_by_client() const noexcept {
  return node->canceledByRstStream;
}

http_response request_context::stream_response(int status, http_headers_t hdrs,
                                               stream_body_maker_t makebody) {
  assert(status > 0);
  assert(makebody);
  assert(node);
  http_response rsp;
  rsp.status = status;
  rsp.headers = std::move(hdrs);
  node->makebody = std::move(makebody);

  return rsp;
}

http_response request_context::connect_response(int status, http_headers_t hdrs, streaming_body_t outstream) {
  assert(node);
  // if exception thrown, it will be in `server::handle_request`
  // (since it called there) and will lead to  RST_STREAM
  memory_queue_ptr q = new memory_queue(node);
  // emulating send_response here, send_response later will not handle it
  http_response rsp;
  rsp.status = status;
  rsp.headers = std::move(hdrs);
  node->makebody = [n = node, outstream = std::move(outstream), q = std::move(q)](http_headers_t&,
                                                                                  request_context) mutable {
    n->bidir_stream_active = true;
    return std::move(outstream);
  };
  return rsp;
}

dd::task<void> request_context::send_interim_response(int status, http_headers_t hdrs) {
  assert(status >= 100 && status <= 199);
  assert(node);
  if (!node->connection)
    co_return;
  HTTP2_LOG(TRACE, "sending interim response with status {}", status, node->connection->name);
  bytes_t bytes;
  node->connection->start_headers_block(*node, /*force_disable_hpack=(unknown here)*/ false, bytes);
  auto out = std::back_inserter(bytes);
  auto& encoder = node->connection->encoder;
  encoder.encode_status(status, out);
  for (auto& h : hdrs) {
    encoder.encode_with_cache(h.name(), h.value(), out);
  }
  HTTP2_WAIT_WRITE(*node->connection);
  io_error_code ec;
  co_await node->connection->write(bytes, ec);
  if (ec) {
    if (ec != boost::asio::error::operation_aborted)
      HTTP2_LOG(ERROR, "cannot send interim response, err: {}", ec.message(), node->connection->name);
  }
}

boost::asio::io_context* request_context::owner_ioctx() {
  assert(node);
  if (!node->connection)
    return nullptr;
  return &node->connection->ioctx;
}

bool request_context::answered_before_data() const noexcept {
  assert(node);
  return node->answered_before_data;
}

memory_queue_ptr request_context::get_memory_queue() {
  assert(node);
  if (!q) {
    q = new memory_queue(node);
  }
  return q;
}

request_context::next_chunk_awaiter::next_chunk_awaiter(request_node& n) noexcept : node(n) {
  prevfn = std::exchange(node.onDataPart, this);
}

request_context::next_chunk_awaiter::~next_chunk_awaiter() {
  node.onDataPart = prevfn;
}

bool request_context::next_chunk_awaiter::await_ready() noexcept {
  return !node.req.body.data.empty();
}

void request_context::next_chunk_awaiter::await_suspend(std::coroutine_handle<> h) {
  waiter = h;
}

request_context::chunk_holder request_context::next_chunk_awaiter::await_resume() noexcept {
  auto& data = node.req.body.data;
  return data.empty() ? chunk_holder(received, node.end_stream_received)
                      : chunk_holder(data, node.end_stream_received);
}

void request_context::next_chunk_awaiter::operator()(std::span<const byte_t> in, bool /*is last*/) {
  assert(node.req.body.data.empty() && waiter);
  // is last setted in node->end_stream_received
  received = in;
  waiter.resume();
}

}  // namespace http2
