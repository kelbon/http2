

#include "http2/http2_server_session.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/logger.hpp"
#include "http2/asio/asio_executor.hpp"

#include <algorithm>
#include <utility>

#include <http2/http2_server.hpp>
#include <zal/zal.hpp>

/*

Путь каждого запроса внутри сессии сервера:

1. server_reader читает HEADERS фрейм от клиента
2. server_reader вызывает startRequestAssemble, который создаёт стрим ноду и:
    * добавляет её в connection.responses (в сервере responses используются также для сборки запроса)
    * выставляет статус reqerr_e::REQUEST_CREATED чтобы обозначить, что запрос ещё не собран
    ** после выхода из startRequestAssemble стрим остаётся без владельца
3. server_reader продолжает получать фреймы от клиента, собирая их в один внутри responses
и вызывает onRequestReady, когда приходит фрейм с флагом END_STREAM (это может быть самый первый фрейм
HEADERS)
4. onRequestReady
    * стартует корутину send_response перехватывает владение стримом
    * выставляет статус RESPONSE_IN_PROGRESS
    * вызывает пользовательский калбек для получения Response
  Важно: если стрим отменён на этом этапе, то send_response будет продолжать ждать Response от пользователя и
только потом удалится вместе с стримом .task в стриме ещё nullptr, чтобы не разбудить send_response во время
ожидания Response
5. После получения Response корутина send_response выставляет стрим.status и стрим.request, засыпает на
responseWritten, предварительно выставляя в стрим.task свой хендл и перенося стрим из connection.responses в
connection.requests (очередь на отправку для писателя)
6. Писатель забирает из очереди стрим с готовым для отправки Response и пишет его, после завершения вызывает
connection.finsihRequest

На любом из этапов стрим может быть отменён, например через получение фрейма RST_STREAM или .requestTerminate.
Во время и для завершения учитываются stream.task, и владельцы (если стрим сейчас без владельца, он
уничтожается на месте) Удаление из контейнеров requests/responses (forget) предотвращает любое получение
фреймов для этого стрима в server_reader

  Важно: стрим всегда должен быть хотя бы в одном из контейнеров responses/requests, чтобы пришедшие фреймы
RST_STREAM и прочие могли на него повлиять


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
    return req;
  }
};

server_session::server_session(http2_connection_ptr_t con, http2_server_options opts, http2_server& s)
    : connection(std::move(con)), options(opts), server(&s) {
  assert(connection);
}

server_session::~server_session() {
  assert(connection->isDropped() && connection->requests.empty() && connection->responses.empty() &&
         "server session was not closed before destroy");
  HTTP2_LOG(TRACE, "session ended", name());
}

static dd::task<int> send_response(node_ptr node, server_session& session) {
  assert(node);
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
  http_response rsp;
  HTTP2_ASSUME_THREAD_UNCHANGED_START;
  try {
    rsp = co_await session.server->handle_request(std::move(node->req), request_context(*node));
    // here node->is_streaming() possible if ctx.streaming_response was called
    assert(!(node->is_streaming() && !rsp.body.empty()) && "streaming response must be with empty body");
  } catch (stream_error& e) {
    // Note: catching stream error, so user can implement other protocol over HTTP/2 with additional
    // requirements
    HTTP2_LOG(ERROR, "handle request failed: {}", e.what(), session.name());
    HTTP2_ASSUME_THREAD_UNCHANGED_END;
    assert(e.streamid == node->streamid);
    send_rst_stream(session.connection, node->streamid, e.errc).start_and_detach();
    co_return 0;
  } catch (std::exception& e) {
    HTTP2_LOG(ERROR, "request handling ended with error, streamid: {}, err: {}", node->streamid, e.what(),
              session.name());
    HTTP2_ASSUME_THREAD_UNCHANGED_END;
    send_rst_stream(session.connection, node->streamid, errc_e::INTERNAL_ERROR).start_and_detach();
    co_return 0;
  }
  HTTP2_ASSUME_THREAD_UNCHANGED_END;
  assert(rsp.status > 0);
  node->status = (int)rsp.status;
  node->req = response_bro::torequest(std::move(rsp));

  if (co_await session.responseWritten(*node)) {
    HTTP2_LOG(TRACE, "response for stream {} successfully written", node->streamid, session.name());
  } else {
    HTTP2_LOG(TRACE, "response for stream {} failed", node->streamid, session.name());
  }
  co_return 0;
}

void server_session::onRequestReady(request_node& n) noexcept {
  // was detached before in startRequestAssemble
  http2::node_ptr np(&n, /*add_ref=*/false);
  if (np->bidir_stream_active)
    return;  // already done, DATA with END_STREAM received (its END_STREAM)
  np->status = reqerr_e::RESPONSE_IN_PROGRESS;
  on_scope_failure(nodedone) {
    onResponseDone();
  };
  if (!np->responsesHook.is_linked()) {
    // already canceled
    HTTP2_LOG(TRACE, "stream {} response canceled due session shutdown", np->streamid, name());
    return;
  }
  HTTP2_LOG(TRACE, "session sends response to stream {}", np->streamid, name());
  stream_id_t streamid = np->streamid;

  try {
    send_response(std::move(np), *this).start_and_detach();
    nodedone.no_longer_needed();
  } catch (std::exception& e) {
    send_rst_stream(connection, streamid, errc_e::INTERNAL_ERROR).start_and_detach();
    HTTP2_LOG(ERROR, "session cannot handle request {} due exception: {}", streamid, e.what(), name());
  }
}

bool server_session::rstStreamServer(rst_stream rstframe) {
  connection->validateRstFrame(rstframe);
  request_node* n = connection->findResponseByStreamid(rstframe.header.streamId);
  if (!n) {
    auto it = std::find_if(
        connection->requests.begin(), connection->requests.end(),
        [streamid = rstframe.header.streamId](request_node& rn) { return rn.streamid == streamid; });
    if (it != connection->requests.end()) {
      n = &*it;
    } else {
      return false;
    }
  }
  n->canceledByRstStream = true;
  finishServerRequest(*n);
  return true;
}

void server_session::rstStreamAfterError(stream_error const& e) {
  rst_stream rst;
  rst.header = rst.make_header(e.streamid);
  rst.errorCode = e.errc;
  // reuse rst stream like if someone sent it
  rstStreamServer(rst);
  send_rst_stream(connection, e.streamid, e.errc).start_and_detach();
}

size_t server_session::requestsLeft() const noexcept {
  return connection->requests.size() + connection->responses.size();
}

void server_session::requestShutdown() noexcept {
  if (!newRequestsForbiden) {
    newRequestsForbiden = true;
    send_goaway(connection, connection->lastInitiatedStreamId(), errc_e::NO_ERROR, "graceful shutdown")
        .start_and_detach();
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

  send_goaway(connection, connection->lastInitiatedStreamId(), errc_e::NO_ERROR, "graceful shutdown")
      .start_and_detach();

  // forget requests (including not finished)
  auto doforget = [&](request_node* n) { finishServerRequest(*n); };
  connection->responses.clear_and_dispose(doforget);
  connection->requests.clear_and_dispose(doforget);

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

void server_session::startRequestAssemble(const http2_frame_t& frame) {
  assert(frame.header.type == frame_e::HEADERS);

  // if stream already exist, its trailers or error
  if (auto* r = connection->findResponseByStreamid(frame.header.streamId)) [[unlikely]] {
    if (r->is_half_closed_server()) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("client initiates stream, which was already open, streamid: {}",
                                       frame.header.streamId));
    } else {
      // trailer headers received
      r->receiveRequestTrailers(connection->decoder, frame);
      if (frame.header.flags & flags::END_STREAM) {
        // Note: manages 'node' lifetime
        onRequestReady(*r);
      }
      return;
    }
  }

  if (frame.header.streamId <= connection->laststartedstreamid) {
    // https://www.rfc-editor.org/rfc/rfc9113.html#section-5.1.1-2
    // "identifier of a newly established stream MUST be numerically greater than all streams that the
    // initiating endpoint has opened"
    throw protocol_error(
        errc_e::PROTOCOL_ERROR,
        std::format("stream identifier that is not numerically greater than previous (new: {}, prev: {})",
                    frame.header.streamId, connection->laststartedstreamid));
  }

  // Note: before making a decision about a stream, to keep in mind the client's desire to create such stream
  connection->laststartedstreamid = std::max(connection->laststartedstreamid, frame.header.streamId);

  if (connection->is_closed_stream(frame.header.streamId)) {
    throw protocol_error(errc_e::STREAM_CLOSED,
                         std::format("stream already closed, but received HEADERS frame. Stream id: {}",
                                     frame.header.streamId));
  } else if (requestsLeft() >= connection->localSettings.maxConcurrentStreams) {
    throw stream_error(errc_e::REFUSED_STREAM, frame.header.streamId,
                       std::format("refused due max concurrent streams exceeded, max count: {}, actual: {}",
                                   connection->localSettings.maxConcurrentStreams, requestsLeft()));
  }

  http2::node_ptr n = newEmptyStreamNode(frame.header.streamId);
  n->status = reqerr_e::REQUEST_CREATED;
  connection->insertResponseNode(*n);
  // Note: после этого detach() стрим остаётся без владельца
  // это учитывается в onRequestReady и finishServerRequest
  request_node& node = *n.detach();

  node.receiveRequestHeaders(connection->decoder, frame);
  if ((frame.header.flags & flags::END_STREAM) || node.is_connect_request()) {
    // Note: manages 'node' lifetime
    onRequestReady(node);
  }
}

void server_session::clientSettingsChanged(http2_frame_t newsettings) {
  connection->settings_changed(newsettings, /*remote_is_client=*/true);
}

void server_session::clientRequestsGracefulShutdown(goaway_frame f) {
  (void)f;
  HTTP2_LOG(TRACE, "received goaway from client, laststreamid: {}, dbginfo: {}", f.lastStreamId, f.debugInfo,
            name());
  // nothing to do, since server do not start streams
  // and client wants to work until all requests are done (ok just work, then
  // drop connection)
}

void server_session::finishServerRequest(request_node& n) noexcept {
  if (n.status == reqerr_e::REQUEST_CREATED) {
    // request did not assembled yet
    assert(n.task == nullptr);
    connection->forget(n);
    HTTP2_LOG(TRACE, "stream {} canceled before its asembled", n.streamid, name());
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

void server_session::receive_headers(http2_frame_t frame) {
  assert(frame.header.type == frame_e::HEADERS);
  frame.validate_streamid();
  frame.removePadding();
  frame.ignoreDeprecatedPriority();
  if (newRequestsForbiden) [[unlikely]] {
    connection->ignoreFrame(frame);
    send_rst_stream(connection, frame.header.streamId, errc_e::REFUSED_STREAM).start_and_detach();
    return;
  }
  startRequestAssemble(frame);
}

void server_session::receive_data(http2_frame_t frame) {
  assert(frame.header.type == frame_e::DATA);
  frame.validate_streamid();
  frame.removePadding();
  request_node* node = connection->findResponseByStreamid(frame.header.streamId);
  if (!node) {
    connection->ignoreFrame(frame);
    return;
  }
  // applicable only to data
  // Note: includes padding!
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-4.2-1
  decrease_window_size(connection->myWindowSize, int32_t(frame.header.length));
  node->receiveRequestData(frame);
  if (frame.header.flags & flags::END_STREAM) {
    // Note: manages 'node' lifetime
    onRequestReady(*node);
  }
}

}  // namespace http2
