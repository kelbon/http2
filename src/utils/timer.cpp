#include "http2/utils/timer.hpp"

#include "http2/errors.hpp"

#include <boost/asio/steady_timer.hpp>

namespace http2 {

struct timer_t::impl {
  boost::asio::steady_timer timer;
  std::optional<duration> period;
  move_only_fn_soos<void()> fn;
  bool armed = false;

  explicit impl(boost::asio::io_context& ctx) : timer(ctx) {
  }
};

timer_t::timer_t(boost::asio::io_context& ctx) : m_impl(std::make_shared<timer_t::impl>(ctx)) {
}

void timer_t::arm(duration d) {
  arm(std::chrono::steady_clock::now() + d);
}

struct callback_t {
  std::weak_ptr<timer_t::impl> w;

  void operator()(const io_error_code& ec) {
    auto x = w.lock();
    if (!x)
      return;
    if (ec) {
      x->armed = false;
      return;
    }
    if (x->fn)
      x->fn();
    if (!x->period) {
      x->armed = false;
      return;
    }
    x->timer.expires_after(*x->period);
    x->timer.async_wait(callback_t(std::move(w)));
  }
};

void timer_t::arm(time_point tp) {
  cancel();
  m_impl->timer.expires_at(tp);
  m_impl->armed = true;
  m_impl->timer.async_wait(callback_t(m_impl));
}

void timer_t::arm_periodic(duration d) {
  arm(d);
  m_impl->period = d;
}

bool timer_t::armed() const noexcept {
  return m_impl->armed;
}

bool timer_t::cancel() noexcept {
  if (!armed())
    return false;
  m_impl->period = std::nullopt;
  m_impl->armed = false;
  m_impl->timer.cancel();
  return true;
}

void timer_t::set_callback(move_only_fn_soos<void()> fn) {
  m_impl->fn = std::move(fn);
}

}  // namespace http2
