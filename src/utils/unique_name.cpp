
#include "http2/utils/unique_name.hpp"

#include <random>

static void fill_str_random(char* b, char* e) {
  constexpr std::string_view chars = "qwertyuiopasdfghjklzxcvbnmQWERTYUIOPASDFGHJKLZXCVBNM1234567890";
  std::minstd_rand gen(std::random_device{}());
  std::uniform_int_distribution<size_t> dist(0, chars.size() - 1);
  while (b != e) {
    *b = chars[dist(gen)];
    ++b;
  }
}

namespace http2 {

unique_name::unique_name() {
  static_assert(LEN >= 8);
  fill_str_random(m_str, m_str + LEN);
  m_str[0] = '[';
  m_str[1] = '-';  // prefix placeholder
  m_str[2] = ']';
  m_str[3] = '[';
  m_str[LEN - 1] = ']';
}

void unique_name::set_prefix(char prefix) noexcept {
  m_str[1] = prefix;
}

}  // namespace http2
