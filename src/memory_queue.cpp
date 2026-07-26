#include "hidi/utils/memory_queue.hpp"
#include "hidi/h2connection.hpp"

namespace hidi {

memory_queue::memory_queue(h2stream& node) noexcept {
  assert(!node.on_data_part_fn);
  node.on_data_part_fn = this;
  n = &node;
  // on server side memory queue must be created after receiving HEADERS, before any DATA
  // on client side must be created only in send_connect_request, no data is sent for it
  assert(node.req.body.data.empty());
}

memory_queue::~memory_queue() {
  n->on_data_part_fn = nullptr;
}

}  // namespace hidi
