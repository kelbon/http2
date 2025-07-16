#pragma once

#include <chrono>
#include <memory>

#include <anyany/anyany.hpp>

#include <boost/asio/io_context.hpp>

namespace http2 {

// TODO? мб не на boost asio завязывать а на timer group, вопрос откуда она возьмётся конечно...

// for using in single thread!
struct timer_t {
  using clock_type = std::chrono::steady_clock;
  using time_point = clock_type::time_point;
  using duration = clock_type::duration;
  template <typename Signature>
  using move_only_function = aa::any_with<aa::call<Signature>, aa::move>;

  struct impl;

 private:
  std::shared_ptr<impl> m_impl;

 public:
  timer_t(boost::asio::io_context&);

  // arms timer to execute callback after 'd'
  // if timer was armed, its canceled first
  void arm(duration d);

  // arms timer to execute callback on specified time point
  // if timer was armed, its canceled first
  void arm(time_point);

  // repeat interface of old timer, same as 'arm'
  void rearm(duration d) {
    arm(d);
  }
  void rearm(time_point t) {
    arm(t);
  }

  // arms timer after 'd' and repeats this each 'd'
  // new arm will be after executing task
  // if timer was armed, its canceled first
  void arm_periodic(duration d);

  [[nodiscard]] bool armed() const noexcept;

  // returns 'true' if timer was armed before 'cancel'
  // do not touches setted callback
  bool cancel() noexcept;

  void set_callback(move_only_function<void()>);
};

}  // namespace http2
