
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
dd::task<http2_connection_ptr_t> establish_http2_session_client(http2_connection_ptr_t con,
                                                                http2_client_options options);

// server drops connection if client inactive this amount of time
constexpr inline duration_t SERVER_DEFAULT_IDLE_TIMEOUT = std::chrono::seconds(25);
// timeout for sending client preface
constexpr inline duration_t SERVER_DEFAULT_CONNECTION_TIMEOUT = std::chrono::seconds(5);

struct http2_server_options {
  uint32_t hpackDyntabSize = 4096;
  // Note: disabling hpack from global config has higher priority
  bool forceDisableHpack = false;
  uint32_t maxReceiveFrameSize = FRAME_LEN_MAX;
  duration_t connectionTimeout = SERVER_DEFAULT_CONNECTION_TIMEOUT;
  // when drop client if it does not send anything
  duration_t idleTimeout = SERVER_DEFAULT_IDLE_TIMEOUT;
};

// creates server connection with client
// accepts unestablished session 'con' and returns established connection or
// exception
dd::task<http2_connection_ptr_t> establish_http2_session_server(http2_connection_ptr_t con,
                                                                http2_server_options);

}  // namespace http2
