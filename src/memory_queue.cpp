#include "http2/utils/memory_queue.hpp"
#include "http2/http2_connection.hpp"

namespace http2 {

memory_queue::memory_queue(h2stream& node) noexcept {
  assert(!node.onDataPart);
  node.onDataPart = this;
  n = &node;
  // on server side memory queue must be created after receiving HEADERS, before any DATA
  // on client side must be created only in send_connect_request, no data is sent for it
  assert(node.req.body.data.empty());
}

memory_queue::~memory_queue() {
  n->onDataPart = nullptr;
}

}  // namespace http2
