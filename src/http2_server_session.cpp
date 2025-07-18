

#include "http2/http2_server_session.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/logger.hpp"

#include <algorithm>
#include <utility>

#include <http2/http2_server.hpp>
#include <zal/zal.hpp>

/*

Путь каждого запроса внутри сессии сервера:

1. server_reader читает HEADERS фрейм от клиента
2. server_reader вызывает startRequestAssemble, который создаёт стрим ноду и:
    * добавляет её в connection.responses (в сервере responses используются
также для сборки запроса)
    * выставляет статус reqerr_e::REQUEST_CREATED чтобы обозначить, что запрос
ещё не собран
    ** после выхода из startRequestAssemble стрим остаётся без владельца
3. server_reader продолжает получать фреймы от клиента, собирая их в один внутри
responses и вызывает onRequestReady, когда приходит фрейм с флагом END_STREAM
(это может быть самый первый фрейм HEADERS)
4. onRequestReady
    * стартует корутину send_response перехватывает владение стримом
    * выставляет статус RESPONSE_IN_PROGRESS
    * вызывает пользовательский калбек для получения Response
  Важно: если стрим отменён на этом этапе, то send_response будет продолжать
ждать Response от пользователя и только потом удалится вместе с стримом .task в
стриме ещё nullptr, чтобы не разбудить send_response во время ожидания Response
5. После получения Response корутина sendResponse засыпает на responseWritten,
предварительно выставляя в стрим.task свой хендл и перенося стрим из
connection.responses в connection.requests (очередь на отправку для писателя)
6. Писатель забирает из очереди стрим с готовым для отправки Response и пишет
его, после завершения вызывает connection.finsihRequest

На любом из этапов стрим может быть отменён, например через получение фрейма
RST_STREAM или .requestTerminate. Во время и для завершения учитываются
stream.task, и владельцы (если стрим сейчас без владельца, он уничтожается на
месте) Удаление из контейнеров requests/responses (forget) предотвращает любое
получение фреймов для этого стрима в server_reader

  Важно: стрим всегда должен быть хотя бы в одном из контейнеров
responses/requests, чтобы пришедшие фреймы RST_STREAM и прочие могли на него
повлиять


*/
namespace http2 {

// friend of Response
struct response_bro {
  static http_request torequest(http_response&& rsp) noexcept {
    http_request req;
    req.body.data = std::move(rsp.body);
    req.headers = std::move(rsp.headers);
    // must not contain ":status" or other pseudoheaders
    assert(std::find_if(rsp.headers.begin(), rsp.headers.end(),
                        [](http_header_t& h) { return h.name().starts_with(':'); }) == rsp.headers.end());
    auto it = std::find_if(req.headers.begin(), req.headers.end(), [](http_header_t const& h) {
      // header names are always in lower case
      return h.name() == "content-type";
    });
    if (it != req.headers.end()) {
      req.body.contentType = std::move(it->hvalue);
      req.headers.erase(it);
    }
    return req;
  }
};

server_session::server_session(http2_connection_ptr_t con, http2_server_options opts, http2_server& s)
    : connection(std::move(con)), options(opts), server(&s) {
  assert(connection);
  HTTP2_LOG(TRACE, "[SERVER] session {} started", (void*)connection.get());
}

server_session::~server_session() {
  assert(connection->isDropped() && connection->requests.empty() && connection->responses.empty() &&
         "server session was not closed before destroy");
  HTTP2_LOG(TRACE, "[SERVER] session ended {}", (void*)connection.get());
}

static dd::task<int> send_response(node_ptr node, server_session& session) {
  assert(node);
  assert(!node->req.path.empty());
  assert(node->refcount == 1);
  assert(node->status == reqerr_e::RESPONSE_IN_PROGRESS);
  on_scope_exit {
    session.onResponseDone();
  };
  if (session.responsegate.is_closed() || session.connection->isDropped() ||
      !node->responsesHook.is_linked()) {
    // already canceled
    co_return 0;
  }
  auto guard = session.responsegate.hold();

  // Note: здесь неявное предположение о том, что пользовательский калбек
  // не будет ждать .stop сервера (нарушение ведёт к вечному ожиданию)
  // и вернёт хоть когда-нибудь response (нарушение ведёт к зависанию стрима)
  // Note: callback accepts Request by reference, need to handle it here...
  http_response rsp = co_await session.server->handle_request(std::move(node->req));

  node->status = (int)rsp.status;
  node->req = response_bro::torequest(std::move(rsp));

  if (co_await session.responseWritten(*node)) {
    HTTP2_LOG(TRACE, "[SERVER] response for stream {} successfully written", node->streamid);
  } else {
    HTTP2_LOG(TRACE, "[SERVER], response for stream {} failed", node->streamid);
  }
  co_return 0;
}

void server_session::onRequestReady(request_node& n) noexcept {
  assert(!n.req.path.empty());
  assert(n.refcount == 1);

  // was detached before in startRequestAssemble
  http2::node_ptr np(&n, /*add_ref=*/false);
  np->status = reqerr_e::RESPONSE_IN_PROGRESS;

  on_scope_failure(nodedone) {
    onResponseDone();
  };
  if (!np->responsesHook.is_linked()) {
    // already canceled
    HTTP2_LOG(TRACE, "[SERVER] stream {} response canceled due session {} shutdown", np->streamid,
              (void*)connection.get());
    return;
  }
  HTTP2_LOG(TRACE, "[SERVER] session {} sends response to stream {}", (void*)connection.get(), np->streamid);
  stream_id_t streamid = np->streamid;

  try {
    send_response(std::move(np), *this).start_and_detach();
    nodedone.no_longer_needed();
  } catch (std::exception& e) {
    send_rst_stream(connection, streamid, errc_e::INTERNAL_ERROR).start_and_detach();
    HTTP2_LOG(ERROR, "[SERVER] session {} cannot handle request {} due exception: {}",
              (void*)connection.get(), streamid, e.what());
  }
}

bool server_session::rstStreamServer(stream_id_t streamid) noexcept {
  request_node* n = connection->findResponseByStreamid(streamid);
  if (!n) {
    auto it = std::find_if(connection->requests.begin(), connection->requests.end(),
                           [streamid](request_node& rn) { return rn.streamid == streamid; });
    if (it != connection->requests.end()) {
      n = &*it;
    } else {
      return false;
    }
  }
  finishServerRequest(*n);
  return true;
}

size_t server_session::requestsLeft() const noexcept {
  return connection->requests.size() + connection->responses.size();
}

void server_session::requestShutdown() noexcept {
  if (!newRequestsForbiden) {
    newRequestsForbiden = true;
    send_goaway(connection, connection->streamid, errc_e::NO_ERROR, "graceful shutdown").start_and_detach();
  }

  if (requestsLeft() == 0) {
    onSessionDone();
  }
}

void server_session::requestTerminate() noexcept {
  if (terminated) {
    if (requestsLeft() == 0) {
      onSessionDone();
    }
    return;
  }
  terminated = true;
  newRequestsForbiden = true;

  send_goaway(connection, connection->streamid, errc_e::NO_ERROR, "graceful shutdown").start_and_detach();

  // forget requests (including not finished)
  auto doforget = [&](request_node* n) { finishServerRequest(*n); };
  connection->responses.clear_and_dispose(doforget);

  assert(connection->requests.empty() && connection->responses.empty() && connection->timers.empty());
  if (requestsLeft() == 0) {
    onSessionDone();
  }
}

void server_session::onResponseDone() noexcept {
  if (newRequestsForbiden && requestsLeft() == 0) {
    onSessionDone();
  }
}

void server_session::onSessionDone() noexcept {
  assert(newRequestsForbiden && requestsLeft() == 0);
  if (done) {
    return;
  }
  done = true;
  connection->shutdown(reqerr_e::CANCELLED);
  if (sessiondone.hasWaiter()) {
    // after sessiondone notify its possible, that 'this' is already deleted
    // so we give caller time to end using session pointer
    asio::post(server->ioctx(), std::exchange(sessiondone.waiter, nullptr));
  }
}

node_ptr server_session::newEmptyStreamNode(stream_id_t id) {
  assert((id % 2) == 1);
  assert(id <= MAX_STREAM_ID);
  // server reader do not uses 'on_header' / 'on_data_part'
  return connection->newRequestNode({}, deadline_t::never(), nullptr, nullptr, id);
}

request_node& server_session::startRequestAssemble(stream_id_t id) {
  connection->streamid = std::max(id, connection->streamid);
  if (connection->findResponseByStreamid(id)) {
    // TODO check if trying to create already closed stream
    HTTP2_LOG(ERROR, "[SERVER] client initiates stream, which was already open, con: {}",
              (void*)connection.get());
    throw protocol_error{errc_e::PROTOCOL_ERROR};
  }
  http2::node_ptr n = newEmptyStreamNode(id);
  n->status = reqerr_e::REQUEST_CREATED;
  connection->responses.insert(*n);
  // Note: после этого detach() стрим остаётся без владельца
  // это учитывается в onRequestReady и finishServerRequest
  return *n.detach();
}

void server_session::clientSettingsChanged(http2_frame_t newsettings) {
  if (newsettings.header.flags & flags::ACK) {
    if (newsettings.header.length != 0) {
      HTTP2_LOG(ERROR,
                "[SERVER] received client settings with ACK and len != 0 ({}), "
                "con: {}",
                newsettings.header.length, (void*)connection.get());
      throw protocol_error{errc_e::PROTOCOL_ERROR};
    }
    // только после подтверждения настроек я действительно могу перейти на свои настройки
    // ведь до этого клиент мог посылать запросы по старому размеру динамической таблицы
    connection->decoder.dyntab.update_size(connection->localSettings.headerTableSize);
    return;
  }
  // should be called from server, so remote settings is client settings
  settings_t before = connection->remoteSettings;
  client_settings_visitor vtor(connection->remoteSettings);
  settings_frame::parse(newsettings.header, newsettings.data, vtor);
  if (before.headerTableSize != connection->remoteSettings.headerTableSize) {
    HTTP2_LOG(INFO, "HPACK table resized, new size {}, old size: {}, con: {}",
              connection->remoteSettings.headerTableSize, before.headerTableSize, (void*)connection.get());
    connection->encodertablesizechangerequested = true;
  }
  // TODO other value changes, e.g. if initial stream size changed,
  // then change all active streams window size
  send_settings_ack(connection).start_and_detach();
}

void server_session::clientRequestsGracefulShutdown(goaway_frame f) {
  (void)f;
  HTTP2_LOG(TRACE,
            "[SERVER] receives goaway from client, laststreamid: {}, dbginfo: "
            "{}, con: {}",
            f.lastStreamId, f.debugInfo, (void*)connection.get());
  // nothing to do, since server do not start streams
  // and client wants to work until all requests are done (ok just work, then
  // drop connection)
}

void server_session::finishServerRequest(request_node& n) noexcept {
  if (n.status == reqerr_e::REQUEST_CREATED) {
    // request did not assembled yet
    assert(n.task == nullptr);
    connection->forget(n);
    HTTP2_LOG(TRACE, "[SERVER] stream {} canceled before its asembled", n.streamid);
    // not yet complete request, writer will never see it
    assert(n.refcount == 1);
    // single owner, which was 'detach' in startRequestAssemble
    intrusive_ptr_release(&n);
    onResponseDone();
  } else {
    bool orphan = !n.task;
    connection->finishRequest(n, reqerr_e::CANCELLED);
    if (orphan) {
      onResponseDone();
    }
  }
}

}  // namespace http2
