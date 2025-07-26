
#pragma once

#include "http2/http2_client_options.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http_base.hpp"
#include "http2/transport_factory.hpp"
#include "http2/utils/boost_intrusive.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/utils/gate.hpp"
#include "http2/utils/unique_name.hpp"

#include <memory>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <kelcoro/job.hpp>
#include <kelcoro/task.hpp>

#include <zal/zal.hpp>

#undef NO_DATA

namespace http2 {

struct http2_client;

namespace noexport {

struct waiter_of_connection : bi::list_base_hook<link_option_t> {
  std::coroutine_handle<> task;
  http2_client* client = nullptr;
  http2_connection_ptr_t result = nullptr;
  deadline_t deadline;
  KELHTTP2_PIN;

  explicit waiter_of_connection(http2_client* c, deadline_t dl) noexcept : client(c), deadline(dl) {
  }

  ~waiter_of_connection();

  bool await_ready() noexcept;
  std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept;
  [[nodiscard]] http2_connection_ptr_t await_resume();
};

// prevents new connections while alive
struct new_connection_guard {
 private:
  size_t* m_isConnecting;

 public:
  explicit new_connection_guard(size_t& b) noexcept : m_isConnecting(&b) {
    ++*m_isConnecting;
  }
  new_connection_guard(new_connection_guard&&) = delete;
  void operator=(new_connection_guard&&) = delete;

  void release() noexcept {
    if (m_isConnecting) {
      --*m_isConnecting;
      m_isConnecting = nullptr;
    }
  }
  ~new_connection_guard() {
    release();
  }
};

}  // namespace noexport
}  // namespace http2

namespace http2 {

struct http2_client {
 protected:
  friend noexport::waiter_of_connection;

  // on top bcs of destroy order
  asio::io_context m_ioctx = asio::io_context(1);
  endpoint_t m_host;
  http2_client_options m_options;
  http2_connection_ptr_t m_connection;
  // invariant: .!= nullptr, unchanged after creation
  any_transport_factory m_factory;

  // while connection is not ready all new streams wait for it
  bi::list<noexport::waiter_of_connection, bi::cache_last<true>> m_connectionWaiters;
  size_t m_requestsInProgress = 0;
  size_t m_isConnecting = 0;  // if connection started to establish, but not established yet
  // present when m_isConnecting > 0. Used to drop in-flight connection when
  // stopping
  http2_connection_ptr_t m_notYetReadyConnection = nullptr;
  size_t m_stopRequested = 0;
  //  used to correctly wait in 'stop' while all connect calls will end
  gate m_connectionGate;
  // for connection reader/writer
  gate m_connectionPartsGate;
  unique_name m_name;

  // fills requests from raw http2 frames
  static dd::job startReaderFor(http2_client*, http2_connection_ptr_t);

  // postconditon: returns not null, !returned->dropped && returned->stream_id
  // <= MAX_STREAM_ID
  // && !client.stop_requestedg
  [[nodiscard]] noexport::waiter_of_connection borrowConnection(deadline_t deadline) noexcept {
    return noexport::waiter_of_connection(this, deadline);
  }

  void notifyConnectionWaiters(http2_connection_ptr_t result) noexcept;

  [[nodiscard]] bool alreadyConnecting() const noexcept {
    return m_isConnecting > 0;
  }
  [[nodiscard]] noexport::new_connection_guard lockConnections() noexcept {
    return noexport::new_connection_guard(m_isConnecting);
  }

  // поддерживает инвариант: клиент либо не имеет соединения, либо оно в
  // процессе создания, либо оно создано, но не более одного
  [[nodiscard("this handle must be resumed")]] static dd::job startConnecting(http2_client*, deadline_t);

  bool stopRequested() const noexcept {
    return m_stopRequested > 0;
  }

  dd::task<void> sleep(duration_t, io_error_code&);

 public:
  // 'host' used for
  //   * connecting when required
  //  but 'host' header in requests may differ
  // note: same as :authority for HTTP2
  explicit http2_client(endpoint_t host, http2_client_options opts)
      : http2_client(std::move(host), std::move(opts), default_transport_factory(m_ioctx)) {
    m_name.set_prefix(CLIENT_PREFIX);
  }
  explicit http2_client(endpoint_t host, http2_client_options, any_transport_factory);

  http2_client(http2_client&&) = delete;
  void operator=(http2_client&&) = delete;

  endpoint_t const& getHost() const noexcept {
    return m_host;
  }

  void setConnectionTimeout(duration_t dur) noexcept {
    m_options.connectionTimeout = dur;
  }
  http2_client_options const& getOptions() const noexcept {
    return m_options;
  }

  ~http2_client();

  // rethrows exceptions from 'on_header' and 'on_data_part' to caller
  // if 'on_header' is nullptr, all headers ignored (status parsed if
  // 'on_data_part' != nullptr) if 'on_data_part' is nullptr, then server answer
  // ignored # (may be in future) if both nullptr, then request only sended and
  // then returns immediately returns < 0 if error (reqerr_e), 0 if request
  // done, but both handlers nullptr and status not parsed > 0 if 3-digit server
  // response code
  dd::task<int> sendRequest(on_header_fn_ptr onHeader, on_data_part_fn_ptr onDataPart, http_request,
                            deadline_t deadline);

  // throws on errors
  dd::task<http_response> sendRequest(http_request, deadline_t);

  dd::task<http_response> sendRequest(http_request request, duration_t timeout) {
    return sendRequest(std::move(request), deadline_after(timeout));
  }

  bool connected() const;

  void setHost(endpoint_t) noexcept;

  // returns true if client connected after future resoloves
  dd::task<bool> tryConnect(deadline_t);

  // returns true if client connected after future resoloves
  dd::task<bool> tryConnect(duration_t d) {
    return tryConnect(deadline_after(d));
  }

  // returns true if client connected after future resoloves
  dd::task<bool> tryConnect() {
    return tryConnect(deadline_after(m_options.connectionTimeout));
  }

  // ждёт завершения всех стримов и затем останавливается
  // postcondition: *this в состоянии как будто только конструктора, connected()
  // == false
  dd::task<void> coStop();

  // aborts all requsts and stops
  dd::task<void> coAbort();

  void stop();

  bool isHttps() const noexcept;

  // postcondition: !m_connection. Mostly used by client itself
  void dropConnection(reqerr_e::values_e reason) noexcept;

  bool connectionInProgress() const noexcept {
    return m_notYetReadyConnection != nullptr;
  }

  asio::io_context& ioctx() {
    return m_ioctx;
  }

  [[nodiscard]] unique_name const& name() const noexcept {
    return m_name;
  }
};

}  // namespace http2
