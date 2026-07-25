
#pragma once

#include "hidi/http_body_bytes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hidi {

using bytes_t = std::vector<uint8_t>;

struct http_body {
  std::string content_type;
  http_body_bytes data;

  std::string_view strview() const noexcept {
    return {(const char*)data.data(), data.size()};
  }
};

}  // namespace hidi
