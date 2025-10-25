
#include "request_template.hpp"

namespace http2::fuzzing {

std::span<const std::string_view> realistic_valid_pathes() {
  static std::string_view paths[] = {
      // API endpoints
      "/api/v1/users",
      "/api/v2/posts/123",
      "/graphql",
      "/rest/products",
      "/jsonrpc",
      // Authentication
      "/auth/login",
      "/auth/register",
      "/user/profile",
      "/user/settings/notifications",
      "/admin/dashboard",
      // E-commerce
      "/shop/products",
      "/category/electronics",
      "/product/iphone-15/reviews",
      "/cart",
      "/checkout/shipping",
      // Static assets
      "/static/css/main.css",
      "/js/app.min.js",
      "/images/logo.png",
      "/fonts/roboto.woff2",
      "/favicon.ico",
      // Blog/content
      "/blog/2024/10/awesome-post",
      "/tag/technology",
      "/rss.xml",
      "/sitemap.txt",
      "/search?q=http2",
      // Files/media
      "/downloads/report.pdf",
      "/videos/stream.mp4",
      "/audio/podcast.mp3",
      "/uploads/2024/10/file.zip",
      "/thumbnails/small/image.jpg",
      // System/utility
      "/healthcheck",
      "/metrics",
      "/status",
      "/version",
      "/robots.txt",
      // Legacy/redirects
      "/old-page",
      "/legacy/api",
      // Parameters/dynamic
      "/search?query=hello&sort=desc",
      "/user/456/friends?limit=20",
      "/filter?price[min]=100&price[max]=500",
      "/api/data?format=json",
      "/lang/en/page",
      // HTTP/2 specific
      "/h2-push-resource.js",
      "/prioritized/image.jpg",
      // Edge cases
      "/../etc/passwd",
      "/",
  };

  return paths;
}

std::span<const std::string_view> realistic_valid_authorities() {
  static std::string_view arr[] = {
      "example.com",      "api.example.com",       "example.com:443",        "127.0.0.1",
      "[2001:db8::1]",    "user:pass@example.com", "xn--e1aybc.example.com", "localhost",
      "example.com:8080", "service.internal",
  };
  return arr;
}

std::span<const std::string_view> realistic_invalid_authorities() {
  static std::string_view arr[] = {
      "example.com:99999",
      "example..com",
      "user@:pass@example.com",
      "[2001:db8::1",
  };
  return arr;
}

std::span<const http_header_t> realistic_valid_http2_headers() {
  static http_header_t hdrs[] = {
      {"x-biz-trace", "TraceEnabled"},
      {"client-request-id", "72AA1CB9-62C1-4CF9-A4A3-3AD1125F36B7"},
      {"x-internal-session", "Session=abc123"},
      {"x-service-region", "EU-WEST-3"},
      {"x-env-flag", "ProductionMode"},
      {"x-billing-token", "Token XYZ_987"},
      {"x-experiment-group", "GroupBeta"},
      {"client-app-version", "5.3.1"},
      {"x-customer-ref", "Cust-1123"},
      {"app-request-marker", "marker/459d/start"},
      {"x-business-intent", "CheckPricingFlow"},
      {"user-segment", "Enterprise"},
      {"x-partner-code", "partner-code=998"},
      {"x-device-model", "iPhone16,2"},
      {"internal-correlation", "CorrID=239FD"},
      {"x-compliance-mode", "StrictModeOn"},
      {"app-metric-source", "MobileApp"},
      {"x-rollout-bucket", "bucket_23"},
      {"client-tier", "Gold"},
      {"x-audit-token", "Audit:0011"},
      {"x-workflow-id", "workflow/3821"},
      {"service-locale", "en_CA"},
      {"x-app-feature", "SmartSuggest"},
      {"x-tracking-label", "TrackLabel-8327"},
      {"tenant-id", "Tenant_94"},
      {"app-scenario", "Bulk Upload"},
      {"x-biz-priority", "Normal"},
      {"client-profile", "ProfileXYZ"},
      {"region-override", "us-east-edge"},
      {"x-sandbox-mode", "ON"},
      {"biz-tag", "Influencer_Onboarding"},
      {"x-api-variant", "v2-experimental"},
      {"partner-account-id", "ACCT-4451"},
      {"x-bundle-name", "StarterPack"},
      {"x-payload-type", "Analytics"},
      {"client-scope", "ReadOnly"},
      {"x-interaction-mode", "Background"},
      {"x-module-origin", "checkout-widget"},
      {"request-channel", "InApp"},
      {"x-instance-tag", "inst-381"},
      {"user-group-code", "UGC_77"},
      {"x-cluster-key", "CLUSTER=eu-central"},
      {"source-application", "CRM-Frontend"},
      {"x-context-handle", "ctx-aaaa-bbbb"},
      {"feature-toggle", "Enabled"},
      {"x-debug-flow", "TraceAll"},
      {"x-quota-group", "Group_A"},
      {"x-route-intent", "InternalOnly"},
      {"x-data-plan", "Premium"},
      {"x-access-scope", "biz.unit.read"},
  };
  return hdrs;
}

std::span<const http_header_t> realistic_invalid_http2_headers() {
  static http_header_t hdrs[] = {
      {"header name", "value"},     // Пробел в имени заголовка недопустим (RFC 7230, Section 3.2)
      {"header@name", "value"},     // Специальный символ '@' в имени заголовка недопустим
      {"content-length", "-1"},     // Отрицательное значение для content-length недопустимо
      {"te", "chunked"},            // TE не может содержать 'chunked' в HTTP/2 (RFC 9113, Section 8.1)
      {"connection", "close"},      // Заголовок 'connection' запрещен в HTTP/2 (RFC 9113, Section 8.1)
      {"header:name", "value"},     // Двоеточие в имени заголовка недопустимо
      {"", "value"},                // Пустое имя заголовка недопустимо
      {"trailer", "value"},         // 'trailer' не может использоваться как обычный заголовок в HTTP/2
      {"host", "example.com:80"},   // 'host' не должен содержать порт в HTTP/2 (используется :authority)
      {"invalid_header!", "value"}  // Восклицательный знак в имени заголовка недопустим
  };
  return hdrs;
}

}  // namespace http2::fuzzing
