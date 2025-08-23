#include "http2/utils/memory_queue.hpp"
#include "http2/http2_connection.hpp"

namespace http2 {

memory_queue::memory_queue(request_node& n) noexcept {
  assert(!n.onDataPart);
  n.onDataPart = this;
  // on server side memory queue must be created after receiving HEADERS, before any DATA
  // on client side must be created only in send_connect_request, no data is sent for it
  assert(n.req.body.data.empty());
}

}  // namespace http2
