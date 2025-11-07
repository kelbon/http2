
#pragma once

#include "http2/http2_client_options.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http2_protocol.hpp"
#include "http2/utils/deadline.hpp"

#include <kelcoro/task.hpp>

namespace http2 {

// creates client connection with server
// accepts unestablished session 'con' and returns established connection or
// exception
dd::task<h2connection_ptr> establish_http2_session_client(h2connection_ptr con, http2_client_options options);

// server drops connection if client inactive this amount of time
constexpr inline duration_t SERVER_DEFAULT_IDLE_TIMEOUT = std::chrono::seconds(25);
// timeout for sending client preface
constexpr inline duration_t SERVER_DEFAULT_CONNECTION_TIMEOUT = std::chrono::seconds(5);

struct http2_server_options {
  uint32_t hpackDyntabSize = 4096;
  // Note: disabling hpack from global config has higher priority
  bool forceDisableHpack = false;
  uint32_t maxReceiveFrameSize = FRAME_LEN_MAX;
  // how many streams client may run concurrently, default: max possible
  uint32_t maxConcurrentStreams = uint32_t(-1);
  duration_t connectionTimeout = SERVER_DEFAULT_CONNECTION_TIMEOUT;
  // when drop client if it does not send anything
  duration_t idleTimeout = SERVER_DEFAULT_IDLE_TIMEOUT;
  // if false, server will not declare websocket support for clients
  bool supports_websocket = false;
  // Как много байт может быть использовано одной клиент-сервер сессией единовременно для сборки и обработки
  // запросов.
  // Не даёт клиенту забить память сервера посылкой больших запросов без отправки END_STREAM
  // Note: лимит выставляется на все запросы сессии вместе, а не на один
  size_t limit_requests_memory_usage_bytes = size_t(-1);
  // При превышении лимита новые соединения будут отброшены
  size_t limit_clients_count = size_t(-1);
  // cannot be > 1 GB
  uint32_t max_continuation_len_bytes = uint32_t(-1);
};

// creates server connection with client
// accepts unestablished session 'con' and returns established connection or
// exception
dd::task<h2connection_ptr> establish_http2_session_server(h2connection_ptr con, http2_server_options);

}  // namespace http2
