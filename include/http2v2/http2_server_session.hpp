
#pragma once

#include "http2v2/http2_connection.hpp"
#include "http2v2/http2_connection_establishment.hpp"
#include "http2v2/http2_connection_fwd.hpp"
#include "http2v2/http2_errors.hpp"
#include "http2v2/http2_protocol.hpp"
#include "http2v2/signewaiter_signal.hpp"
#include "http2v2/utils/boost_intrusive.hpp"

#include <boost/intrusive/list_hook.hpp>
#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/net/socket_defs.hh>

namespace http2 {
struct Server;
}

namespace http2v2 {

struct http2_frame_t;

// Not RAII type, must be closed (requestTerminate/shutdown + wait gate) before
// destroy
struct server_session : bi::list_base_hook<bi::link_mode<bi::safe_link>> {
  seastar::gate responsegate;
  // for connection reader/writer
  seastar::gate connectionPartsGate;
  // invariant: != nullptr
  http2_connection_ptr_t connection;
  http2_server_options options;
  seastar::abort_source sleepaborter;
  http2::Server *server = nullptr;
  // may be setted when newRequestsForbiden = true
  singlewaiter_signal sessiondone;
  // reader increments this value for detecting client idle
  size_t framecount = 0;
  // changed only once from 'false' to 'true' when shutdown requested
  bool newRequestsForbiden = false;
  bool terminated = false;
  bool done = false;

  // precondition: con != nullptr
  server_session(http2_connection_ptr_t con,
                 seastar::socket_address const &remoteAddress,
                 http2_server_options opts,
                 http2::Server &server KELCORO_LIFETIMEBOUND);

  server_session(server_session &&) = delete;
  void operator=(server_session &&) = delete;

  ~server_session();

  [[nodiscard]] size_t requestsLeft() const noexcept;

  // precondition: 'node' request completely assembled by server reader
  void onRequestReady(request_node &node) noexcept;

  // forbids new requests, but existing request handling continues
  // session will be closed when open requests are handled or network error
  // occurs
  void requestShutdown() noexcept;

  // forbids new requests, cancels current requests
  // session will be closed in near future
  void requestTerminate() noexcept;

  // used when request fully handled and response is sent
  void onResponseDone() noexcept;

  // returns false if no such stream
  bool rstStreamServer(stream_id_t streamid) noexcept;

  // invoked when session completely done
  void onSessionDone() noexcept;

  // creates new stream node, then server reader will collect request parts
  // until its ready then server will handle request and send response returns
  // reference to newly created node, which is alive until response sent /
  // request canceled there are 3 ways for created node:
  // * request assembled and then sent or canceled, in this case
  // onResponseDone() called by 'send_response'
  // * request not assembled and canceled, onResponseDone() called by
  // rstStreamServer()
  // * server terminates session, then onResponseDone called by
  // server_session::requestTerminate
  request_node &startRequestAssemble(stream_id_t);

  // after creation 3 hooks (requests, responses, timers) and 'task' left unused
  http2v2::node_ptr newEmptyStreamNode(stream_id_t);

  // used when settings changed while connection active
  // may throw protocol error
  // precondition: newsettings is SETTINGS frame
  void clientSettingsChanged(http2_frame_t newsettings);

  // used when server receives GOAWAY frame with NO_ERROR
  void clientRequestsGracefulShutdown(goaway_frame);

  struct response_written_awaiter {
    http2_connection *con = nullptr;
    request_node *n = nullptr;

    bool await_ready() noexcept // NOLINT
    {
      // data part and header marker for writer, that this request must be
      // awaiken after sending
      assert(con && n);
      assert(!n->onDataPart && !n->onHeader);
      assert(!n->requestsHook.is_linked());
      if (con->isDropped()) {
        n->status = reqerr_e::CANCELLED;
        return true;
      }
      // if not linked, request was canceled already, status is not DONE
      return !n->responsesHook.is_linked();
    }
    std::coroutine_handle<>
    await_suspend(dd::task<int>::handle_type h) noexcept // NOLINT
    {
      erase_byref(con->responses, *n);
      con->requests.push_back(*n);
      n->task = h;
      if (con->writer.handle) // if writer waits job now
      {
        return std::exchange(con->writer.handle, nullptr);
      }
      return std::noop_coroutine();
    }
    [[nodiscard]] bool await_resume() noexcept // NOLINT
    {
      return n->status > 0;
    }
  };

  // pushes node into send queue and notifies writer about it.
  // resumes when writer writes 'node' content
  // returns 'true' if response sent, false if error occurs
  KELCORO_CO_AWAIT_REQUIRED response_written_awaiter
  responseWritten(request_node &n) noexcept {
    return response_written_awaiter{connection.get(), &n};
  }

  // завершает запрос независимо от того собран он уже или нет
  // Вызывается для нештатного завершения,
  // для успешного завершения писатель вызывает connection.finishRequest
  void finishServerRequest(request_node &) noexcept;
};

} // namespace http2v2
