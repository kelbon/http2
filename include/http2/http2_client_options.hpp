
#pragma once

#include "http2/utils/deadline.hpp"
#include "http2/utils/unique_name.hpp"

namespace http2 {

struct http2_client_options {
  // may be used to handle requests while sending big frames, such as files data
  // must not be 0
  uint32_t maxSendFrameSize = 8 * 1024;  // 8 KB
  uint32_t maxReceiveFrameSize = uint32_t(-1);
  uint32_t hpackDyntabSize = 4096;  // default value from rfc
  bool forceDisableHpack = false;   // if true, forces disabling hpack both for server and client
  // sends ping when there are no requests(for keeping alive).
  // duration_t::max() means disable pings
  // Note: its not recommended to disable pings
  duration_t pingInterval = std::chrono::seconds(5);
  duration_t connectionTimeout = std::chrono::seconds(1);
  // If the server does not respond to ping within this time, drops connection
  duration_t pingTimeout = std::chrono::seconds(10);
  // can significantly speed up the first requests on connection,
  // but not all servers correctly support it
  bool allow_requests_before_server_settings = true;
  // cannot be > 1 GB
  uint32_t max_continuation_len_bytes = uint32_t(-1);
  log_context logctx = {};
};

}  // namespace http2
