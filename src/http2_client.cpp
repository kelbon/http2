

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"

#include "http2/http2_client.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http2_errors.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/http2_writer.hpp"
#include "http2/logger.hpp"
#include "http2/utils/macro.hpp"
#include "http2/utils/reusable_buffer.hpp"
#include "http2/utils/timer.hpp"
#include "http2/asio/awaiters.hpp"

#include <hpack/hpack.hpp>
#include <zal/zal.hpp>

/*

Путь каждого запроса в http2_client:

1. корутина sendRequest захватывает соединение через borrowConnection, при этом
может потребоваться вызвать startConnecting, на время соединения sendRequest
попадаёт в client.m_connectionWaiters. Note: При ошибке создания соединения,
запросы в m_connectionWaiters будут отменёны. В будущем эту политику можно
заменить на ещё одну попытку соединения и отмену только тех запросов, что по
таймауту уже не успевают
2. После захвата соединения sendRequst создаёт стрим, инициализирует его
streamid и засыпает на responseReceived, который:
    * инициализирует stream.task хендлом корутины sendRequest
    * складывает стрим в connection.responses (для отправки писателем) и
connection.timers (для проверки на таймаут)
3. писатель берёт из очереди стрим, перекладывает его в connection.responses и
начинает писать серверу. Важно переложить в responses заранее, чтобы не потерять
никаких фреймов для этого стрима
4. читатель начинает получать фреймы для стрима, вызывая on_header /
on_data_part при чтении HEADERS/DATA фреймов соответственно. При исключении из
пользовательских калбеков читатель вызывает finishRequestWithUserException

5. При получении фрейма с флагом END_STREAM читатель вызывает finishRequest,
который корректно забывает (forget) стрим из всех контейнеров и делает
task.resume, будя корутину sendRequest

    Note: фреймом с END_STREAM может быть в том числе trailer HEADERS после DATA

На любом этапе может прийти фрейм RST_STREAM/Goaway или вызваться coStop, это
также приведёт к finishRequest
*/

namespace http2 {

void http2_client::notifyConnectionWaiters(http2_connection_ptr_t result) noexcept {
  // assume only i have access to waiters
  auto waiters = std::move(m_connectionWaiters);
  HTTP2_LOG(TRACE, "resuming connection waiters, count: {}, connection: {}", waiters.size(),
            (void*)result.get());
  assert(m_connectionWaiters.empty());  // check boost really works as expected
  waiters.clear_and_dispose([&](noexport::waiter_of_connection* w) {
    assert(!w->result);
    w->result = result;
    assert(w->task);
    // ordering matters for getting correct stream id
    w->task.resume();
  });
}

// periodically invoked in connection.pingtimer
struct ping_callback {
  http2_connection_ptr_t con = nullptr;
  stream_id_t lastid = 0;
  duration_t pingtimeout;
  http2_client* client = nullptr;

  // precondition: c != nullptr, client != nullptr
  explicit ping_callback(http2_connection_ptr_t c, http2_client* clientptr, duration_t pingTimeout) {
    assert(c);
    assert(clientptr);
    con = std::move(c);
    client = std::move(clientptr);
    lastid = con->streamid;
    pingtimeout = pingTimeout;
  }

  void operator()() {
    // send ping only if nothing happens since last iteration
    if (lastid != con->streamid) {
      lastid = con->streamid;
      return;
    }
    if (!con->pingdeadlinetimer.armed()) {
      con->pingdeadlinetimer.arm(pingtimeout);
    }
    // assume will be ended before client dies (io_ctx)
    send_ping(con, PING_VALUE, /*requestPong=*/true).start_and_detach();
  }
};

dd::job http2_client::startConnecting(http2_client* self, deadline_t deadline) {
  assert(self);
  co_await std::suspend_always{};  // resumed when needed by creator

  if (self->m_connection || !self->m_connectionGate.try_enter()) {
    self->notifyConnectionWaiters(self->m_connection);
    co_return;
  }
  on_scope_exit {
    self->m_connectionGate.leave();
  };
  if (self->stopRequested()) {
    HTTP2_LOG(DEBUG, "connection tries to create when stop requested, ignored");
    self->notifyConnectionWaiters(nullptr);
    co_return;
  }
  if (self->alreadyConnecting()) {
    // connection awaiters will be awakened by connection when ends
    co_return;
  }
  http2_connection_ptr_t newConnection = nullptr;

  try {
    // note: connection unlocked before notify, avoiding this:
    // * first request drops connection,
    // * starting new connection
    // * ignore new connection (connections locked)
    // * no new requests, all stopped.
    {
      auto lock = self->lockConnections();
      HTTP2_LOG(TRACE, "creating connection for client {}", (void*)self);
      on_scope_exit {
        lock.release();
        self->notifyConnectionWaiters(newConnection);
        HTTP2_LOG(TRACE, "new connection: {}", (void*)newConnection.get());
      };
      any_connection_t tcpCon = co_await self->m_factory->createConnection(self->getHost(), deadline);
      http2_connection_ptr_t con = new http2_connection(std::move(tcpCon), self->ioctx());
      timer_t timer(self->ioctx());
      timer.arm(deadline.tp);
      timer.set_callback([con] { con->shutdown(reqerr_e::TIMEOUT); });
      assert(!self->m_notYetReadyConnection);
      self->m_notYetReadyConnection = con;
      on_scope_exit {
        self->m_notYetReadyConnection = nullptr;
      };
      newConnection = co_await establish_http2_session_client(std::move(con), self->m_options);
      timer.cancel();  //-V779 //-V2535
    }
    assert(!self->m_connection);
    assert(newConnection);
    self->m_connection = newConnection;  //-V779 //-V2535

    // this gate closed only in coStop and only after all startConnecting already done
    assert(!self->m_connectionPartsGate.is_closed());

    startReaderFor(self, newConnection);
    self->m_connection->writer.handle = nullptr;
    // writer itself sets writer handle in connection
    auto sleepcb = [self](duration_t d, io_error_code& ec) { return self->sleep(d, ec); };
    auto onnetworkerr = [self] { self->dropConnection(reqerr_e::NETWORK_ERR); };
    start_writer_for_client(newConnection, std::move(sleepcb), std::move(onnetworkerr),
                            self->getOptions().forceDisableHpack, self->m_connectionPartsGate.hold());

    if (self->m_options.pingInterval != duration_t::max()) {
      newConnection->pingtimer.arm_periodic(self->m_options.pingInterval);
      newConnection->pingtimer.set_callback(ping_callback(newConnection, self, self->m_options.pingTimeout));
      // armed when ping sended, canceled when ping received
      newConnection->pingdeadlinetimer.set_callback([self] { self->dropConnection(reqerr_e::TIMEOUT); });
    }
    // newConnection->timeoutWardenTimer will be armed when requests will be added
    newConnection->timeoutWardenTimer.set_callback([newConnection] {
      newConnection->dropTimeouted();
      if (!newConnection->timers.empty()) {
        newConnection->timeoutWardenTimer.arm(newConnection->timers.top()->deadline.tp);
      }
    });
  } catch (std::exception& e) {
    HTTP2_LOG(ERROR, "exception while trying to connect: {}", e.what());
  }
}

// handles only utility frames (not DATA / HEADERS), returns false on protocol
// error (may throw it too)
static bool handle_utility_frame(http2_frame_t frame, http2_connection& con) {
  using enum frame_e;

  switch (frame.header.type) {
    case HEADERS:
    case DATA:
      unreachable();
    case SETTINGS:
      con.serverSettingsChanged(frame);
      return true;
    case PING:
      handle_ping(ping_frame::parse(frame.header, frame.data), &con).start_and_detach();
      return true;
    case RST_STREAM:
      if (!con.finishStreamWithError(rst_stream::parse(frame.header, frame.data))) {
        HTTP2_LOG(INFO,
                  "server finished stream (id: {}) which is not exists (maybe "
                  "timeout or canceled)",
                  frame.header.streamId);
      }
      return true;
    case GOAWAY: {
      goaway_frame f = goaway_frame::parse(frame.header, frame.data);
      if (f.errorCode != errc_e::NO_ERROR) {
        throw goaway_exception(f.lastStreamId, f.errorCode, std::move(f.debugInfo));
      } else {
        con.serverRequestsGracefulShutdown(f);
        return true;
      }
    }
    case WINDOW_UPDATE:
      con.windowUpdate(window_update_frame::parse(frame.header, frame.data));
      return true;
    case PUSH_PROMISE:
      if (con.localSettings.enablePush) {
        HTTP2_LOG(INFO, "received PUSH_PROMISE, not supported");
      }
      return false;
    case CONTINUATION:
      HTTP2_LOG(INFO, "received CONTINUATION, not supported");
      return false;
    default:
    case PRIORITY:
    case PRIORITY_UPDATE:
      // ignore
      return true;
  }
}

// handles DATA or HEADERS, returns false on protocol error
[[nodiscard]] static bool handle_headers_or_data(http2_frame_t frame, http2_connection& con) noexcept {
  using enum frame_e;

  assert(con.localSettings.deprecatedPriorityDisabled);
  if (frame.header.streamId == 0) {
    return false;
  }
  if (!frame.removePadding()) {
    return false;
  }

  request_node* node = con.findResponseByStreamid(frame.header.streamId);
  if (!node) {
    con.ignoreFrame(frame);
    return true;
  }
  try {
    switch (frame.header.type) {
      case HEADERS:
        frame.ignoreDeprecatedPriority();
        node->receiveResponseHeaders(con.decoder, frame);
        if ((frame.header.flags & flags::END_HEADERS) && !node->onDataPart) {
          con.finishRequest(*node, node->status);
        }
        break;
      case DATA:
        // applicable only to data
        // Note: includes padding!
        decrease_window_size(con.myWindowSize, frame.header.length);
        node->receiveData(frame);
        break;
      default:
        unreachable();
    }
  } catch (hpack::protocol_error& e) {
    HTTP2_LOG(ERROR, "exception while decoding headers block (HPACK), err: {}", e.what());
    // To avoid ambiguity just send max_stream_id every time. Server did not
    // initiate streams (SERVER_PUSH disabled) and nghttp2 for example answers
    // with protocol error for some values
    //
    // Rfc:
    // If a connection terminates without a GOAWAY frame,
    // the last stream identifier is effectively the highest possible stream
    // identifier
    send_goaway(&con, MAX_STREAM_ID, errc_e::COMPRESSION_ERROR, e.what()).start_and_detach();
    return false;
  } catch (protocol_error& e) {
    HTTP2_LOG(ERROR, "exception while handling frame for stream {}, err: {}", node->streamid, e.what());
    return false;
  } catch (...) {
    // user-handling exception, do not drop connection
    con.finishRequestWithUserException(*node, std::current_exception());
    return true;
  }

  if (frame.header.flags & flags::END_STREAM) {
    con.finishRequest(*node, node->status);
  }
  return true;
}

// returns false on protocol error
[[nodiscard]] static bool handle_frame(http2_frame_t frame, http2_connection& con) {
  using enum frame_e;
  switch (frame.header.type) {
    case HEADERS:
    case DATA:
      return handle_headers_or_data(frame, con);
    default:
      return handle_utility_frame(frame, con);
  }
}

// writer works on node with reader (window_update / rst_stream possible)
// also node may be cancelled or destroyed, so writer and reader must never
// cache node between co_awaits
dd::job http2_client::startReaderFor(http2_client* self, http2_connection_ptr_t c) {
  using enum frame_e;
  HTTP2_LOG(TRACE, "reader started for {}", (void*)c.get());
  assert(self && c);
  assert(!self->m_connectionPartsGate.is_closed());

  on_scope_exit {
    HTTP2_LOG(TRACE, "reader for {} ended", (void*)c.get());
  };
  auto guard = self->m_connectionPartsGate.hold();

  http2_connection& con = *c;
  io_error_code ec;
  reusable_buffer buffer;
  http2_frame_t frame;
  int reason = reqerr_e::UNKNOWN_ERR;

  try {
    while (!con.isDoneCompletely()) {
      if (con.isDropped()) {
        goto connection_dropped;
      }

      // read frame header

      frame.data = buffer.getExactly(FRAME_HEADER_LEN);

      co_await con.read(frame.data, ec);

      if (ec) {
        goto network_error;
      }
      if (con.isDropped()) {
        goto connection_dropped;
      }

      // parse frame header

      frame.header = frame_header::parse(frame.data);
      if (!frame.validateHeader()) {
        goto protocol_error;
      }

      // read frame data

      frame.data = buffer.getExactly(frame.header.length);
      co_await con.read(frame.data, ec);
      if (ec) {
        goto network_error;
      }
      if (con.isDropped()) {
        goto connection_dropped;
      }

      // handle frame

      if (!handle_frame(frame, con)) {
        goto protocol_error;
      }
      // connection control flow (streamlevel in handle_frame)
      if (con.myWindowSize < MAX_WINDOW_SIZE / 2) {
        co_await update_window_to_max(con.myWindowSize, 0, c);
      }
    }
  } catch (protocol_error&) {
    HTTP2_LOG(INFO, "protocol error happens");
    reason = reqerr_e::PROTOCOL_ERR;
    goto dropConnection;
  } catch (goaway_exception& gae) {
    HTTP2_LOG(ERROR, "goaway received, info: {}, errc: {}", gae.debugInfo, e2str(gae.errorCode));
    reason = reqerr_e::SERVER_CANCELLED_REQUEST;
    goto dropConnection;
  } catch (std::exception& se) {
    HTTP2_LOG(INFO, "unexpected exception {}", se.what());
    reason = reqerr_e::UNKNOWN_ERR;
    goto dropConnection;
  } catch (...) {
    HTTP2_LOG(INFO, "unknown exception happens");
    reason = reqerr_e::UNKNOWN_ERR;
    goto dropConnection;
  }

  assert(con.isDoneCompletely());
  // must not resume anyone with 'done', because no pending requests (completely
  // done)
  reason = reqerr_e::DONE;
  goto dropConnection;
protocol_error:
  // give some time for sending goaway
  co_await self->sleep(std::chrono::milliseconds(3), ec);
  reason = reqerr_e::PROTOCOL_ERR;
  goto dropConnection;
network_error:
  reason = ec == boost::asio::error::operation_aborted ? reqerr_e::CANCELLED : reqerr_e::NETWORK_ERR;
  if (reason == reqerr_e::NETWORK_ERR) {
    HTTP2_LOG(DEBUG, "reader drops connection after network err: {}", ec.what());
  }
dropConnection:
  self->dropConnection(static_cast<reqerr_e::values_e>(reason));
connection_dropped:
  if (!self->m_connectionWaiters.empty() && !self->alreadyConnecting()) {
    HTTP2_LOG(DEBUG, "client initiates reconnect after graceful shutdown or out of streams");
    co_await dd::this_coro::destroy_and_transfer_control_to(
        startConnecting(self, deadline_after(self->m_options.connectionTimeout)).handle);
  }
  co_return;
}

http2_client::~http2_client() {
  HTTP2_LOG(TRACE, "~http2_client");
  // in any case, even if client stopped now, some requests may wait
  notifyConnectionWaiters(nullptr);
  dropConnection(reqerr_e::CANCELLED);
  stop();
  // make sure client was closed correctly before destroy
  assert(!m_connection);
  assert(m_connectionWaiters.empty());
  assert(m_requestsInProgress == 0);
}

http2_client::http2_client(endpoint_t host, http2_client_options opts, any_transport_factory tf)
    : m_host(std::move(host)), m_options(opts), m_factory(std::move(tf)) {
  assert(m_factory);
  m_options.maxReceiveFrameSize = std::min(FRAME_LEN_MAX, m_options.maxReceiveFrameSize);
  HTTP2_LOG(TRACE, "http2_client created");
}

noexport::waiter_of_connection::~waiter_of_connection() {
  // in case when .destroy on handle called correctly cancels request
  if (is_linked()) {
    erase_byref(client->m_connectionWaiters, *this);
  }
}

bool noexport::waiter_of_connection::await_ready() noexcept {
  if (!client->m_connection || client->m_connection->isDropped() ||
      client->m_connection->isOutofStreamids()) {
    return false;
  }
  result = client->m_connection;
  return true;
}

std::coroutine_handle<> noexport::waiter_of_connection::await_suspend(std::coroutine_handle<> h) noexcept {
  task = h;
  client->m_connectionWaiters.push_back(*this);
  if (client->alreadyConnecting()) {
    return std::noop_coroutine();
  }
  return client->startConnecting(client, deadline).handle;
}

[[nodiscard]] http2_connection_ptr_t noexport::waiter_of_connection::await_resume() {
  if (!result || result->isDropped() || client->stopRequested()) {
    return nullptr;
  }
  return std::move(result);
}

void http2_client::dropConnection(reqerr_e::values_e reason) noexcept {
  http2_connection_ptr_t con = std::move(m_connection);
  if (!con) {
    return;
  }
  // note: i have shared ptr to con, so it will not be destroyed while shutting
  // down and resuming its reader/writer
  con->shutdown(reason);
}

dd::task<int> http2_client::sendRequest(on_header_fn_ptr onHeader, on_data_part_fn_ptr onDataPart,
                                        http_request request, deadline_t deadline) {
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  if (deadline.isReached()) [[unlikely]] {
    co_return reqerr_e::TIMEOUT;
  }
  ++m_requestsInProgress;
  on_scope_exit {
    --m_requestsInProgress;
  };
  http2_connection_ptr_t con = co_await borrowConnection(deadline);
  if (deadline.isReached()) {
    co_return reqerr_e::TIMEOUT;
  }
  if (!con) {
    co_return reqerr_e::NETWORK_ERR;
  }
  if (stopRequested()) [[unlikely]] {
    co_return reqerr_e::CANCELLED;
  }
  assert(!request.path.empty());
  request.scheme = con->tcpCon->isHttps() ? scheme_e::HTTPS : scheme_e::HTTP;
  stream_id_t streamid = con->nextStreamid();
  HTTP2_LOG(TRACE, "sending http2 request, path: {}, method: {}, streamid: {}", request.path,
            (int)request.method, streamid);

  node_ptr node = con->newRequestNode(std::move(request), deadline, onHeader, onDataPart, streamid);

  co_return co_await con->responseReceived(*node);
}

dd::task<void> http2_client::coStop() {
  HTTP2_LOG(TRACE, "http2_client::coStop started, client: {}", (void*)this);
  on_scope_exit {
    HTTP2_LOG(TRACE, "http2_client::coStop ended, client: {}", (void*)this);
  };
  io_error_code ec;
  http2_connection_ptr_t con = m_connection;  // prevent destroy for graceful shutdown.

  // waiting all requests finish

  // prevent new requests on this connection
  if (con && !con->gracefulshutdownGoawaySended) {
    con->initiateGracefulShutdown(con->lastInitiatedStreamId());
  }
  // wait all requests done
  while (m_requestsInProgress != 0)  // -V776
  {
    co_await sleep(std::chrono::nanoseconds(10), ec);
  }
  // request stop
  ++m_stopRequested;
  on_scope_exit {
    --m_stopRequested;
  };
  // wait all 'connect' coroutines done
  co_await m_connectionGate.close();
  // prevent new connection tries and wait all startConnecting coroutines are
  // done
  auto lock = lockConnections();
  assert(!m_notYetReadyConnection);
  // 1 is my connection lock here.
  // case when startConnecting on stage creating tcp connection
  while (m_isConnecting > 1) {
    co_await sleep(std::chrono::nanoseconds(10), ec);
  }
  // notify all not started requests about stop
  notifyConnectionWaiters(nullptr);
  // drop our connection correctly if exists
  dropConnection(reqerr_e::CANCELLED);

  co_await m_connectionPartsGate.close();
  m_connectionPartsGate = {};  // reopen
  m_connectionGate = {};       // reopen

  lock.release();
  assert(!m_connection);
  assert(m_connectionWaiters.empty());
  assert(m_requestsInProgress == 0);
}

dd::task<void> http2_client::coAbort() {
  dropConnection(reqerr_e::CANCELLED);
  notifyConnectionWaiters(nullptr);
  return coStop();
}

bool http2_client::connected() const {
  return !!m_connection;
}

bool http2_client::isHttps() const noexcept {
  assert(m_connection);
  return m_connection.get()->tcpCon->isHttps();
}

void http2_client::setHost(endpoint_t s) noexcept {
  assert(!connected());
  m_host = std::move(s);
}

dd::task<bool> http2_client::tryConnect(deadline_t deadline) {
  if (stopRequested() || m_connectionGate.is_closed()) {
    co_return false;
  }
  auto guard = m_connectionGate.hold();
  http2_connection_ptr_t con = co_await borrowConnection(deadline);
  co_return !!con;
}

void http2_client::stop() {
  ioctx().stop();  // cancel all
  ioctx().restart();
  std::coroutine_handle h = coStop().start_and_detach(/*stop_at_end=*/true);
  on_scope_exit {
    h.destroy();
  };
  while (!h.done() && ioctx().run_one() != 0)
    ;
}

dd::task<void> http2_client::sleep(duration_t d, io_error_code& ec) {
  boost::asio::steady_timer timer(ioctx());
  co_await net.sleep(timer, d, ec);
}

}  // namespace http2
#pragma GCC diagnostic pop
