
#include "http2/http2_server.hpp"

#include "http2/asio/factory.hpp"
#include "http2/http2_server_session.hpp"

#include <exception>
#include <list>

#include <http2/http2_connection.hpp>
#include <http2/http2_connection_establishment.hpp>
#include <http2/http2_protocol.hpp>
#include <http2/http2_server_reader.hpp>
#include <http2/http2_writer.hpp>
#include <http2/logger.hpp>
#include <http2/utils/reusable_buffer.hpp>
#include <http2/utils/seastar_future_awaiter.hpp>
#include <http2/asio/awaiters.hpp>

#include <kelcoro/common.hpp>
#include <kelcoro/algorithm.hpp>

#include <zal/zal.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#undef DELETE
#undef NO_ERROR

/*

Предназначение сервера это управление соединениями. Вся логика по обработке соединений с клиентами внутри
server_session

listen добавляет serverAddress в прослушиваемые и создаёт корутину acceptConnections слушающую этот адрес
    Note: эти корутины останавливаются после stopListeners, но адреса продолжают висеть вплоть до удаления
сервера. "Так сложилось"

acceptConnections вечно создаёт сокеты прослушивая адрес пока не получит специальную ошибку означающую отмену
слушания При получении сокета создаёт корутину sessionLifecycle

sessionLifecycle устанавливает соединение уровнем выше TCP (tls/http2), задаёт нужные настройки и далее
служит жизненным пространством для server_session, которая в свою очередь обёртка над http2_connection
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
  dd::gate gate;
  http2_server_options options;
  http2_server* creator = nullptr;

  asio::io_context& ioctx() {
    return io;
  }

  explicit impl(int boost_hint) : io(boost_hint) {
  }

  void listen(server_endpoint a) {
    acceptor_t& acceptor = listeners.emplace_back(ioctx(), a.addr, a.reuse_address);
    acceptor.listen();
    acceptConnections(gate.hold(), listeners.back()).start_and_detach();
    HTTP2_LOG(INFO, "[SERVER] is listening on {}:{}", a.addr.address().to_string(), a.addr.port());
  }

  // TODO должен быть один поток принимающий соединения и все остальные обрабатывающие соединения
  // видимо какая то абстракция как получать strand? А мб это на пользователя переложить через отдачу
  // io_context а тут просто делать стренды
  dd::task<void> acceptConnections(dd::gate::gate::holder, acceptor_t& srv) {
    on_scope_exit {
      HTTP2_LOG(TRACE, "[SERVER] stops listening");
    };
    // note: do not remove listener on scope exit
    while (!gate.is_closed()) {
      io_error_code ec;
      // there are only one acceptConnections for each address (listen), after creation connection will be
      // handled singlethreaded so strand used here for single thread guarantee
      asio::ip::tcp::socket socket(asio::make_strand(srv.get_executor()));
      co_await net.accept(srv, socket, ec);

      if (ec == asio::error::operation_aborted) {
        HTTP2_LOG(TRACE, "[SERVER] listening on {} stopped", srv.local_endpoint().address().to_string());
        co_return;
      }
      if (ec) {
        HTTP2_LOG(ERROR, "[SERVER] accept failed on {}, err: {}", srv.local_endpoint().address().to_string(),
                  ec.message());
        continue;
      }
      HTTP2_LOG(TRACE, "[SERVER] accepted connection");
      if (!gate.is_closed()) {
        sessionLifecycle(gate.hold(), std::move(socket)).start_and_detach();
      }
    }
    HTTP2_LOG(TRACE, "[SERVER] acceptConnections: gate is closed");
  }

  dd::task<http2_connection_ptr_t> createConnection(asio::ip::tcp::socket socket) {
    try {
      if (sslctx) {
        HTTP2_LOG(TRACE, "start TLS session");
        any_connection_t tcpcon(new asio_tls_connection(std::move(socket), sslctx));
        co_return new http2_connection(std::move(tcpcon), ioctx());
      } else {
        HTTP2_LOG(TRACE, "start non-tls session");
        any_connection_t tcpcon(new asio_connection(std::move(socket)));
        co_return new http2_connection(std::move(tcpcon), ioctx());
      }
    } catch (std::exception const& e) {
      HTTP2_LOG(ERROR, "Transport initialization failure: {}", e.what());
      co_return nullptr;
    }
  }

  // Note: this code ignores possible bad_alloc and other logs exceptions
  dd::task<void> sessionLifecycle(dd::gate::gate::holder, asio::ip::tcp::socket socket) try {
    http2_connection_ptr_t http2con = co_await createConnection(std::move(socket));
    if (!http2con || !creator) {
      co_return;
    }
    if (gate.is_closed()) {
      http2con->shutdown(reqerr_e::CANCELLED);
      HTTP2_LOG(INFO, "session completed, but server stopped (server session is not created)");
      co_return;
    }

    http2_server_options opts = options;
    int reader_ec = 0;

    // firstly insert session into list, so server will drop it if stops during session establishing
    server_session session(std::move(http2con), opts, *creator);
    sessions.push_back(session);
    on_scope_exit {
      erase_byref(sessions, session);
    };

    auto sleepcb = [&session](duration_t d, io_error_code& ec) -> dd::task<void> {
      asio::steady_timer timer(session.server->ioctx());
      co_await net.sleep(timer, d, ec);
    };
    auto requestTerminateInactive = [&] {
      HTTP2_LOG(TRACE, "[SERVER] drops connection {} due client inactivity", (void*)session.connection.get());
      session.requestTerminate();
    };
    auto requestTerminate = [&] {
      HTTP2_LOG(TRACE, "[SERVER] writer drops connection {}", (void*)session.connection.get());
      session.requestTerminate();
    };

    try {
      timer_t timer(ioctx());
      timer.set_callback([&] {
        HTTP2_LOG(ERROR, "[SERVER] connection timeout, session: {}", (void*)session.connection.get());
        session.connection->shutdown(reqerr_e::TIMEOUT);
      });
      timer.arm(options.connectionTimeout);
      (void)co_await establish_http2_session_server(session.connection, opts);
      timer.cancel();
    } catch (std::exception& e) {
      HTTP2_LOG(ERROR, "server -> client connection establishment failed, err: {}", e.what());
      goto drop_session;
    }

    if (gate.is_closed()) {
      goto drop_session;
    }

    (void)start_writer_for_server(session.connection, sleepcb, requestTerminate, opts.forceDisableHpack,
                                  session.connectionPartsGate.hold());

    session.connection->pingdeadlinetimer.set_callback(requestTerminateInactive);
    session.connection->pingtimer.set_callback([framecount = size_t(0), &session, server = this]() mutable {
      if (session.framecount != framecount) {
        framecount = session.framecount;
        return;
      }
      // nothing happens since last call
      if (!session.connection->pingdeadlinetimer.armed()) {
        session.connection->pingdeadlinetimer.arm(server->options.idleTimeout);
      }
    });
    session.connection->pingtimer.arm_periodic(std::chrono::milliseconds(100));
    reader_ec = co_await start_server_reader_for(session);
    if (reader_ec != reqerr_e::DONE) {
      // give time for sending goaway
      co_await net.sleep(ioctx(), std::chrono::milliseconds(1));
    }
    HTTP2_LOG(TRACE, "[SERVER] session {} reader stops, waiting stop", (void*)session.connection.get());
  drop_session:
    session.requestTerminate();
    if (session.requestsLeft() > 0) {
      co_await session.sessiondone.wait();
    }
    // we are here if reader ended with exception or after soft shutdown (streams closed, new requests
    // forbidden)
    co_await session.connectionPartsGate.close();
    co_await session.responsegate.close();
    HTTP2_LOG(TRACE, "[SERVER] session {} stop ended", (void*)session.connection.get());
  } catch (std::exception& e) {
    HTTP2_LOG(ERROR, "session ended with exception: {}", e.what());
  }

  void stopListeners() {
    HTTP2_LOG(TRACE, "[SERVER] shutdown: listeners size {}", listeners.size());
    for (auto& l : listeners) {
      l.close();
    }
    // TODO run .open when required?
  }

  dd::task<void> shutdown() {
    HTTP2_LOG(TRACE, "[SERVER] shutdown started, server: {}", (void*)this);
    on_scope_exit {
      HTTP2_LOG(TRACE, "[SERVER] shutdown ended. server {}", (void*)this);
    };
    gate.request_close();
    stopListeners();
    for (auto& session : sessions) {
      session.requestShutdown();
    }
    co_await gate.close();
    assert(sessions.empty());
  }

  dd::task<void> terminate() {
    HTTP2_LOG(TRACE, "[SERVER] terminate started, server: {}", (void*)this);
    on_scope_exit {
      HTTP2_LOG(TRACE, "[SERVER] terminate ended. server {}", (void*)this);
    };
    gate.request_close();
    stopListeners();
    for (auto& session : sessions) {
      session.requestTerminate();
    }
    co_await gate.close();
    assert(sessions.empty());
  }
};

http2_server::http2_server(ssl_context_ptr ctx, http2_server_options options)
    : m_impl(std::make_unique<http2_server::impl>(
          // https://beta.boost.org/doc/libs/1_74_0/doc/html/boost_asio/overview/core/concurrency_hint.html
          options.singlethread ? 1 : BOOST_ASIO_CONCURRENCY_HINT_DEFAULT)) {
  if (ctx) {
    m_impl->sslctx = std::move(ctx);
  }
  m_impl->options = options;
  m_impl->creator = this;
}

http2_server::~http2_server() {
  assert(m_impl);

  if (m_impl->gate.active_count() == 0) {
    return;
  }
  m_impl->creator = nullptr;
  std::coroutine_handle h = m_impl->terminate().start_and_detach(/*stop_at_end=*/true);

  on_scope_exit {
    h.destroy();
  };
  // assume 'h' is suspended here every time when we check h.done()
  while (!h.done() && ioctx().run_one() != 0)
    ;
}

size_t http2_server::sessionsCount() const noexcept {
  return m_impl->sessions.size();
}

void http2_server::listen(server_endpoint a) {
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

}  // namespace http2
