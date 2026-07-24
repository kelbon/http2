#pragma once

#include <chrono>
#include <memory>

#include <anyany/anyany.hpp>

#include "http2/asio/aio_context.hpp"
#include "http2/utils/any_timer.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/utils/fn_ref.hpp"

namespace http2 {

// for using in single thread!
struct asio_timer {
  using clock_type = std::chrono::steady_clock;
  using time_point = clock_type::time_point;
  using duration = clock_type::duration;

  struct impl;

 private:
  std::shared_ptr<impl> m_impl;

 public:
  explicit asio_timer(boost::asio::io_context&);

  // arms timer to execute callback after 'd'
  // if timer was armed, its canceled first
  void arm(duration d);

  // arms timer to execute callback on specified time point
  // if timer was armed, its canceled first
  void arm(time_point);

  void arm(deadline_t d) {
    return arm(d.tp);
  }

  // arms timer after 'd' and repeats this each 'd'
  // new arm will be after executing task
  // if timer was armed, its canceled first
  void arm_periodic(duration d);

  [[nodiscard]] bool is_armed() const noexcept;

  // returns 'true' if timer was armed before 'cancel'
  // do not touches setted callback
  bool cancel() noexcept;

  void set_callback(timer_callback_t);
};

}  // namespace http2
