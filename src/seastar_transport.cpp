

#include "http2v2/seastar_transport.hpp"

#include "http2v2/logger.hpp"
#include "http2v2/signewaiter_signal.hpp"
#include "http2v2/utils/seastar_future_awaiter.hpp"

#include <queue>

#include <kelcoro/job.hpp>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/coroutine/switch_to.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/later.hh>
#include <yacore/net/DnsResolver.hpp>
#include <yacore/transport/Exceptions.hpp>
#include <yacore/utils/scope_exit.hpp>

namespace http2v2 {

static seastar::future<seastar::connected_socket>
connect_socket_impl(sockaddr_or_fqdn_t addr, seastar::socket_address srcaddr,
                    seastar::socket_address &sockaddr) {
  sockaddr = co_await addr.resolve();
  if (sockaddr.is_unspecified()) {
    throw std::runtime_error(fmt::format("[HTTP2] cannot resolve addr"));
  }
  co_return co_await seastar::connect(sockaddr, srcaddr, {});
}

static dd::task<seastar::connected_socket>
connect_socket(sockaddr_or_fqdn_t addr, seastar::socket_address srcaddr,
               deadline_t deadline, seastar::socket_address &sockaddr) {
  seastar::connected_socket sock = co_await wait_future(
      connect_socket_impl(std::move(addr), srcaddr, sockaddr), deadline);
  co_return sock;
}

struct writer_node {
  std::coroutine_handle<> callback;
  std::span<char const> data;
  io_error_code *ec = nullptr;
};

struct seastar_connection::impl
    : std::enable_shared_from_this<seastar_connection::impl> {
  seastar::connected_socket sock;
  seastar::input_stream<char> input;
  seastar::output_stream<char> output;
  // seastar output forbids more that one operation at once,
  // this queue guarantees only one writer at one time
  std::queue<writer_node> writersqueue;
  singlewaiter_signal workexist;
  seastar::gate gate;

  impl(seastar::connected_socket s, seastar_connection_options opts) noexcept
      : sock(std::move(s)),
        input(sock.input({.buffer_size = opts.receiveBufferSize})),
        output(sock.output(opts.sendBufferSize)) {
    assert(sock);
    sock.set_nodelay(!opts.mergeSmallRequests);
  }
  // Note: no destructor, destroyed in shutdown in curious way
};

static dd::job start_seastar_writer_for(
    seastar::shared_ptr<seastar_connection::impl> impl) try {
  assert(impl);
  ON_SCOPE_EXIT {
    // cancel all writes
    while (!impl->writersqueue.empty()) {
      writer_node n = impl->writersqueue.front();
      impl->writersqueue.pop();
      *n.ec = io_error_code_e::operation_aborted;
      n.callback.resume();
    }

    HTTP2_LOG(TRACE, "seastar writer ended");
  };

  seastar::gate::holder guard = impl->gate.hold();
  while (!impl->gate.is_closed()) {
    co_await impl->workexist.wait();

    while (!impl->writersqueue.empty() && !impl->gate.is_closed()) {
      writer_node n = impl->writersqueue.front();

      auto write = wait_future_noexcept(
          impl->output.write((char const *)n.data.data(), n.data.size()));
      co_await write;
      if (write.f.failed()) [[unlikely]] {
        if (impl->gate.is_closed()) {
          // node will be canceled
          co_return;
        }
        *n.ec = io_error_code_e::network_unreachable;
        HTTP2_LOG(ERROR, "seastar writer: write ended with exception: {}",
                  write.f.get_exception());
      }
      // pop only here to assume operation canceled on bad_alloc for coroutine
      impl->writersqueue.pop();
      n.callback.resume();
    }
    // flush even if gate is closed already
    if (seastar::future f = co_await wait_future_noexcept(impl->output.flush());
        f.failed()) [[unlikely]]
    {
      HTTP2_LOG(ERROR, "seastar writer: flush ended with exception: {}",
                f.get_exception());
    }
  }
} catch (std::bad_alloc &) {
  // try log anyway
  HTTP2_LOG(ERROR, "seastar writer failed: bad_alloc");
}

seastar_connection::seastar_connection(seastar::connected_socket s,
                                       seastar_connection_options opts) {
  assert(s);
  m_impl = seastar::make_shared<impl>(std::move(s), opts);
  (void)start_seastar_writer_for(m_impl);
}

seastar_connection::~seastar_connection() {
  seastar::engine().run_in_background(doshutdown());
}

dd::task<seastar_connection> seastar_connection::create(
    sockaddr_or_fqdn_t addr, seastar::socket_address srcaddr,
    seastar_connection_options opts, deadline_t deadline) {
  seastar::socket_address dstaddr;
  seastar::connected_socket sock =
      co_await connect_socket(std::move(addr), srcaddr, deadline, dstaddr);
  (void)
      dstaddr; // 0nly reason for this is
               // https://gitlab.tel.yadro.com/tel/yacore/yacore/-/merge_requests/6416
  co_return seastar_connection(std::move(sock), opts);
}

static dd::job do_read_nossl(seastar::shared_ptr<seastar_connection::impl> in,
                             std::span<byte_t> buf, io_error_code &ec,
                             std::coroutine_handle<> callback) {
  ON_SCOPE_EXIT { in->gate.leave(); };
  try {
    if (buf.size() == 0) {
      callback();
      co_return;
    }
    seastar::temporary_buffer<char> res =
        co_await wait_future(in->input.read_exactly(buf.size()));
    if (res.size() < buf.size()) {
      // Note: it may be 'eof', which may not be network error, (so read is
      // cancelled) but in most cases its network error
      ec = io_error_code_e::network_unreachable;
    }
    // TODE inline into client and rm memcpy
    // (pass buf, use callback)
    std::memcpy(buf.data(), res.begin(), res.size());
  } catch (std::exception &e) {
    HTTP2_LOG(ERROR, "non-tls read failed with exception: {}", e.what());
    ec = io_error_code_e::network_unreachable;
  }
  callback();
}

void seastar_connection::startRead(std::coroutine_handle<> callback,
                                   std::span<byte_t> buf, io_error_code &ec) {
  if (!m_impl || !m_impl->gate.try_enter()) [[unlikely]] {
    ec = io_error_code_e::operation_aborted;
    callback();
    return;
  }
  // bad alloc case
  ON_SCOPE_FAILURE(leave) { m_impl->gate.leave(); };
  do_read_nossl(m_impl, buf, ec, callback);
  leave.noLongerNeeded();
}

void seastar_connection::startWrite(std::coroutine_handle<> callback,
                                    std::span<byte_t const> buf,
                                    io_error_code &ec, size_t &written) {
  if (!m_impl) [[unlikely]] {
    ec = io_error_code_e::operation_aborted;
    callback();
    return;
  }
  written = buf.size(); // no way to get less
  m_impl->writersqueue.push(writer_node{
      .callback = callback,
      .data = std::span<char const>((char const *)buf.data(), buf.size()),
      .ec = &ec,
  });
  m_impl->workexist.notify();
}

seastar::future<> seastar_connection::doshutdown() noexcept {
  if (!m_impl) {
    return seastar::make_ready_future();
  }
  seastar::future f = m_impl->gate.close();
  return [](seastar::shared_ptr<seastar_connection::impl> i,
            seastar::future<> closegate) -> seastar::future<void> {
    try {
      // give seastar time to really send data. Even after .write() + .flush()
      // after dropping connection seastar not guarantees, that remote peer will
      // observe it
      co_await seastar::sleep(std::chrono::milliseconds(1));
      // close input first, since input may provoke output
      i->workexist.notify();
      i->sock.shutdown_input();
      i->sock.shutdown_output();
      co_await std::move(closegate);
      assert(i->writersqueue.empty());
      try {
        co_await i->output.close();
      } catch (std::exception &e) {
        HTTP2_LOG(ERROR, "shutdown output ended with exception: {}", e);
      }
      try {
        co_await i->input.close();
      } catch (std::exception &e) {
        HTTP2_LOG(ERROR, "shutdown input ended with exception: {}", e);
      }
      try {
        co_await i->sock.wait_input_shutdown();
      } catch (std::exception &e) {
        HTTP2_LOG(ERROR, "wait input shutdown ended with exception: {}", e);
      }
      i->input = {};
      i->output = {};
      i->sock = {};
    } catch (std::exception &e) {
      HTTP2_LOG(ERROR, "shutdown ended with exception: {}", e);
    } catch (...) {
      HTTP2_LOG(ERROR, "shutdown ended with unknown exception");
    }
  }(std::move(m_impl), std::move(f));
}

void seastar_connection::shutdown() noexcept {
  seastar::engine().run_in_background(doshutdown());
}

dd::task<any_connection_t>
seastar_transport::createConnection(sockaddr_or_fqdn_t addr,
                                    seastar::socket_address srcaddr,
                                    deadline_t deadline) {
  auto socket = co_await seastar_connection::create(std::move(addr), srcaddr,
                                                    options, deadline);
  std::unique_ptr<connection_i> con(new seastar_connection(std::move(socket)));
  co_return con;
}

dd::task<void> seastar_transport::sleep(duration_t d, io_error_code &ec) {
  try {
    co_await wait_future(seastar::sleep_abortable(d, abortsrc));
  } catch (seastar::sleep_aborted &) {
    ec = io_error_code_e::operation_aborted;
  }
}

dd::task<any_connection_t> seastar_tls_connection::create(
    sockaddr_or_fqdn_t addr, seastar::socket_address srcaddr,
    deadline_t deadline, transport::ClientSecurity params, tlserrcb_t cb) {
  seastar::socket_address dstaddr;
  seastar::connected_socket sock =
      co_await connect_socket(std::move(addr), srcaddr, deadline, dstaddr);
  transport::client_tls_params args{
      .sec = params, .sock = std::move(sock), .remoteAddr = dstaddr};
  std::unique_ptr t =
      co_await wait_future(transport::TlsTransport::connect(std::move(args)));
  co_return std::unique_ptr<connection_i>(
      new seastar_tls_connection(std::move(t), std::move(cb)));
}

seastar::future<any_connection_t>
seastar_tls_connection::accept(transport::server_tls_params params,
                               tlserrcb_t cb) {
  std::unique_ptr transport =
      co_await transport::TlsTransport::accept(std::move(params));
  co_return std::unique_ptr<connection_i>(
      new seastar_tls_connection(std::move(transport), std::move(cb)));
}

static dd::task<bool> read_exactly(transport::TlsTransport &in,
                                   std::span<byte_t> buf) {
  size_t readen = 0;
  while (readen != buf.size()) {
    size_t sz = co_await wait_future(in.recv(
        std::span<char>((char *)(buf.data() + readen), buf.size() - readen)));
    if (sz == 0) {
      co_return false;
    }
    readen += sz;
  }
  co_return true;
}

static dd::job do_read(std::shared_ptr<transport::TlsTransport> in,
                       std::span<byte_t> buf, io_error_code &ec,
                       seastar::shared_ptr<tlserrcb_t> cb,
                       std::coroutine_handle<> callback) {
  assert(in);
  bool success = false;
  try {
    success = co_await read_exactly(*in, buf);
  } catch (transport::tls_connection_error const &ex) {
    HTTP2_LOG(ERROR, "TLS read ended with exception, tlserr: {}, err: {}",
              (int)ex.getTlsError(), ex.what());
    if (cb && *cb) {
      (*cb)(ex);
    }
  } catch (std::exception &e) {
    HTTP2_LOG(ERROR, "TLS read ended with exception: {}", e.what());
  }
  if (!success) {
    ec = io_error_code_e::network_unreachable;
  }
  callback();
}

void seastar_tls_connection::startRead(std::coroutine_handle<> callback,
                                       std::span<byte_t> buf,
                                       io_error_code &ec) {
  if (!m_impl) [[unlikely]] {
    ec = io_error_code_e::operation_aborted;
    callback();
    return;
  }
  do_read(m_impl, buf, ec, m_tlserrcb, callback);
}

void seastar_tls_connection::startWrite(std::coroutine_handle<> callback,
                                        std::span<byte_t const> buf,
                                        io_error_code &ec, size_t &written) {
  if (!m_impl) [[unlikely]] {
    ec = io_error_code_e::operation_aborted;
    callback();
    return;
  }
  if (buf.size() == 0) [[unlikely]] {
    callback();
    return;
  }
  bool success = false;
  try {
    success = m_impl->send(
        std::span<char const>((char const *)(buf.data()), buf.size()));
  } catch (transport::tls_connection_error const &ex) {
    HTTP2_LOG(ERROR, "TLS write ended with exception, tlserr: {}, err: '{}'",
              (int)ex.getTlsError(), ex.what());
    if (m_tlserrcb && *m_tlserrcb) {
      (*m_tlserrcb)(ex);
    }
  } catch (std::exception &e) {
    HTTP2_LOG(ERROR, "TLS write ended with exception: {}", e.what());
  }
  if (success) {
    written = buf.size();
  } else {
    ec = io_error_code_e::network_unreachable;
  }
  callback();
}

dd::task<any_connection_t>
seastar_tls_transport::createConnection(sockaddr_or_fqdn_t addr,
                                        seastar::socket_address srcaddr,
                                        deadline_t deadline) {
  return seastar_tls_connection::create(std::move(addr), srcaddr, deadline,
                                        secparams, cb);
}

dd::task<void> seastar_tls_transport::sleep(duration_t d, io_error_code &ec) {
  seastar::future f =
      co_await wait_future_noexcept(seastar::sleep_abortable(d, abortsrc));
  if (f.failed()) {
    ec = io_error_code_e::operation_aborted;
  }
}

void seastar_tls_connection::shutdown() noexcept {
  seastar::engine().run_in_background(doshutdown());
}

seastar::future<> seastar_tls_connection::doshutdown() noexcept {
  if (!m_impl) {
    return seastar::make_ready_future();
  }
  std::shared_ptr i = std::exchange(m_impl, nullptr);
  if (m_tlserrcb) {
    // assume we not invoke callback after shutdown
    *m_tlserrcb = nullptr;
  }
  return
      [](std::shared_ptr<transport::TlsTransport> impl) -> seastar::future<> {
        try {
          // give seastar time to really send data. Even after .write() +
          // .flush() after dropping connection seastar not guarantees, that
          // remote peer will observe it
          co_await seastar::sleep(std::chrono::milliseconds(1));
          co_await impl->close();
        } catch (std::exception &e) {
          HTTP2_LOG(ERROR, "TLS shutdown ended with exception: {}", e);
        } catch (...) {
          HTTP2_LOG(ERROR, "TLS shutdown ended with unknown exception");
        }
      }(std::move(i));
}

seastar_tls_connection::~seastar_tls_connection() {
  seastar::engine().run_in_background(doshutdown());
}

} // namespace http2v2
