#include "http2/asio/factory.hpp"
#include "http2/asio/asio_executor.hpp"
#include "http2/asio/awaiters.hpp"
#include "http2/logger.hpp"
#include "kelcoro/job.hpp"

#include <boost/asio/read.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>

namespace {

struct work_awaiter {
  http2::noexport::single_writer_guarantee* i = nullptr;
  ZAL_PIN;

  bool await_ready() const noexcept {
    return !i->writersqueue.empty();
  }
  void await_suspend(std::coroutine_handle<dd::job_promise> writer) noexcept {
    assert(i->writer == nullptr);
    i->writer = writer;
  }
  static void await_resume() noexcept {
  }
};

}  // namespace

namespace http2 {

// Гарантирует:
// * writer либо в wait_work либо в net.write, никогда не завершается сам
// * если в очереди ничего нет, значит ничего не пишется и сейчас нет активного write
static dd::job start_inner_writer_for(auto* self) {
  // terminates on bad alloc
  assert(self);

  on_scope_exit {
    assert(self->writedata.writersqueue.empty());
  };
  noexport::single_writer_guarantee& wd = self->writedata;
  for (;;) {
    co_await work_awaiter(&wd);

    while (!wd.writersqueue.empty()) {
      writer_node& n = wd.writersqueue.front();

      co_await net.write(self->sock, n.data, n.ec);
      // убираем из очереди только здесь, чтобы гарантировать для shutdown
      // что пустая очередь == обработанная очередь
      self->writedata.writersqueue.pop_front();
      // 'ec' обрабатывает callback
      n.callback.resume();
      if (!self->writedata.allow_write) {
        wd.writersqueue.clear_and_dispose([](writer_node* node) {
          node->ec = boost::asio::error::operation_aborted;
          node->callback.resume();
        });
        // встаём на ожидание .destroy
        co_await work_awaiter(&wd);
        http2::unreachable();
      }
    }
    // TODO experiment flush
  }
}

[[nodiscard]] static bool do_try_read(noexport::single_read_assumption& rd, std::span<byte_t> buf) noexcept {
  size_t avail = rd.readen_end - rd.readen_start;
  bool b = avail >= buf.size();
  if (b) {
    memcpy(buf.data(), rd.readen_start, buf.size());
    rd.readen_start += buf.size();
  }
  return b;
}

static dd::job do_read_some(auto& c, std::span<byte_t> userbuf, io_error_code& ec,
                            std::coroutine_handle<> callback) {
  size_t readen = 0;
  const size_t userbufsz = userbuf.size();
  byte_t* const userbufend = userbuf.data() + userbufsz;
  while (readen < userbufsz) {
    readen += co_await net.read_some_many(c.sock, ec, std::span(userbuf.data() + readen, userbufend),
                                          std::span(c.readdata.readen));
    if (ec) [[unlikely]]
      break;
  }
  c.readdata.readen_end += readen - userbufsz;
  co_await dd::this_coro::destroy_and_transfer_control_to(callback);
}

static void do_start_read(auto& self, std::coroutine_handle<> h, std::span<byte_t> buf, io_error_code& ec) {
  // assumes only one reader at one time
  size_t avail = self.readdata.readen_end - self.readdata.readen_start;
  assert(avail < buf.size());  // start_read must be invoked only if try_read failed
  memcpy(buf.data(), self.readdata.readen_start, avail);
  self.readdata.readen_start = self.readdata.readen_end = self.readdata.readen;
  (void)do_read_some(self, suffix(buf, buf.size() - avail), ec, h);
}

static size_t do_try_write(auto& self, std::span<const byte_t> buf, io_error_code& ec) {
  if (!self.writedata.allow_write) [[unlikely]] {
    ec = boost::asio::error::operation_aborted;
    return 0;
  }
  // нельзя писать когда есть кто-то в writersqueue, чтобы не нарушить порядок отправки
  if (!self.writedata.writersqueue.empty())
    return 0;
  // TODO эксперимент мб маленькие данные пихать сюда, может таким образом батчинг будет ускорять
  size_t written = self.sock.write_some(asio::buffer(buf.data(), buf.size()), ec);
  if (ec) {
    if (ec == asio::error::would_block)
      ec.clear();  // not a error
  }
  return written;
}

static void do_start_write(noexport::single_writer_guarantee& g, writer_node* n) {
  assert(n);
  assert(g.allow_write);
  g.writersqueue.push_back(*n);
  g.notify_writer();
}

static void close_tcp_sock(auto& tcp_sock) {
  if (!tcp_sock.is_open())
    return;
  io_error_code ec;
  ec = tcp_sock.cancel(ec);
  // dont stop on errors, i need to stop connection somehow
  (void)ec;
  // Do not do SSL shutdown, useless errors and wasting time
  ec = tcp_sock.shutdown(asio::socket_base::shutdown_both, ec);
  (void)ec;
  ec = tcp_sock.close(ec);
  (void)ec;
}

dd::task<void> do_shutdown(noexport::single_writer_guarantee& wd, auto& sock) {
  if (!wd.allow_write) {
    assert(wd.writer == nullptr);
    co_return;
  }
  // запрещает новые try_write/start_write
  wd.allow_write = false;
  // либо writer сейчас работает, либо уже done
  // (т.к. любой write бы его разбудил и он бы никогда не заснул пока не обработает всё)
  while (!wd.writer_done())
    co_await yield_on_asio_ioctx(sock.get_executor());
  std::exchange(wd.writer, nullptr).destroy();
  close_tcp_sock(sock);
}

asio_tls_connection::asio_tls_connection(asio::ip::tcp::socket s, ssl_context_ptr ctx)
    // Note: order
    : sock(std::move(s), ctx->ctx), sslctx(std::move(ctx)) {
  sock.lowest_layer().non_blocking(true);
  (void)start_inner_writer_for(this);
}

bool asio_tls_connection::try_read(std::span<byte_t> buf) noexcept {
  return do_try_read(readdata, buf);
}

void asio_tls_connection::start_read(std::coroutine_handle<> h, std::span<byte_t> buf, io_error_code& ec) {
  do_start_read(*this, h, buf, ec);
}

size_t asio_tls_connection::try_write(std::span<const byte_t> buf, io_error_code& ec) noexcept {
  return do_try_write(*this, buf, ec);
}

void asio_tls_connection::start_write(writer_node* n) {
  do_start_write(writedata, n);
}

dd::task<void> asio_tls_connection::shutdown() noexcept {
  return do_shutdown(writedata, sock.lowest_layer());
}

asio_connection::asio_connection(asio::ip::tcp::socket s) : sock(std::move(s)) {
  sock.non_blocking(true);
  (void)start_inner_writer_for(this);
}

bool asio_connection::try_read(std::span<byte_t> buf) noexcept {
  return do_try_read(readdata, buf);
}

void asio_connection::start_read(std::coroutine_handle<> h, std::span<byte_t> buf, io_error_code& ec) {
  do_start_read(*this, h, buf, ec);
}

size_t asio_connection::try_write(std::span<const byte_t> buf, io_error_code& ec) noexcept {
  return do_try_write(*this, buf, ec);
}

void asio_connection::start_write(writer_node* n) {
  do_start_write(writedata, n);
}

dd::task<void> asio_connection::shutdown() noexcept {
  return do_shutdown(writedata, sock);
}

any_io_context make_asio_io_context(asio::io_context& ctx, tcp_connection_options opts) {
  return any_io_context(aa::inplaced{[&] { return asio_ref_factory(ctx, std::move(opts)); }});
}

any_io_context make_asio_io_context(tcp_connection_options opts) {
  return any_io_context(aa::inplaced{[&] { return asio_factory(std::move(opts)); }});
}

any_io_context make_asio_tls_io_context(asio::io_context& ctx, ssl_context_ptr ssl,
                                        tcp_connection_options opts) {
  if (ssl)
    return any_io_context(
        aa::inplaced{[&] { return asio_tls_ref_factory(ctx, std::move(ssl), std::move(opts)); }});
  else {
    return make_asio_io_context(ctx, std::move(opts));
  }
}
// TODO rename (одна функция просто по дефолту = nullptr)
any_io_context make_asio_tls_io_context(ssl_context_ptr ssl, tcp_connection_options opts) {
  if (ssl)
    return any_io_context(aa::inplaced{[&] { return asio_tls_factory(std::move(ssl), std::move(opts)); }});
  else
    return make_asio_io_context(std::move(opts));
}

any_io_context make_asio_tls_io_context(std::vector<std::filesystem::path> certs) {
  tcp_connection_options options;
  options.additional_ssl_certificates = std::move(certs);
  return any_io_context(aa::inplaced{[&] { return asio_tls_factory(std::move(options)); }});
}

static dd::task<any_connection_t> do_create_connection_client(auto& self, endpoint ep, deadline_t deadline) {
  using tcp = asio::ip::tcp;

  tcp::resolver resolver(self.ioctx);

  asio_timer timer(self.ioctx);
  bool timeoutflag = false;

  timer.arm(deadline);
  timer.set_callback([&](bool canceled) {
    if (!canceled) {
      timeoutflag = true;
      resolver.cancel();
    }
  });

  io_error_code ec;
  auto results = co_await net.resolve(resolver, ep, ec);
  if (timeoutflag)
    throw timeout_exception();

  if (results.empty() || ec)
    throw network_exception("[TCP] cannot resolve host: {}, err: {}", ep.to_string(), ec.message());
  tcp::socket tcp_sock(self.ioctx);

  timer.cancel();
  timer.arm(deadline);
  timer.set_callback([&](bool canceled) {
    if (!canceled) {
      timeoutflag = true;
      close_tcp_sock(tcp_sock);
    }
  });

  co_await net.connect(tcp_sock, results, ec);

  if (ec)
    throw network_exception("[TCP] cannot connect to {}, err: {}", ep.to_string(), ec.message());
  if (self.starter)
    co_await self.starter(tcp_sock, deadline);
  self.options.apply(tcp_sock);
  co_return any_connection_t(new asio_connection(std::move(tcp_sock)));
}

asio_factory::asio_factory(tcp_connection_options opts, starter_t s)
    : options(std::move(opts)), starter(std::move(s)) {
}

dd::task<any_connection_t> asio_factory::create_connection_client(endpoint ep, deadline_t deadline) {
  return do_create_connection_client(*this, ep, deadline);
}

struct asio_acceptor {
  boost::asio::ip::tcp::acceptor a;

  internet_address get_local_endpoint() const {
    return a.local_endpoint();
  }

  internet_address listen() {
    a.listen();
    return get_local_endpoint();
  }

  dd::task<any_connection_t> accept(io_error_code& ec) {
    asio::ip::tcp::socket socket(a.get_executor());
    co_await net.accept(a, socket, ec);
    if (ec)
      co_return nullptr;
    co_return any_connection_t(new asio_connection(std::move(socket)));
  }

  void close() {
    return a.close();
  }
};

any_acceptor asio_factory::create_acceptor(internet_address addr, bool reuse_address) {
  return asio_acceptor{boost::asio::ip::tcp::acceptor{ioctx, std::move(addr), reuse_address}};
}

asio_ref_factory::asio_ref_factory(asio::io_context& ctx, tcp_connection_options opts, starter_t s)
    : asio_factory_ref_base(ctx), options(std::move(opts)), starter(std::move(s)) {
}

dd::task<any_connection_t> asio_ref_factory::create_connection_client(endpoint ep, deadline_t deadline) {
  return do_create_connection_client(*this, ep, deadline);
}

any_acceptor asio_ref_factory::create_acceptor(internet_address addr, bool reuse_address) {
  return asio_acceptor{boost::asio::ip::tcp::acceptor{ioctx, std::move(addr), reuse_address}};
}

// TLS

static dd::task<any_connection_t> do_create_connection_client_tls(auto& self, endpoint endpoint,
                                                                  deadline_t deadline) {
  namespace ssl = asio::ssl;
  using tcp = asio::ip::tcp;

  tcp::resolver resolver(self.ioctx);

  asio_timer timer(self.ioctx);
  bool timeoutflag = false;

  timer.arm(deadline);
  timer.set_callback([&](bool canceled) {
    if (!canceled) {
      timeoutflag = true;
      resolver.cancel();
    }
  });

  io_error_code ec;
  auto results = co_await net.resolve(resolver, endpoint, ec);
  if (timeoutflag)
    throw timeout_exception();
  if (results.empty() || ec)
    throw network_exception("[TCP] cannot resolve host: {}, err: {}", endpoint.to_string(), ec.what());
  asio::ip::tcp::socket tcp_sock(self.ioctx);

  timer.cancel();
  timer.arm(deadline);
  timer.set_callback([&](bool canceled) {
    if (!canceled) {
      timeoutflag = true;
      close_tcp_sock(tcp_sock);
    }
  });

  co_await net.connect(tcp_sock, std::move(results), ec);

  if (timeoutflag)
    throw timeout_exception();
  if (ec)
    throw network_exception("[TCP] cannot connect to {}, err: {}", endpoint.to_string(), ec.message());
  if (self.starter)
    co_await self.starter(tcp_sock, deadline);
  self.options.apply(tcp_sock);
  assert(self.sslctx);
  std::unique_ptr<asio_tls_connection> res(new asio_tls_connection(std::move(tcp_sock), self.sslctx));
  if (self.options.host_for_name_verification) {
    res->sock.set_verify_mode(ssl::verify_peer);
    res->sock.set_verify_callback(
        asio::ssl::host_name_verification(*self.options.host_for_name_verification));
  } else {
    res->sock.set_verify_mode(ssl::verify_none);
  }
  if (!self.options.is_primal_connection)
    SSL_set_mode(res->sock.native_handle(), SSL_MODE_RELEASE_BUFFERS);
  co_await net.handshake(res->sock, ssl::stream_base::handshake_type::client, ec);
  if (timeoutflag)
    throw timeout_exception();
  if (ec)
    throw network_exception("[TCP/SSL] cannot ssl handshake: {}", ec.message());
  co_return any_connection_t(std::move(res));
}

asio_tls_factory::asio_tls_factory(tcp_connection_options opts, starter_t s)
    : asio_tls_factory(make_ssl_context_for_http2(opts.additional_ssl_certificates), opts, std::move(s)) {
}

asio_tls_factory::asio_tls_factory(ssl_context_ptr ctx, tcp_connection_options opts, starter_t s)
    : asio_factory_base(), options(std::move(opts)), sslctx(std::move(ctx)), starter(std::move(s)) {
  assert(sslctx != nullptr);
}

dd::task<any_connection_t> asio_tls_factory::create_connection_client(endpoint endpoint,
                                                                      deadline_t deadline) {
  return do_create_connection_client_tls(*this, endpoint, deadline);
}

struct asio_tls_acceptor {
  boost::asio::ip::tcp::acceptor a;
  ssl_context_ptr sslctx;

  internet_address get_local_endpoint() const {
    return a.local_endpoint();
  }

  internet_address listen() {
    a.listen();
    return get_local_endpoint();
  }

  dd::task<any_connection_t> accept(io_error_code& ec) {
    asio::ip::tcp::socket socket(a.get_executor());

    co_await net.accept(a, socket, ec);
    if (ec)
      co_return nullptr;
    std::unique_ptr<asio_tls_connection> tcpcon(new asio_tls_connection(std::move(socket), sslctx));
    co_await net.handshake(tcpcon->sock, asio::ssl::stream_base::server, ec);
    if (ec)
      co_return nullptr;
    co_return any_connection_t(std::move(tcpcon));
  }

  void close() {
    return a.close();
  }
};

any_acceptor asio_tls_factory::create_acceptor(internet_address addr, bool reuse_address) {
  return asio_tls_acceptor{boost::asio::ip::tcp::acceptor{ioctx, std::move(addr), reuse_address}, sslctx};
}

asio_tls_ref_factory::asio_tls_ref_factory(asio::io_context& ctx, tcp_connection_options opts, starter_t s)
    : asio_tls_ref_factory(ctx, make_ssl_context_for_http2(opts.additional_ssl_certificates), opts,
                           std::move(s)) {
}

asio_tls_ref_factory::asio_tls_ref_factory(asio::io_context& ctx, ssl_context_ptr ssl,
                                           tcp_connection_options opts, starter_t s)
    : asio_factory_ref_base(ctx), options(std::move(opts)), sslctx(std::move(ssl)), starter(std::move(s)) {
  assert(sslctx != nullptr);
}

any_acceptor asio_tls_ref_factory::create_acceptor(internet_address addr, bool reuse_address) {
  return asio_tls_acceptor{boost::asio::ip::tcp::acceptor{ioctx, std::move(addr), reuse_address}, sslctx};
}

dd::task<any_connection_t> asio_tls_ref_factory::create_connection_client(endpoint endpoint,
                                                                          deadline_t deadline) {
  return do_create_connection_client_tls(*this, endpoint, deadline);
}

}  // namespace http2
