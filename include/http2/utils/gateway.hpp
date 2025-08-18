#pragma once

#include <kelcoro/executor_interface.hpp>

#include <cassert>
#include <utility>

namespace http2 {

// intrusive queue of awaiters, not thread safe
template <typename Awaiter>
struct awaiters_queue {
  using node_type = Awaiter;

 private:
  node_type* first = nullptr;
  // if !first, 'last' value unspecified
  // if first, then 'last' present too
  node_type* last = nullptr;

  [[nodiscard]] node_type* pop_all_nolock() noexcept {
    return std::exchange(first, nullptr);
  }

 public:
  bool empty() const noexcept {
    return first == nullptr;
  }

  // precondition: node != nullptr && node is not contained in queue
  void push(node_type* node) {
    node->next = nullptr;
    push_list(node, node);
  }

  // attach a whole linked list
  void push_list(node_type* first_, node_type* last_) {
    assert(first_ && last_);
    if (first) {
      last->next = first_;
      last = last_;
    } else {
      first = first_;
      last = last_;
    }
  }

  void push_list(node_type* first_) {
    if (!first_)
      return;
    auto last_ = first_;
    while (last_->next)
      last_ = last_->next;
    push_list(first_, last_);
  }

  [[nodiscard]] node_type* pop_all() {
    return pop_all_nolock();
  }

  // precondition: node && node->task
  // executor interface
  void attach(node_type* node) noexcept {
    assert(node && node->task);
    push(node);
  }
};

// gateway for coroutines. Not thread safe
struct gateway {
 private:
  // want exclusive lock
  awaiters_queue<dd::task_node> _waiters;
  bool _closed = false;

 public:
  // precondition: not closed (use wait_open in loop...)
  void close() noexcept {
    assert(!_closed);
    _closed = true;
  }

  [[nodiscard]] bool closed() const noexcept {
    return _closed;
  }

  // notifies all waiters, that gateway is opened
  void open(dd::executor auto& e) {
    _closed = false;
    attach_list(e, _waiters.pop_all());
  }

  struct open_awaiter : dd::task_node {
    gateway& me;

    open_awaiter(gateway& w) noexcept : me(w) {
    }

    bool await_ready() const noexcept {
      return !me.closed();
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      task = h;
      me._waiters.push(this);
    }

    // someone may grab `lock` before our wake up
    [[nodiscard]] bool await_resume() noexcept {
      return !me.closed();
    }
  };

  open_awaiter wait_open() noexcept {
    return open_awaiter(*this);
  }
};

}  // namespace http2
