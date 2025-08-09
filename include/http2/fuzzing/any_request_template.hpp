#pragma once

#include "http2/http2_client.hpp"
#include "http2/fuzzing/request_template.hpp"

#include <anyany/anyany.hpp>

namespace http2::fuzzing {

struct make_req_m {
  static hreq do_invoke(const auto& self, fuzzer& fuz) {
    return self.generate_request(fuz);
  }

  template <typename CRTP>
  struct plugin {
    hreq generate_request(fuzzer& fuz) const {
      const CRTP& self = static_cast<CRTP const&>(*this);
      return aa::invoke<make_req_m>(self, fuz);
    }
  };
};

using any_reqtem = aa::any_with<make_req_m, aa::copy>;

struct weighted_reqtems {
 private:
  struct weighted_reqtem {
    any_reqtem reqtem;
    int weight = 1;
  };
  std::vector<weighted_reqtem> reqtems;
  mutable std::discrete_distribution<int> dist;

 public:
  void add_reqtem(any_reqtem reqtem, int weight = 1) {
    reqtems.emplace_back(std::move(reqtem), weight);
    auto weights = std::views::transform(reqtems, &weighted_reqtem::weight);
    dist = std::discrete_distribution<int>(weights.begin(), weights.end());
  }

  // not safe to invoke in multi thread
  any_reqtem const& select_reqtem(fuzzer& fuz) const {
    return reqtems[dist(fuz.g)].reqtem;
  }

  hreq generate_request(fuzzer& fuz) const {
    return select_reqtem(fuz).generate_request(fuz);
  }
};

// sends always same request (not connect, not streaming)
struct clone_reqtem {
  hreq req;

  hreq generate_request(fuzzer&) const {
    return req;
  }
};

}  // namespace http2::fuzzing
