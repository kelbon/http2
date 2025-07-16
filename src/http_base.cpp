

#include "http2/http2_client.hpp"
#include "http2/http2_errors.hpp"

#include <strswitch/strswitch.hpp>

// windows)))
#undef DELETE

namespace http2 {

[[noreturn]] static void throw_bad_status(int status) {
  assert(status < 0);
  using enum reqerr_e::values_e;
  switch (reqerr_e::values_e(status)) {
    case DONE:
      unreachable();
    case TIMEOUT:
      throw timeout_exception{};
    case NETWORK_ERR:
      throw network_exception{""};
    case PROTOCOL_ERR:
      throw protocol_error{};
    case CANCELLED:
      throw std::runtime_error("HTTP client: request was canceled");
    case SERVER_CANCELLED_REQUEST:
      throw std::runtime_error("HTTP client: request was canceled by server");
    default:
    case UNKNOWN_ERR:
      throw std::runtime_error("HTTP client unknown error happens");
  }
}

dd::task<http_response> http2_client::sendRequest(http_request request, deadline_t deadline) {
  http_response rsp;
  auto onHeader = [&](std::string_view name, std::string_view value) {
    rsp.headers.emplace_back(std::string(name), std::string(value));
  };
  auto onDataPart = [&](std::span<byte_t const> bytes, bool /*lastPart*/) {
    rsp.body.insert(rsp.body.end(), bytes.begin(), bytes.end());
  };
  rsp.status = co_await sendRequest(&onHeader, &onDataPart, std::move(request), deadline);
  if (rsp.status < 0) {
    throw_bad_status(rsp.status);
  }

  co_return std::move(rsp);
}

std::string_view e2str(reqerr_e::values_e e) noexcept {
  using enum reqerr_e::values_e;
  switch (e) {
    case DONE:
      return "done";
    case CANCELLED:
      return "cancelled";
    case TIMEOUT:
      return "timeout";
    case NETWORK_ERR:
      return "network_err";
    case PROTOCOL_ERR:
      return "protocol_err";
    case SERVER_CANCELLED_REQUEST:
      return "server_cancelled_request";
    case UNKNOWN_ERR:
      return "unknown_err";
  }
  unreachable();
}

std::string_view e2str(http_method_e e) noexcept {
  using enum http_method_e;
  switch (e) {
    case GET:
      return "GET";
    case POST:
      return "POST";
    case PUT:
      return "PUT";
    case DELETE:
      return "DELETE";
    case PATCH:
      return "PATCH";
    case OPTIONS:
      return "OPTIONS";
    case HEAD:
      return "HEAD";
    case CONNECT:
      return "CONNECT";
    case TRACE:
      return "TRACE";
    case UNKNOWN:
      return "UNKNOWN";
    default:
      unreachable();  // error
  }
}

void enum_from_string(std::string_view str, http_method_e& e) noexcept {
  using enum http_method_e;
  e = ss::string_switch<http_method_e>(str)
          .case_("GET", GET)
          .case_("POST", POST)
          .case_("PUT", PUT)
          .case_("DELETE", DELETE)
          .case_("PATCH", PATCH)
          .case_("HEAD", HEAD)
          .case_("CONNECT", CONNECT)
          .case_("OPTIONS", OPTIONS)
          .case_("TRACE", TRACE)
          .orDefault(UNKNOWN);
}

std::string_view e2str(scheme_e e) noexcept {
  switch (e) {
    case scheme_e::HTTP:
      return "http";
    case scheme_e::HTTPS:
      return "https";
    case scheme_e::UNKNOWN:
      return "UNKNOWN";
    default:
      unreachable();  // error
  }
}

void enum_from_string(std::string_view str, scheme_e& s) noexcept {
  using enum scheme_e;
  s = ss::string_switch<scheme_e>(str).case_("http", HTTP).case_("https", HTTPS).orDefault(UNKNOWN);
}

}  // namespace http2
