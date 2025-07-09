
#pragma once

#include "http2v2/http_body_bytes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace http2v2 {

using bytes_t = std::vector<uint8_t>;

struct http_body {
  std::string contentType;
  http_body_bytes data;
};

} // namespace http2v2
