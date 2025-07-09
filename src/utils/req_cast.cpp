

#include "http2v2/utils/req_cast.hpp"

namespace http2v2 {

void lower_string(std::span<char> str) {
  for (char &c : str) {
    c = static_cast<char>(tolower(c));
  }
}

// friend of Request
struct request_bro {
  static http_request &access(http2::Request &r) { return r.m_req; }
};

http_request req_cast(http2::Request r) {
  return std::move(request_bro::access(r));
}

http2::Request req_uncast(http_request r) {
  http2::Request req;
  request_bro::access(req) = std::move(r);
  return req;
}

} // namespace http2v2
