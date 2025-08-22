#pragma once

#include <memory>
#include <coroutine>

#include <boost/intrusive_ptr.hpp>

#include "http2/http2_connection_fwd.hpp"
#include "http2/utils/macro.hpp"
#include "http2/http_body.hpp"

namespace http2 {

struct memory_queue {
 private:
  size_t refcount = 0;
  bytes_t bytes;
  std::coroutine_handle<> waiter = nullptr;
  node_ptr node = nullptr;
  bool eof = false;  // true if last data frame was received
  KELHTTP2_PIN;

  struct data_awaiter {
    memory_queue* q;

    bool await_ready() const noexcept {
      return !q->bytes.empty() || q->eof;
    }

    void await_suspend(std::coroutine_handle<> h) noexcept {
      assert(!q->waiter);
      q->waiter = h;
    }

    [[nodiscard]] bytes_t await_resume() const noexcept {
      return q->poll_data();
    }
  };

 public:
  // creates memory queue associated with `n`, it will receive all data for `n`
  explicit memory_queue(node_ptr n) noexcept;

  ~memory_queue();

  bool has_data() const noexcept {
    return !bytes.empty();
  }

  // returns empty bytes if no data available
  // returns by copy to avoid invalidate after `push_data`
  [[nodiscard]] bytes_t poll_data() noexcept {
    return std::move(bytes);
  }

  // returns awaiters
  // await returns empty data only in case EOF
  // only one reader at one time allowed
  auto read() {
    return data_awaiter(this);
  }

  // for using with function ref
  void operator()(std::span<const byte_t> b, bool lastframe) {
    // Note: order: set EOF, then push,
    // so if push will wait data again it will produce empty data (EOF marker)
    if (lastframe)
      eof = true;
    bytes.insert(bytes.end(), b.begin(), b.end());
    if (waiter)
      std::exchange(waiter, nullptr).resume();
  }

  friend void intrusive_ptr_add_ref(memory_queue* p) noexcept {
    ++p->refcount;
  }

  friend void intrusive_ptr_release(memory_queue* p) noexcept {
    --p->refcount;
    if (p->refcount == 0) {
      delete p;
    }
  }
};

using memory_queue_ptr = boost::intrusive_ptr<memory_queue>;

}  // namespace http2
