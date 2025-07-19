
#pragma once

#include "http2/utils/deadline.hpp"

namespace http2 {

struct http2_client_options {
  // may be used to handle requests while sending big frames, such as files data
  // must not be 0
  uint32_t maxSendFrameSize = 8 * 1024;  // 8 KB
  uint32_t maxReceiveFrameSize = uint32_t(-1);
  uint32_t hpackDyntabSize = 4096;  // default value from rfc
  bool forceDisableHpack = false;   // if true, forces disabling hpack both for server and client
  // sends ping when there are no requests(for keeping alive). disabled by
  // default
  duration_t pingInterval = duration_t::max();
  duration_t connectionTimeout = std::chrono::seconds(1);
  // If the server does not respond to ping within this time, drops connection
  duration_t pingTimeout = std::chrono::seconds(10);
};

}  // namespace http2
