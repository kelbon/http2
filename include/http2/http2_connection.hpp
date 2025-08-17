
#pragma once

#include "http2/any_connection.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/http_base.hpp"
#include "http2/signewaiter_signal.hpp"
#include "http2/utils/boost_intrusive.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/utils/unique_name.hpp"
#include "http2/utils/fn_ref.hpp"
#include "http2/utils/timer.hpp"
#include "http2/utils/merged_segments.hpp"

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

  void validateHeader() const {
    if (header.length > FRAME_LEN_MAX || header.streamId > MAX_STREAM_ID)
      throw protocol_error(errc_e::PROTOCOL_ERROR, std::format("invalid frame header {}", header));
  }

  void validate_streamid() const {
    // SERVER_PUSH disabled always, so stream id must be odd
    if (header.streamId == 0 || (header.streamId % 2) == 0)
      throw protocol_error(errc_e::PROTOCOL_ERROR, std::format("invalid streamid, header: {}", header));
  }

  // returns false if incorrect frame
  // Note: not changes header.length, instead changes .data span. Its important
  // for control flow
  // makes sense only for DATA/HEADERS (they can be padded)
  void removePadding() {
    if (header.flags & flags::PADDED) [[unlikely]] {
      // padding len in first data byte
      strip_padding(data);
      // set flag to 0, so next 'removePadding' will not break frame
      header.flags &= flags_t(~flags::PADDED);
    }
  }

  // precondition: frame is HEADERS
  // throws on protocol error
  void ignoreDeprecatedPriority() {
    assert(header.type == frame_e::HEADERS);
    if (!(header.flags & flags::PRIORITY)) [[likely]] {
      return;
    }
    // options to disable priority is extension for protocol, it may not be
    // supported so i can receive it ignores
    //  [Exclusive (1)],
    //  [Stream Dependency (31)],
    //  [Weight (8)]
    static_assert(CHAR_BIT == 8);
    if (data.size() < 5) {
      throw protocol_error{
          errc_e::PROTOCOL_ERROR,
          std::format("invalid HEADERS frame with priority, data size < 5 ({})", data.size())};
    }
    uint32_t dependency;
    memcpy(&dependency, data.data(), sizeof(dependency));
    htonli(dependency);
    dependency &= uint32_t(0x7FFFFFFF);
    if (dependency == header.streamId) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("HEADER with priority depends on itself", header.streamId));
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
  using requests_hook_type = bi::list_member_hook<link_option_t>;
  using responses_hook_type = bi::unordered_set_member_hook<link_option_t>;
  using timers_hooks_type = bi::bs_set_member_hook<link_option_t>;

  // used when request started in connection.requests and when its free in
  // connection.freeNodes
  requests_hook_type requestsHook;
  responses_hook_type responsesHook;
  timers_hooks_type timersHook;
  uint32_t refcount = 0;
  stream_id_t streamid = 0;
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
  deadline_t deadline;
  dd::task<int>::handle_type task;  // setted by 'await_suspend' (requester)
  http2_connection_ptr_t connection = nullptr;
  // received resonse (filled by 'reader' in connection)
  on_header_fn_ptr onHeader;
  on_data_part_fn_ptr onDataPart;
  int status = reqerr_e::UNKNOWN_ERR;
  bool canceledByRstStream = false;  // for server request_context
  bool bidir_stream_active = false;  // true for CONNECT requests after accepting stream

  // filled if this a streaming request
  move_only_fn<streaming_body_t(http_headers_t& optional_trailers)> makebody;
  KELHTTP2_PIN;

  // connect request returns before END_STREAM when first HEADERS received and not send :path and :authority
  [[nodiscard]] bool is_connect_request() const noexcept {
    return req.method == http_method_e::CONNECT;
  }
  [[nodiscard]] bool is_streaming() const noexcept {
    return makebody.has_value();
  }
  [[nodiscard]] bool has_body() const noexcept {
    return is_streaming() || !req.body.data.empty();
  }

  // precondition: started
  [[nodiscard]] bool finished() const noexcept {
    return task == nullptr;
  }

  // returns true if stream was already assembled and response now in progress
  // server side
  [[nodiscard]] bool is_half_closed_server() const noexcept {
    // status >= 0 - запрос уже на стадии отправки
    return status == reqerr_e::RESPONSE_IN_PROGRESS || status >= 0;
  }

  // client side
  void receiveTrailersHeaders(hpack::decoder&, http2_frame_t /*headers frame*/);

  // server side
  void receiveRequestTrailers(hpack::decoder&, http2_frame_t /*headers frame*/);

  // client side
  // expects :status as first header
  // precondition: padding removed
  void receiveResponseHeaders(hpack::decoder& decoder, http2_frame_t frame);

  // client side
  // precondition: padding removed
  void receiveResponseData(http2_frame_t frame);

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

  std::string_view name() const noexcept;
};

// Note: shutdown must be called, its not RAII type because its not possible to
// stop writer/reader in shutdown with seastar
struct http2_connection {
  using requests_member_hook_t =
      bi::member_hook<request_node, request_node::requests_hook_type, &request_node::requestsHook>;
  using responses_member_hook_t =
      bi::member_hook<request_node, request_node::responses_hook_type, &request_node::responsesHook>;
  using timers_member_hook_t =
      bi::member_hook<request_node, request_node::timers_hooks_type, &request_node::timersHook>;

  using requests_t =
      bi::list<request_node, bi::cache_last<true>, requests_member_hook_t, bi::constant_time_size<true>>;

  using responses_t = bi::unordered_set<request_node, bi::constant_time_size<true>, responses_member_hook_t,
                                        bi::key_of_value<request_node::key_of_value>,
                                        bi::equal<request_node::equal_by_streamid>,
                                        bi::hash<request_node::hash_by_streamid>, bi::power_2_buckets<true>>;

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
  // правила для первого фрейма SETTINGS отличаются от правил для последующих
  bool first_settings_frame_received = false;
  hpack::decoder decoder;
  // odd, for client its last started stream, for server last stream started by client
  stream_id_t laststartedstreamid = 0;
  uint32_t refcount = 0;
  cfint_t myWindowSize = INITIAL_WINDOW_SIZE_FOR_CONNECTION_OVERALL;
  cfint_t receiverWindowSize = INITIAL_WINDOW_SIZE_FOR_CONNECTION_OVERALL;
  // setted only when writer is suspended and nullptr when works
  dd::job writer = {};
  requests_t requests;

  static constexpr inline size_t initial_buckets_count = 2;

  // Note: must be before 'responses' because of destroy ordering
  // invariant: .size is always pow of 2
  std::vector<responses_t::bucket_type> buckets;
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
  // all done stream ids stored here (before adding or search / 2 to map 1 3 5 to 0 1 2)
  merged_segments closed_streams;
  unique_name name;

  explicit http2_connection(any_connection_t&& c, boost::asio::io_context&);

  http2_connection(http2_connection&&) = delete;
  void operator=(http2_connection&&) = delete;

  ~http2_connection();

  void mark_stream_closed(stream_id_t id) noexcept {
    closed_streams.add_point(id / 2);
  }

  bool is_closed_stream(stream_id_t id) const noexcept {
    return closed_streams.has_point(id / 2);
  }

  // stream not yet started
  bool is_idle_stream(stream_id_t id) const noexcept {
    return id > laststartedstreamid;
  }

  [[nodiscard]] bool isDropped() const noexcept {
    return dropped;
  }

  void startDrop() noexcept {
    dropped = true;
  }

  // used when client send request and waits for response
  // OR
  // when server assebles request (in this case 'responses' used as hash table)
  // or server writes response and inserts into responses to catch WINDOW_UPDATe / RST_STREAM
  void insertResponseNode(request_node& node) {
    responses.insert(node);
    if (responses.size() == buckets.size()) [[unlikely]] {
      // https://github.com/boostorg/intrusive/issues/96
      // workaround:
      // unordered set bucket copy(move) ctor does nothing, so .resize will be UB
      decltype(buckets) new_buckets(buckets.size() * 2);
      responses.rehash({new_buckets.data(), new_buckets.size()});
      buckets = std::move(new_buckets);
    }
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
      // resumer should set it to nullptr
      assert(connection->writer.handle == nullptr);
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
    return laststartedstreamid >= MAX_STREAM_ID;
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
  // or if channel for streaming node throws
  void finishRequestWithUserException(request_node& node, std::exception_ptr) noexcept;

  // client side
  // returns false if no such stream
  [[nodiscard]] bool finishStreamWithError(rst_stream rstframe);

  void finishRequestByTimeout(request_node& node) noexcept {
    finishRequest(node, reqerr_e::TIMEOUT);
  }

  void finishAllWithReason(reqerr_e::values_e reason);

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
    if (laststartedstreamid == 0) [[unlikely]] {
      laststartedstreamid = 1;
      return laststartedstreamid;
    }
    assert(laststartedstreamid <= MAX_STREAM_ID);
    assert((laststartedstreamid % 2) == 1);
    laststartedstreamid += 2;
    return laststartedstreamid;
  }

  // 0 if there are no streams
  [[nodiscard]] stream_id_t lastInitiatedStreamId() const noexcept {
    return laststartedstreamid;
  }

  // client side
  // after creation 3 hooks (requests, responses, timers) and 'task' left unused
  node_ptr newRequestNode(http_request&& request, deadline_t deadline, on_header_fn_ptr onHeader,
                          on_data_part_fn_ptr onDataPart, stream_id_t streamid);

  // client side
  node_ptr newStreamingRequestNode(http_request&& request, deadline_t deadline, on_header_fn_ptr onHeader,
                                   on_data_part_fn_ptr onDataPart, stream_id_t streamid,
                                   move_only_fn<streaming_body_t(http_headers_t&)> makebody);

  void returnNode(request_node* ptr) noexcept;

  void ignoreFrame(http2_frame_t frame);

  // `remote_is_client` should be true on server side
  void settings_changed(http2_frame_t newsettings, bool remote_is_client);

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

  void validatePriorityFrameHeader(const http2_frame_t& h) {
    assert(h.header.type == frame_e::PRIORITY);
    assert(h.data.size() == h.header.length);
    if (h.header.length != 5 || h.header.streamId == 0)
      throw protocol_error(errc_e::PROTOCOL_ERROR, "invalid priority frame");
    uint32_t dependency;
    memcpy(&dependency, h.data.data(), sizeof(dependency));
    htonli(dependency);
    dependency &= uint32_t(0x7FFFFFFF);
    if (dependency == h.header.streamId) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("PRIORITY frame depends on itself", h.header.streamId));
    }
  }

  void validateRstFrame(const rst_stream& r) {
    if (is_idle_stream(r.header.streamId)) {
      throw protocol_error(errc_e::PROTOCOL_ERROR,
                           std::format("RST_STREAM frame on a idle stream {}", r.header.streamId));
    }
  }

  // работает для всех фреймов, но проверяет только максимальное ограничение размера
  void validate_frame_max_size(const frame_header& h) {
    using enum frame_e;
    assert(localSettings.maxFrameSize >= MIN_MAX_FRAME_LEN);
    if (h.length > localSettings.maxFrameSize) {
      throw protocol_error(errc_e::FRAME_SIZE_ERROR,
                           std::format("{} frame too big, max size: {}, frame size: {}", e2str(h.type),
                                       localSettings.maxFrameSize, h.length));
    }
  }

  // used when SETTINGS_INITIAL_WINDOW_SIZE changed
  void adjustWindowForAllStreams(cfint_t old_window_size, cfint_t new_window_size);

  inline void start_headers_block(request_node& node, bool force_disable_hpack, bytes_t& hdrs) {
    // https://www.rfc-editor.org/rfc/rfc9113.html#name-settings-synchronization
    if (encodertablesizechangerequested) [[unlikely]] {
      encodertablesizechangerequested = false;
      encoder.dyntab.set_user_protocol_max_size(remoteSettings.headerTableSize);
      // encoding dyntab size update also updates size
      if (force_disable_hpack) {
        encoder.encode_dynamic_table_size_update(0, std::back_inserter(hdrs));
      } else {
        // not sure if encoder required to send it, but its not error, so just set max size for new settings
        encoder.encode_dynamic_table_size_update(encoder.dyntab.user_protocol_max_size(),
                                                 std::back_inserter(hdrs));
      }
    } else if (node.streamid == 1 && force_disable_hpack) [[unlikely]] {
      encoder.encode_dynamic_table_size_update(0, std::back_inserter(hdrs));
    }
  }

  // collects HEADERS from many CONTINUATIONS and first HEADERS frame without END_HEADERS and passes it into
  // `when_done` invokes `oneachframe` when receives new frame header
  dd::task<void> receive_headers_with_continuation(http2_frame_t frame, io_error_code& ec,
                                                   move_only_fn<void()> oneachframe,
                                                   move_only_fn<void(http2_frame_t)> whendone);

  void client_receive_headers(http2_frame_t frame);

  void client_receive_data(http2_frame_t frame);
};

#ifdef HTTP2_ENABLE_TRACE
void trace_request_headers(request_node const&, bool fromclient);
#endif

}  // namespace http2
