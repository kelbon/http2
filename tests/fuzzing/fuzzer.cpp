#include "fuzzer.hpp"

#include <algorithm>

namespace hidi::fuzzing {

std::string exclude_symbols(std::string_view included, std::string_view excluded) {
  std::string r(included);
  std::erase_if(r, [&](char c) { return excluded.find(c) != excluded.npos; });
  return r;
}

std::string unique_symbols(std::string_view syms) {
  std::string r(syms);
  std::ranges::sort(r);
  auto removed = std::ranges::unique(r);
  r.erase(removed.begin(), removed.end());
  return r;
}

}  // namespace hidi::fuzzing
