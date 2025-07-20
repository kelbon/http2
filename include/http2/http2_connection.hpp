
#pragma once

#include "http2/any_connection.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http_base.hpp"
#include "http2/signewaiter_signal.hpp"
#include "http2/utils/boost_intrusive.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/utils/fn_ref.hpp"
#include "http2/utils/timer.hpp"
#include <boost/asio/io_context.hpp>

#include <span>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/list_hook.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/intrusive/treap_set.hpp>
#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive_ptr.hpp>
#include <kelcoro/job.hpp>
#include <kelcoro/task.hpp>

namespace http2 {

struct http2_frame_t {
  frame_header header;
  std::span<byte_t> data;

  // returns false if incorrect header
  [[nodiscard]] bool validateHeader() const noexcept {
    return header.length <= FRAME_LEN_MAX && header.streamId <= MAX_STREAM_ID;
  }
  // returns false if incorrect frame
  // Note: not changes header.length, instead changes .data span. Its important
  // for control flow
  [[nodiscard]] bool removePadding() noexcept {
    if (header.flags & flags::PADDED) [[unlikely]] {
      // padding len in first data byte
      if (!strip_padding(data)) {
        return false;
      }
      // set flag to 0, so next 'removePadding' will not break frame
      header.flags &= flags_t(~flags::PADDED);
    }
    return true;
  }

  // precondition: frame is HEADERS
  // throws on protocol error
  void ignoreDeprecatedPriority() {
    if (!(header.flags & flags::PRIORITY)) [[likely]] {
      return;
    }
    HTTP2_LOG(INFO, "received PRIORITY headers frame");
    // options to disable priority is extension for protocol, it may not be
    // supported so i can receive it ignores
    //  [Exclusive (1)],
    //  [Stream Dependency (31)],
    //  [Weight (8)]
    static_assert(CHAR_BIT == 8);
    if (data.size() < 5) {
      HTTP2_LOG(ERROR,
                "incorrect PRIORITY headers frame, len: {}, streamid: {}, "
                "flags: {}, type: {}. Len after padding rm: {}",
                header.length, header.streamId, header.flags, (int)header.type, data.size());
      throw protocol_error{};
    }
    remove_prefix(data, 5);
    // set flag to 0, so next 'ignoreDeprecatedPriority' will not break frame
    header.flags &= flags_t(~flags::PRIORITY);
  }
};

// request starts in connection.requests, then goes into connection.responses
// Note: this type used in client when sending request
// and reused in server, in this case its response for sending in writer (.req
// field stores response)
struct request_node {
  // used when request started in connection.requests and when its free in
  // connection.freeNodes
  bi::list_member_hook<link_option_t> requestsHook;
  bi::unordered_set_member_hook<link_option_t> responsesHook;
  bi::bs_set_member_hook<link_option_t> timersHook;
  uint32_t refcount = 0;
  // local -> remote hope
  // inited from remote settings
  // how many octets local can send to remote
  cfint_t lrStreamlevelWindowSize = 0;
  // remote -> local hope
  // inited from local settings
  // how many octets remote can send to local
  cfint_t rlStreamlevelWindowSize = 0;
  // 'newRequestNode' fills req, deadline and initial stream window sizes
  http_request req;
  stream_id_t streamid = 0;
  deadline_t deadline;
  dd::task<int>::handle_type task;  // setted by 'await_suspend' (requester)
  http2_connection_ptr_t connection = nullptr;
  // received resonse (filled by 'reader' in connection)
  on_header_fn_ptr onHeader;
  on_data_part_fn_ptr onDataPart;
  int status = reqerr_e::UNKNOWN_ERR;
  KELHTTP2_PIN;

  // precondition: started ()
  [[nodiscard]] bool finished() const noexcept {
    return task == nullptr;
  }

  void receiveTrailersHeaders(hpack::decoder& decoder, http2_frame_t frame);

  // client side
  // expects :status as first header
  // precondition: padding removed
  void receiveResponseHeaders(hpack::decoder& decoder, http2_frame_t frame);

  // precondition: padding removed
  void receiveData(http2_frame_t frame);

  // server side
  // expects required pseudoheaders like :path
  // precondition: padding removed
  void receiveRequestHeaders(hpack::decoder& decoder, http2_frame_t frame);

  // server side
  // adds frame data octets to request body
  // precondition: padding removed
  void receiveRequestData(http2_frame_t frame);

  struct equal_by_streamid {
    bool operator()(stream_id_t const& l, stream_id_t const& r) const noexcept {
      return l == r;
    }
  };
  struct key_of_value {
    using type = stream_id_t;
    type const& operator()(request_node const& v) const noexcept {
      return v.streamid;
    }
  };
  struct hash_by_streamid {
    size_t operator()(stream_id_t s) const noexcept {
      // only grow (1, 3, 5...) so last bit is always 1, never intersects
      // uint32_t, so always less uint32_t max (high bits always false and
      // fixable by |, but i dont need it) proved as best possible hash for this
      // case in bench
      return s >> 1;
    }
  };
  struct compare_by_deadline {
    bool operator()(request_node const& l, request_node const& r) const noexcept {
      return l.deadline < r.deadline;  // less means higher priority
    }
  };
};

// Note: shutdown must be called, its not RAII type because its not possible to
// stop writer/reader in shutdown with seastar
struct http2_connection {
  using requests_member_hook_t =
      bi::member_hook<request_node, bi::list_member_hook<link_option_t>, &request_node::requestsHook>;
  using responses_member_hook_t = bi::member_hook<request_node, bi::unordered_set_member_hook<link_option_t>,
                                                  &request_node::responsesHook>;
  using requests_t = bi::list<request_node, bi::cache_last<true>, requests_member_hook_t>;
  using responses_t = bi::unordered_set<request_node, bi::constant_time_size<true>, responses_member_hook_t,
                                        bi::key_of_value<request_node::key_of_value>,
                                        bi::equal<request_node::equal_by_streamid>,
                                        bi::hash<request_node::hash_by_streamid>, bi::power_2_buckets<true>>;
  using timers_member_hook_t =
      bi::member_hook<request_node, bi::bs_set_member_hook<link_option_t>, &request_node::timersHook>;
  using timers_t = bi::treap_multiset<request_node, bi::constant_time_size<true>, timers_member_hook_t,
                                      bi::priority<request_node::compare_by_deadline>,
                                      bi::compare<request_node::compare_by_deadline>>;

  settings_t remoteSettings;
  settings_t localSettings;
  any_connection_t tcpCon;
  hpack::encoder encoder;
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2-2.2.1
  // Server may send SETTINGS frame with reduced hpack table size,
  // this means request for client encoder to send dynamic_size_update
  //
  // if true, new value in server_settings.header_table_size
  bool encodertablesizechangerequested = false;
  hpack::decoder decoder;
  // odd for client, even for server
  stream_id_t streamid = 0;
  uint32_t refcount = 0;
  cfint_t myWindowSize = INITIAL_WINDOW_SIZE_FOR_CONNECTION_OVERALL;
  cfint_t receiverWindowSize = INITIAL_WINDOW_SIZE_FOR_CONNECTION_OVERALL;
  // setted only when writer is suspended and nullptr when works
  dd::job writer;
  requests_t requests;
  // Note: must be before 'responses' because of destroy ordering
  static constexpr inline size_t buckets_count = 256;
  responses_t::bucket_type buckets[buckets_count];
  responses_t responses;
  timers_t timers;
  bool dropped = false;  // setted ONLY in dropConnection
  // if goaway with NO_ERROR was already sended. This ensures, that we will
  // initiate goaway (in coStop) OR server initiates goaway and we answered once
  bool gracefulshutdownGoawaySended = false;
  timer_t pingtimer;
  timer_t pingdeadlinetimer;
  timer_t timeoutWardenTimer;
  bi::slist<request_node, requests_member_hook_t, bi::constant_time_size<true>> freeNodes;

  explicit http2_connection(any_connection_t&& c, boost::asio::io_context&);

  http2_connection(http2_connection&&) = delete;
  void operator=(http2_connection&&) = delete;

  ~http2_connection();

  [[nodiscard]] bool isDropped() const noexcept {
    return dropped;
  }
  void startDrop() noexcept {
    dropped = true;
  }

  // interface for writer only

  struct work_waiter {
    http2_connection* connection = nullptr;
    KELHTTP2_PIN;

    bool await_ready() const noexcept {
      return !connection->requests.empty() || connection->isDropped();
    }
    void await_suspend(std::coroutine_handle<dd::job_promise> writer) noexcept {
      assert(connection->writer.handle == nullptr);
      connection->writer.handle = writer;
    }
    [[nodiscard]] bool await_resume() const noexcept {
      connection->writer.handle = nullptr;
      return !connection->isDropped();
    }
  };

  // postcondition: if returned true, then !requests.empty() && connection not
  // dropped Note: worker may be still has no right to work (too many streams)
  [[nodiscard]] work_waiter waitWork() noexcept {
    return work_waiter(this);
  }

  write_awaiter write(std::span<byte_t const> bytes, io_error_code& ec) {
    return write_awaiter{tcpCon, ec, bytes};
  }
  read_awaiter read(std::span<byte_t> buf, io_error_code& ec) {
    return read_awaiter{tcpCon, ec, buf};
  }

  [[nodiscard]] size_t concurrentStreamsNow() noexcept {
    return responses.size();
  }

  // interface for reader

  [[nodiscard]] bool isOutofStreamids() const noexcept {
    return streamid > MAX_STREAM_ID;
  }

  void initiateGracefulShutdown(stream_id_t laststreamid) noexcept;

  // when streamid is max, connection is not broken, but required to stop
  [[nodiscard]] bool isDoneCompletely() const noexcept {
    return isOutofStreamids() && requests.empty() && responses.empty();
  }

  void forget(request_node& node) noexcept;

  // ALL streams must be finished by calling this function except when user
  // exception throwed from on_header/on_data_part callbacks
  void finishRequest(request_node& node, int status) noexcept;

  // used only when user exception throwed from on_header/on_data_part callbacks
  void finishRequestWithUserException(request_node& node, std::exception_ptr) noexcept;

  // client side
  // returns false if no such stream
  [[nodiscard]] bool finishStreamWithError(rst_stream rstframe);

  void finishRequestByTimeout(request_node& node) noexcept {
    finishRequest(node, reqerr_e::TIMEOUT);
  }

  void finishAllWithException(reqerr_e::values_e reason);

  [[nodiscard]] request_node* findResponseByStreamid(stream_id_t id) noexcept;

  void dropTimeouted();

  // used when window update received from remote endpoint
  void windowUpdate(window_update_frame frame);

  // cancels all requests and responses etc, but not shutdowns tcp connection
  // returns true if first shutdown
  bool prepareToShutdown(reqerr_e::values_e reason) noexcept;

  void shutdown(reqerr_e::values_e reason) noexcept;

  // interface for sendRequest

  // postcondition: returns correct streamid (<= MAX_STREAM_ID)
  [[nodiscard]] stream_id_t nextStreamid() noexcept {
    // out of stream ids == (2 ^ 31 - 1) + 2
    assert(streamid <= (MAX_STREAM_ID + 2));
    assert((streamid % 2) == 1);  // client side
    auto id = streamid;
    streamid += 2;
    return id;
  }

  // client side, server should use just stream id
  [[nodiscard]] stream_id_t lastInitiatedStreamId() const noexcept {
    // streamid is id which will be setted to next stream, so prev stream is -2
    return streamid < 2 ? 0 : streamid - 2;
  }

  // client side
  // after creation 3 hooks (requests, responses, timers) and 'task' left unused
  node_ptr newRequestNode(http_request&& request, deadline_t deadline, on_header_fn_ptr onHeader,
                          on_data_part_fn_ptr onDataPart, stream_id_t streamid);

  void returnNode(request_node* ptr) noexcept;

  void ignoreFrame(http2_frame_t frame);

  // client side
  // used when settings changed while connection active
  // may throw protocol error
  // precondition: newsettings is SETTINGS frame
  void serverSettingsChanged(http2_frame_t newsettings);

  // client side
  // used when client receives GOAWAY frame with NO_ERROR (or may be second
  // goaway from server) forbids creating new requests, sends goaway answer
  void serverRequestsGracefulShutdown(goaway_frame);

  struct response_awaiter {
    http2_connection* con = nullptr;
    request_node* n = nullptr;

    static bool await_ready() noexcept {
      return false;
    }

    std::coroutine_handle<> await_suspend(dd::task<int>::handle_type h) noexcept {
      n->task = h;
      if (con->writer.handle)  // if writer waits job now
      {
        return std::exchange(con->writer.handle, nullptr);
      }
      return std::noop_coroutine();
    }

    [[nodiscard]] int await_resume() const noexcept {
      return n->status;
    }
  };

  // client side
  // Waits until response received (or request_node finished somehow else)
  // and returns response status
  KELCORO_CO_AWAIT_REQUIRED response_awaiter responseReceived(request_node& node) noexcept;

  void validatePriorityFrameHeader(const frame_header& h) {
    assert(h.type == frame_e::PRIORITY);
    if (h.length != 5 || h.streamId == 0)
      throw protocol_error(errc_e::PROTOCOL_ERROR, "invalid priority frame");
  }

  void validateFrame(const rst_stream& r) {
    if (r.header.streamId > this->streamid) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("RST_STREAM frame on a idle stream {}", streamid));
    }
  }
  // precondition: h.type is DATA / HEADERS / CONTINUATION
  void validateDataOrHeadersFrameSize(const frame_header& h) {
    using enum frame_e;
    assert(localSettings.maxFrameSize >= MIN_MAX_FRAME_LEN);
    assert(h.type == DATA || h.type == HEADERS || h.type == CONTINUATION);
    if (h.length > localSettings.maxFrameSize) {
      throw protocol_error(errc_e::FRAME_SIZE_ERROR,
                           std::format("{} frame too big, max size: {}, frame size: {}", e2str(h.type),
                                       localSettings.maxFrameSize, h.length));
    }
  }
};

#ifdef HTTP2_ENABLE_TRACE
void trace_request_headers(http2::http_request const& req, bool fromclient);
#endif

}  // namespace http2
