
#pragma once

#include "http2/http2_connection.hpp"
#include "http2/http2_connection_establishment.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http2_errors.hpp"
#include "http2/http2_protocol.hpp"

#include <boost/intrusive/list_hook.hpp>

#include <kelcoro/gate.hpp>

namespace http2 {

struct http2_server;
struct http2_frame_t;

// Not RAII type, must be closed (requestTerminate/shutdown + wait gate) before
// destroy
struct server_session : bi::list_base_hook<bi::link_mode<bi::safe_link>> {
  uint32_t refcount = 0;
  dd::gate responsegate;
  // for connection reader/writer
  dd::gate connectionPartsGate;
  // invariant: != nullptr
  http2_connection_ptr_t connection;
  http2_server_options options;
  http2_server* server = nullptr;
  // reader increments this value for detecting client idle
  size_t framecount = 0;
  // changed only once from 'false' to 'true' when shutdown requested
  bool newRequestsForbiden = false;
  bool terminated = false;
  bool done = false;
  // used to check if goaway required when shutting down. Do not send goaway if session not established yet
  bool established = false;

  // precondition: con != nullptr
  server_session(http2_connection_ptr_t con, http2_server_options opts,
                 http2_server& server KELCORO_LIFETIMEBOUND);

  server_session(server_session&&) = delete;
  void operator=(server_session&&) = delete;

  ~server_session();

  [[nodiscard]] bool hasUnfinishedRequests() const noexcept {
    return !connection->requests.empty() || !connection->responses.empty();
  }
  [[nodiscard]] size_t requestsLeftApprox() const noexcept {
    return connection->requests.size() + connection->responses.size();
  }
  [[nodiscard]] size_t requestsLeftExactly() const noexcept;

  // precondition: 'node' request completely assembled by server reader
  void onRequestReady(h2stream& node) noexcept;

  // forbids new requests, but existing request handling continues
  // session will be closed when open requests are handled or network error
  // occurs
  void requestShutdown() noexcept;

  // forbids new requests, cancels current requests
  // session will be closed in near future
  void requestTerminate() noexcept;

  // used when request fully handled and response is sent
  void onResponseDone() noexcept;

  // called when client sent RST_STREAM
  // returns false if no such stream
  bool rstStreamServer(rst_stream);

  void rstStreamAfterError(stream_error const&);

  // invoked when session completely done
  void onSessionDone() noexcept;

  void receive_headers(http2_frame_t frame);

  // precondition: `frame` is DATA
  void receive_data(http2_frame_t frame);

  // marks client as not idle
  void received_frame() {
    ++framecount;
    if (connection->pingdeadlinetimer.armed()) [[unlikely]] {  // client not idle
      connection->pingdeadlinetimer.cancel();
    }
  }

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
  //
  // Note: may accept trailers headers too
  void startRequestAssemble(const http2_frame_t& /*HEADERS frame*/);

  // after creation 3 hooks (requests, responses, timers) and 'task' left unused
  stream_ptr new_empty_stream_node(stream_id_t);

  // used when settings changed while connection active
  // may throw protocol error
  // precondition: newsettings is SETTINGS frame
  void clientSettingsChanged(http2_frame_t newsettings);

  // used when server receives GOAWAY frame with NO_ERROR
  void clientRequestsGracefulShutdown(goaway_frame);

  struct response_written_awaiter {
    http2_connection* con = nullptr;
    h2stream* n = nullptr;

    bool await_ready() noexcept {
      // data part and header marker for writer, that this request must be
      // awaiken after sending
      assert(con && n);
      assert(!n->requestsHook.is_linked());
      if (con->isDropped()) {
        n->status = reqerr_e::CANCELLED;
        return true;
      }
      // if not linked, request was canceled already, status is not DONE
      return !n->responsesHook.is_linked();
    }
    std::coroutine_handle<> await_suspend(dd::task<int>::handle_type h) noexcept {
      con->requests.push_back(*n);
      n->task = h;
      if (con->writer.handle)  // if writer waits job now
      {
        return std::exchange(con->writer.handle, nullptr);
      }
      return std::noop_coroutine();
    }
    [[nodiscard]] bool await_resume() noexcept {
      return n->status > 0;
    }
  };

  // pushes node into send queue and notifies writer about it.
  // resumes when writer writes 'node' content
  // returns 'true' if response sent, false if error occurs
  KELCORO_CO_AWAIT_REQUIRED response_written_awaiter responseWritten(h2stream& n) noexcept {
    return response_written_awaiter{connection.get(), &n};
  }

  // завершает запрос независимо от того собран он уже или нет
  // Вызывается для нештатного завершения,
  // для успешного завершения писатель вызывает connection.finishRequest
  void finishServerRequest(h2stream&) noexcept;

  const unique_name& name() const noexcept {
    return connection->name;
  }
};

inline void intrusive_ptr_add_ref(server_session* p) noexcept {
  ++p->refcount;
}

inline void intrusive_ptr_release(server_session* p) noexcept {
  --p->refcount;
  if (p->refcount == 0) {
    delete p;
  }
}

using server_session_ptr = boost::intrusive_ptr<server_session>;

}  // namespace http2
