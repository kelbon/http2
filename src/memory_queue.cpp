#include "http2/utils/memory_queue.hpp"
#include "http2/http2_connection.hpp"

namespace http2 {

memory_queue::memory_queue(node_ptr n) noexcept : node(std::move(n)) {
  assert(node);
  node->onDataPart = this;
  auto& data = node->req.body.data;
  if (!data.empty()) {
    // if was last frame, we cannot be here
    (*this)(data, /*lastframe=*/false);
    data.clear();
  }
}

memory_queue::~memory_queue() {
  node->onDataPart = nullptr;
  assert(this->bytes.empty() && "not all data received!");
}

}  // namespace http2
