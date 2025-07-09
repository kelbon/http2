
#pragma once

#include "http2v2/http_base.hpp"

#include <yacore/http2/Request.hpp>
#include <yacore/http2/Response.hpp>

namespace http2v2 {

// postcondition: all 'str' symbols are lowered
void lower_string(std::span<char> str);

http_request req_cast(http2::Request r);

http2::Request req_uncast(http_request r);

} // namespace http2v2
