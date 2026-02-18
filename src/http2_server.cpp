
#include "http2/http2_server.hpp"

#include "http2/asio/asio_executor.hpp"
#include "http2/asio/factory.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/http2_server_session.hpp"

#include <exception>
#include <latch>
#include <list>

#include <http2/http2_connection.hpp>
#include <http2/http2_connection_establishment.hpp>
#include <http2/http2_protocol.hpp>
#include <http2/http2_server_reader.hpp>
#include <http2/http2_writer.hpp>
#include <http2/logger.hpp>
#include <http2/utils/reusable_buffer.hpp>
#include <http2/asio/awaiters.hpp>

#include <kelcoro/common.hpp>
#include <kelcoro/algorithm.hpp>

#include <zal/zal.hpp>

#include "http2/asio/aio_context.hpp"
#include <boost/asio/ip/tcp.hpp>

/*

Предназначение сервера это управление соединениями. Вся логика по обработке соединений с клиентами внутри
server_session

listen добавляет serverAddress в прослушиваемые и создаёт корутину acceptConnections слушающую этот адрес
    Note: эти корутины останавливаются после stopListeners, но адреса продолжают висеть вплоть до удаления
сервера. "Так сложилось"

acceptConnections вечно создаёт сокеты прослушивая адрес пока не получит специальную ошибку означающую отмену
слушания При получении сокета создаёт корутину sessionLifecycle

sessionLifecycle устанавливает соединение уровнем выше TCP (tls/http2), задаёт нужные настройки и далее
служит жизненным пространством для server_session, которая в свою очередь обёртка над h2connection
server_session завершается когда читатель отдаёт управление, например при получении GOAWAY фрейма

Другой путь завершения server_session это методы сервера shutdown/terminate

shutdown отсылает goaway клиенту на всех соединениях и ждёт завершения всех соединений

terminate отсылает goaway и отменяет все запросы на всех соединениях, затем ждёт завершения соединений

*/

namespace http2 {

using acceptor_t = boost::asio::ip::tcp::acceptor;

struct http2_server::impl {
  // on top bcs of destroy order
  asio::io_context io;
  bi::list<server_session> sessions;
  ssl_context_ptr sslctx = nullptr;  // if nullptr, then server is http (not https)
  std::list<acceptor_t> listeners;
  // gate for opened sessions / acceptors
  dd::gate sessionsgate;
  http2_server_options options;
  http2_server* creator = nullptr;
  tcp_connection_options tcpopts;
  move_only_fn<void(asio::ip::tcp::socket)> acceptcb;
#ifndef NDEBUG
  std::thread::id tid = std::this_thread::get_id();
#endif
  asio::io_context& ioctx() {
    return io;
  }

  const log_context& logctx() const noexcept {
    return options.logctx;
  }

  explicit impl(tcp_connection_options tcpopts, http2_server_options opts)
      : io(), options(std::move(opts)), tcpopts(std::move(tcpopts)) {
    options.logctx.name = unique_name{};  // generate new (for different names for each server in mt_server)
    options.logctx.name.set_prefix(SERVER_PREFIX);
  }

  internet_address listen(server_endpoint a) {
    assert(std::this_thread::get_id() == tid);
    acceptor_t& acceptor = listeners.emplace_back(ioctx(), a.addr, a.reuse_address);
    // store resolved endpoint (e.g. if port 0 was used) and store it before acceptConnections
    // (acceptConnections may delete acceptor!)
    internet_address binded = acceptor.local_endpoint();
    acceptor.listen();
    auto lit = std::prev(listeners.end());
    on_scope_failure(eraselistener) {
      listeners.erase(lit);
    };
    acceptConnections(sessionsgate.hold(), lit).start_and_detach();
    eraselistener.no_longer_needed();
    HTTP2_LOG(logctx(), INFO, "Server listening on {}:{}", binded.address().to_string(), a.addr.port());
    return binded;
  }

  dd::task<void> acceptConnections(dd::gate::holder, decltype(listeners)::iterator lit) try {
    assert(std::this_thread::get_id() == tid);
    assert(lit != listeners.end());
    on_scope_exit {
      HTTP2_LOG_TRACE(logctx(), "stops listening");
      listeners.erase(lit);
    };
    std::string addrstr = [&] {
      try {
        return lit->local_endpoint().address().to_string();
      } catch (...) {
        return std::string();
      }
    }();
    // note: do not remove listener on scope exit
    while (!sessionsgate.is_closed()) {
      io_error_code ec;
      asio::ip::tcp::socket socket(ioctx());
      co_await net.accept(*lit, socket, ec);
      assert(std::this_thread::get_id() == tid);
      if (ec == asio::error::operation_aborted) {
        HTTP2_LOG_TRACE(logctx(), "listening on {} stopped", addrstr);
        if (sessionsgate.is_closed())
          co_return;
        else
          continue;
      }
      if (ec) {
        HTTP2_LOG(logctx(), ERROR, "accept failed on {}, err: {}", addrstr, ec.message());
        if (sessionsgate.is_closed())
          co_return;
        else
          continue;
      }
      HTTP2_LOG_TRACE(logctx(), "accepted connection");
      if (!sessionsgate.is_closed()) {
        if (!acceptcb)
          sessionLifecycle(sessionsgate.hold(), std::move(socket)).start_and_detach();
        else
          acceptcb(std::move(socket));
      }
    }
    HTTP2_LOG_TRACE(logctx(), "acceptConnections: gate is closed");
  } catch (std::exception& e) {
    HTTP2_LOG(logctx(), ERROR, "acceptConnections failed with err {}", e.what());
  }

  dd::task<h2connection_ptr> createConnection(asio::ip::tcp::socket socket) {
    assert(std::this_thread::get_id() == tid);
    try {
      tcpopts.apply(socket);
      if (sslctx) {
        HTTP2_LOG_TRACE(logctx(), "start TLS session");

        any_connection_t tcpcon(new asio_tls_connection(std::move(socket), sslctx));
        io_error_code ec;
        co_await net.handshake(static_cast<asio_tls_connection*>(tcpcon.get())->sock,
                               asio::ssl::stream_base::server, ec);
        if (ec) {
          HTTP2_LOG(logctx(), ERROR, "error during ssl handshake: {}", ec.message());
          co_return nullptr;
        }
        co_return new h2connection(std::move(tcpcon), ioctx());
      } else {
        HTTP2_LOG_TRACE(logctx(), "start non-tls session");
        any_connection_t tcpcon(new asio_connection(std::move(socket)));
        co_return new h2connection(std::move(tcpcon), ioctx());
      }
    } catch (std::exception const& e) {
      HTTP2_LOG(logctx(), ERROR, "connection creation failure: {}", e.what());
      co_return nullptr;
    }
  }

  // Note: this code ignores possible bad_alloc and other logs exceptions
  dd::task<void> sessionLifecycle(dd::gate::holder, asio::ip::tcp::socket socket) try {
    assert(std::this_thread::get_id() == tid);

    h2connection_ptr http2con = co_await createConnection(std::move(socket));
    if (!http2con || !creator) {
      co_return;
    }
    if (sessionsgate.is_closed()) {
      http2con->shutdown(reqerr_e::CANCELLED);
      HTTP2_LOG(logctx(), INFO, "session completed, but server stopped (server session is not created)");
      co_return;
    }

    int reader_ec = 0;

    // firstly insert session into list, so server will drop it if stops during session establishing
    server_session_ptr session_ptr = new server_session(std::move(http2con), options, *creator);
    server_session& session = *session_ptr;
    session.connection->logctx.name.set_prefix(SERVER_SESSION_PREFIX);

    session.connection->logctx.lvl = logctx().lvl;
    session.connection->logctx.dolog = logctx().dolog;

    HTTP2_LOG_TRACE(session.logctx(), "server {} new session", logctx().name);

    sessions.push_back(session);
    on_scope_exit {
      erase_byref(sessions, session);
    };

    auto sleepcb = [session_ptr](duration_t d, io_error_code& ec) -> dd::task<void> {
      asio::steady_timer timer(session_ptr->server->ioctx());
      co_await net.sleep(timer, d, ec);
    };
    auto requestTerminateInactive = [session_ptr, nm = this->logctx().name] {
      HTTP2_LOG_TRACE(session_ptr->logctx(), "{} drops connection due client inactivity", nm);
      session_ptr->requestTerminate();
    };
    auto requestTerminate = [session_ptr] {
      HTTP2_LOG_TRACE(session_ptr->logctx(), "writer drops connection");
      session_ptr->requestTerminate();
    };

    try {
      timer_t timer(ioctx());
      timer.set_callback([session_ptr] {
        HTTP2_LOG(session_ptr->logctx(), ERROR, "connection timeout");
        session_ptr->connection->shutdown(reqerr_e::TIMEOUT);
      });
      timer.arm(options.connectionTimeout);
      (void)co_await establish_http2_session_server(session.connection, options);
      session.established = true;
      timer.cancel();
      if (sessions.size() > options.limit_clients_count) [[unlikely]] {
        HTTP2_LOG(session.logctx(), WARN, "connection dropped due server`s clients limit exceeding");
        (void)co_await send_goaway(session.connection, 0, errc_e::NO_ERROR,
                                   "server's clients limit exceeded, try later");
        goto drop_session;
      }
    } catch (std::exception& e) {
      HTTP2_LOG(logctx(), ERROR, "server -> client connection establishment failed, err: {}", e.what());
      goto drop_session;
    }

    if (sessionsgate.is_closed()) {
      goto drop_session;
    }

    (void)start_writer_for_server(session.connection, sleepcb, requestTerminate, options.forceDisableHpack,
                                  session.connectionPartsGate.hold());

    session.connection->pingdeadlinetimer.set_callback(requestTerminateInactive);
    session.connection->pingtimer.set_callback([framecount = size_t(0), &session, server = this]() mutable {
      if (session.framecount != framecount) {
        framecount = session.framecount;
        return;
      }
      // nothing happens since last call
      if (!session.connection->pingdeadlinetimer.armed() && !session.hasUnfinishedRequests()) {
        HTTP2_LOG_TRACE(session.logctx(), "detect nothing happens, arm idle deadline timer");
        session.connection->pingdeadlinetimer.arm(server->options.idleTimeout);
      }
    });
    session.connection->pingtimer.arm_periodic(std::chrono::milliseconds(100));
    reader_ec = co_await start_server_reader_for(session);
    if (reader_ec != reqerr_e::DONE) {
      // give time for sending goaway
      co_await net.sleep(ioctx(), std::chrono::milliseconds(1));
    }
    HTTP2_LOG_TRACE(session.logctx(), "reader stops, waiting stop");
  drop_session:
    session.requestTerminate();
    while (session.hasUnfinishedRequests())
      co_await yield_on_ioctx(session.server->ioctx());

    // we are here if reader ended with exception or after soft shutdown (streams closed, new requests
    // forbidden)
    co_await session.connectionPartsGate.close();
    co_await session.responsegate.close();
    co_await yield_on_ioctx(ioctx());  // give `leave` callers time to finish their work
    HTTP2_LOG_TRACE(session.logctx(), "session stop ended");
  } catch (std::exception& e) {
    HTTP2_LOG(logctx(), ERROR, "session ended with exception: {}", e.what());
  }

  void stopListeners() {
    assert(std::this_thread::get_id() == tid);
    HTTP2_LOG_TRACE(logctx(), "shutdown: listeners size {}", listeners.size());
    for (auto& l : listeners) {
      l.close();
    }
    // after this function sessiongate must be closed to ensure all listeners are done
  }

  dd::task<void> shutdown() {
    co_await jump_on_ioctx(ioctx());
    assert(std::this_thread::get_id() == tid);
    HTTP2_LOG_TRACE(logctx(), "shutdown started");
    on_scope_exit {
      HTTP2_LOG_TRACE(logctx(), "shutdown ended");
    };
    auto closeg = sessionsgate.close();
    for (auto& session : sessions) {
      session.requestShutdown();
    }
    stopListeners();
    co_await closeg;
    co_await yield_on_ioctx(ioctx());
    if (sessionsgate.is_closed())  // may be another shutdown/terminate
      sessionsgate.reopen();
    assert(sessions.empty());
    assert(listeners.empty());
  }

  dd::task<void> terminate() {
    co_await jump_on_ioctx(ioctx());
    assert(std::this_thread::get_id() == tid);
    HTTP2_LOG_TRACE(logctx(), "terminate started");
    on_scope_exit {
      HTTP2_LOG_TRACE(logctx(), "terminate ended");
    };
    auto closeg = sessionsgate.close();
    for (auto& session : sessions) {
      session.requestTerminate();
    }
    stopListeners();
    co_await closeg;
    co_await yield_on_ioctx(ioctx());
    sessionsgate.reopen();
    assert(sessions.empty());
    assert(listeners.empty());
  }
};

http2_server::http2_server(ssl_context_ptr ctx, http2_server_options options, tcp_connection_options tcpopts)
    : m_impl(std::make_unique<http2_server::impl>(std::move(tcpopts), std::move(options))) {
  if (ctx) {
    m_impl->sslctx = std::move(ctx);
  }
  m_impl->creator = this;
}

http2_server::~http2_server() {
  stop();
}

void http2_server::stop() {
  assert(m_impl);
  HTTP2_LOG_TRACE(m_impl->logctx(), "~http2_server");
  m_impl->creator = nullptr;
#ifndef NDEBUG
  m_impl->tid = std::this_thread::get_id();  // change working thread
#endif
  std::coroutine_handle h = m_impl->terminate().start_and_detach(/*stop_at_end=*/true);

  on_scope_exit {
    h.destroy();
  };
  if (ioctx().stopped())
    ioctx().restart();
  try {
    // assume 'h' is suspended here every time when we check h.done()
    while (!h.done() && ioctx().run_one() != 0)
      ;
  } catch (std::exception& e) {
    HTTP2_LOG(m_impl->logctx(), ERROR, "error while ~http2_server: {}", e.what());
  }
}

void http2_server::set_accept_callback(move_only_fn<void(asio::ip::tcp::socket)> cb) {
  m_impl->acceptcb = std::move(cb);
}

size_t http2_server::sessions_count() const noexcept {
  return m_impl->sessions.size();
}

internet_address http2_server::listen(server_endpoint a) {
  return m_impl->listen(std::move(a));
}

dd::task<void> http2_server::shutdown() {
  return m_impl->shutdown();
}

dd::task<void> http2_server::terminate() {
  return m_impl->terminate();
}

asio::io_context& http2_server::ioctx() {
  return m_impl->ioctx();
}

void http2_server::request_stop() {
  shutdown().start_and_detach();
}

void http2_server::run() {
#ifndef NDEBUG
  m_impl->tid = std::this_thread::get_id();
#endif
  if (ioctx().stopped())
    ioctx().restart();
  ioctx().run();
}

http2_server_options& http2_server::get_options() noexcept {
  return m_impl->options;
}

const http2_server_options& http2_server::get_options() const noexcept {
  return m_impl->options;
}

void http2_server::set_ssl_context(ssl_context_ptr c) noexcept {
  m_impl->sslctx = std::move(c);
}

// multi threaded server

void mt_server::initialize() {
  auto cb = [this](asio::ip::tcp::socket sock) {
    auto& server = next_server().server;

    // rebind socket executor
    asio::ip::tcp::socket newsock(server->ioctx());
    io_error_code ec;
    auto p = sock.local_endpoint(ec).protocol();
    auto rawsock = sock.release(ec);
    newsock.assign(p, rawsock);
    if (ec) {
      HTTP2_LOG(server->m_impl->logctx(), ERROR, "error when transfering accepted socket, err: {}",
                ec.what());
      return;
    }
    asio::post(server->ioctx(), [&server, s = std::move(newsock)]() mutable {
      if (server->m_impl->sessionsgate.is_closed()) [[unlikely]]
        return;
      server->m_impl->sessionLifecycle(server->m_impl->sessionsgate.hold(), std::move(s)).start_and_detach();
    });
  };

  listen_server().server->set_accept_callback(cb);
}

internet_address mt_server::listen(server_endpoint e) {
  // listen always on main thread, so `listen` effects will be observable after `server::listen` return
  return listen_server().server->listen(e);
}

void mt_server::run() {
  assert(servers.size() == 1 || servers.size() == pool->queues_range().size() + 1);
  if (running)
    throw std::runtime_error("`run` already called");
  running = true;
  on_scope_exit {
    running = false;
  };
  if (listen_server().server->m_impl->listeners.empty())
    throw std::runtime_error("mt_server `run` called, but no one address listen!");
  std::latch all_done(servers.size());
  if (pool) {
    std::span qs = pool->queues_range();
    for (size_t i = 1; i != servers.size(); ++i) {
      dd::schedule_to(qs[i - 1], [&all_done, ptr = &servers[i]] {
        on_scope_exit {
          all_done.count_down();
        };
        try {
          auto guard = asio::make_work_guard(ptr->server->ioctx());
          ptr->work_guard = &guard;
          ptr->server->run();
        } catch (std::exception& e) {
          HTTP2_LOG(ptr->server->m_impl->logctx(), ERROR, "cannot schedule `run` task: err: {}", e.what());
        }
      });
    }
  }
  auto& main_server = listen_server();
  auto guard = asio::make_work_guard(main_server.server->ioctx());
  main_server.work_guard = &guard;
  main_server.server->run();
  assert(!stopping);
  all_done.arrive_and_wait();
}

void mt_server::request_stop() {
  auto do_request_stop = [](mt_server* self) mutable -> dd::task<void> {
    // run to listen thread to access to `running` only from one thread
    co_await jump_on_ioctx(self->listen_server().server->ioctx());
    if (!self->running)
      co_return;
    if (self->stopping)
      co_return;  // prevent double stop
    self->stopping = true;
    on_scope_failure(term) {
      std::terminate();
    };
    // stop listen thread (0) first to avoid creating new sessions
    auto stop1 = [](local_server_ctx& c) -> dd::task<void> {
      co_await jump_on_ioctx(c.server->ioctx());
      co_await c.server->shutdown();
      c.work_guard->reset();
      c.work_guard = nullptr;
    };
    std::vector<dd::task<void>> tasks;
    for (size_t i = 1; i < self->servers.size(); ++i)
      tasks.push_back(stop1(self->servers[i]));
    co_await self->listen_server().server->shutdown();
    (void)co_await dd::when_all(std::move(tasks));
    co_await jump_on_ioctx(self->listen_server().server->ioctx());
    self->stopping = false;
    self->listen_server().work_guard->reset();
    self->listen_server().work_guard = nullptr;
    term.no_longer_needed();
  };

  do_request_stop(this).start_and_detach();
}

}  // namespace http2
