#pragma once

#include <format>
#include <string_view>

namespace http2 {

// unique name for logging (server / server session/ client / client connection)
// allows grep by name and find all logs associated with entity
struct unique_name {
 private:
  static constexpr size_t LEN = 16;

  char m_str[LEN];

 public:
  unique_name();

  // prefix denotes the belonging of the entity, e.g. client or connection
  void set_prefix(char prefix) noexcept;

  std::string_view str() const noexcept {
    return std::string_view(+m_str, LEN);
  }
};

constexpr inline char CLIENT_PREFIX = 'C';
constexpr inline char SERVER_PREFIX = 'S';
constexpr inline char CLIENT_CONNECTION_PREFIX = 'c';
constexpr inline char SERVER_SESSION_PREFIX = 's';

}  // namespace http2

namespace std {

template <>
struct formatter<::http2::unique_name> : formatter<string_view> {
  auto format(::http2::unique_name const& n, auto& ctx) const -> decltype(ctx.out()) {
    return formatter<string_view>::format(n.str(), ctx);
  }
};

}  // namespace std
