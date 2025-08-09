#pragma once

#include "http2/fuzzing/fuzzer.hpp"
#include "http2/http_base.hpp"
#include "http2/utils/deadline.hpp"
#include "http2/utils/memory_queue.hpp"
#include "http2/fuzzing/assertion.hpp"

#include <array>

namespace http2::fuzzing {

struct methods_weights {
  // Веса методов (можно менять)
  std::array<int, 9> weights = {
      10,  // GET
      5,   // POST
      3,   // PUT
      2,   // DELETE
      1,   // PATCH
      0,   // OPTIONS
      0,   // HEAD
      0,   // CONNECT
      0,   // TRACE
  };

  int& weight_of(http_method_e e) noexcept {
    assert(int(e) < weights.size());
    return weights[int(e)];
  }
  const int& weight_of(http_method_e e) const noexcept {
    assert(int(e) < weights.size());
    return weights[int(e)];
  }
};

std::span<const std::string_view> realistic_valid_pathes();
std::span<const std::string_view> realistic_valid_authorities();
std::span<const std::string_view> realistic_invalid_authorities();
std::span<const http_header_t> realistic_valid_http2_headers();
std::span<const http_header_t> realistic_invalid_http2_headers();

// returns request + is_valid
struct hreq {
  http_request request;
  http_headers_t trailers;
  deadline_t deadline;
  bool is_valid = true;
};

struct hreq_template {
  methods_weights method_template;
  double valid_path_prob = 1.0;
  double authority_prob = 0.85;
  size_t min_hdrs_count = 0;
  size_t max_hdrs_count = 5;
  double valid_hdr_prob = 1.0;
  double body_prob = 0.4;
  size_t min_body_sz = 1;
  size_t max_body_sz = 100;
  double body_content_type_prob = 0.5;
  double trailers_prob = 0.1;
  size_t min_trailer_hdrs_count = 1;  // not less then 1!
  size_t max_trailer_hdrs_count = 5;
  double deadline_prob = 0.5;  // has deadline or no

  [[nodiscard]] hreq generate_request(fuzzer& fuz) const {
    hreq r;
    bool invalid = false;
    r.request.method = gen_method(fuz, invalid);
    r.request.authority = gen_authority(fuz, invalid);
    r.request.path = gen_path(fuz, invalid);
    r.request.headers = gen_headers(fuz, invalid);
    r.request.body = gen_body(fuz);
    r.trailers = gen_trailers(fuz, invalid);
    if (fuz.rbool(deadline_prob)) {
      r.deadline = deadline_after(std::chrono::milliseconds(fuz.rint(500, 5000)));
    }
    r.is_valid = !invalid;
    return r;
  }

 private:
  http_method_e gen_method(fuzzer& fuz, bool&) const {
    return http_method_e(fuz.rindex(method_template.weights.size()));
  }

  std::string_view gen_path(fuzzer& fuz, bool&) const {
    if (fuz.rbool(valid_path_prob))
      return fuz.select(realistic_valid_pathes());
    else
      return "";  // empty path is invalid
  }

  std::string_view gen_authority(fuzzer& fuz, bool&) const {
    if (fuz.rbool(authority_prob))
      return fuz.select(realistic_valid_authorities());
    else
      return "";  // unsetted
  }

  http_headers_t gen_headers(fuzzer& fuz, bool& invalid) const {
    http_headers_t hdrs;
    size_t count = fuz.rint(min_hdrs_count, max_hdrs_count);
    for (size_t i = 0; i < count; ++i) {
      if (fuz.rbool(valid_hdr_prob)) {
        hdrs.push_back(fuz.select(realistic_valid_http2_headers()));
      } else {
        hdrs.push_back(fuz.select(realistic_invalid_http2_headers()));
        invalid = true;
      }
    }
    return hdrs;
  }

  http_headers_t gen_trailers(fuzzer& fuz, bool& invalid) const {
    if (!fuz.rbool(trailers_prob))
      return {};
    http_headers_t hdrs;
    size_t count = fuz.rint(min_trailer_hdrs_count, max_trailer_hdrs_count);
    for (size_t i = 0; i < count; ++i) {
      if (fuz.rbool(valid_hdr_prob)) {
        hdrs.push_back(fuz.select(realistic_valid_http2_headers()));
      } else {
        hdrs.push_back(fuz.select(realistic_invalid_http2_headers()));
        invalid = true;
      }
    }
    return hdrs;
  }

  http_body gen_body(fuzzer& fuz) const {
    if (!fuz.rbool(body_prob))
      return {};
    http_body r;
    if (fuz.rbool(body_content_type_prob))
      r.contentType = "text/plain";
    auto str = fuz.rstring(fuz.rint(min_body_sz, max_body_sz));
    r.data.assign(str.begin(), str.end());
    return r;
  }
};

}  // namespace http2::fuzzing
