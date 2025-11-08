
#include "test_connection.hpp"
#include <moko3/moko3.hpp>

using namespace http2;

static ssl_context_ptr test_ssl_ctx() {
  return make_ssl_context_for_server(HTTP2_TLS_DIR "/test_server.crt", HTTP2_TLS_DIR "/test_server.key");
}

SERVER_TEST("server bytes limit") {
  constexpr size_t LIMIT = 1000;
  // set options before client connection
  server.get_options().limit_requests_memory_usage_bytes = LIMIT;
  bool tls = GENERATE(true, false);
  if (tls)
    server.set_ssl_context(test_ssl_ctx());
  auto client = co_await fake_client_connection(ioctx, addr, tls);
  co_await emulate_client_connection(client);

  SECTION("regular request") {
    std::vector<header> hdrs{
        {":method", "GET"},
        {":path", "/README.md"},
        {":scheme", "http"},
        {":authority", addr.address().to_string()},
    };
    size_t sum = 0;
    for (auto& [n, v, x] : hdrs) {
      // name.size() not counted internally as used bytes (only for pseudoheader)
      sum += v.size();
    }
    auto hdrs_bytes = client.encode_headers(hdrs);

    // size == LIMIT

    co_await client.sendRawHdr(1, hdrs_bytes, /*end_stream=*/false);
    std::string data_frame(LIMIT - sum, char(1));
    co_await client.sendData(1, data_frame, /*end_stream=*/true);
    auto rsp = co_await client.receiveRsp();
    REQUIRE(rsp.endStream);
    REQUIRE(std::ranges::equal(rsp.body, data_frame));
    REQUIRE(rsp.headers.size() == 1 && rsp.headers[0] == header{":status", "200"});

    // size == LIMIT + 1

    hdrs_bytes = client.encode_headers(hdrs);
    co_await client.sendRawHdr(3, hdrs_bytes, /*end_stream=*/false);
    data_frame.push_back(char(1));
    co_await client.sendData(3, data_frame, /*end_stream=*/true);
    co_await client.receiveRstStream(3, errc_e::ENHANCE_YOUR_CALM);
  }

  SECTION("overflow in continuations") {
    std::vector<header> hdrs{
        {":method", "GET"},
        {":path", "/README.md"},
        {":scheme", "http"},
        {"big_header", std::string(LIMIT, char(1))},
        {"some_header", "some_value"},
    };

    auto hdrs_bytes = client.encode_headers(hdrs);

    co_await client.sendRawHdr(1, hdrs_bytes, /*end_stream=*/false);
    co_await client.receiveRstStream(1, errc_e::ENHANCE_YOUR_CALM);

    // checks that dynamic table in correct state after skipping request
    hdrs[3].value = "";
    hdrs_bytes = client.encode_headers(hdrs);

    co_await client.sendRawHdr(3, hdrs_bytes, /*end_stream=*/true);
    auto rsp = co_await client.receiveRsp();
    REQUIRE(rsp.endStream);
    REQUIRE(rsp.body.empty());
    REQUIRE(rsp.headers.size() == 3 && rsp.headers[0] == header{":status", "200"} &&
            rsp.headers[1] == header{"big_header", ""} &&
            rsp.headers[2] == header{"some_header", "some_value"});
  }

  SECTION("overflow in trailers") {
    std::vector<header> hdrs{
        {":method", "GET"},
        {":path", "/README.md"},
        {":scheme", "http"},
        {":authority", addr.address().to_string()},
    };

    co_await client.sendReq(1, hdrs, http_body_bytes(10, byte_t(1)), /*endstream=*/false);

    std::vector<header> trailers{
        {"big_header", std::string(LIMIT, char(1))},
    };
    co_await client.sendHeaders(1, trailers, /*endstream=*/true);

    co_await client.receiveRstStream(1, errc_e::ENHANCE_YOUR_CALM);
  }

  SECTION("try big header spam") {
    // cache one big header and then encode it into 1 byte many times trying to ddos server
    std::vector<header> hdrs{
        {":method", "GET"},
        {":path", "/README.md"},
        {":scheme", "http"},
        {"big_header", std::string(100, char(1))},
    };
    h2connection_ptr con = client.get_inner_connection();
    bytes_t hdrs_bytes;
    for (auto& h : hdrs) {
      con->encoder.encode_header_and_cache(h.name, h.value, std::back_inserter(hdrs_bytes));
    }
    hpack::find_result_t r = con->encoder.dyntab.find(hdrs.back().name, hdrs.back().value);
    REQUIRE(r.value_indexed && r.header_name_index != 0);
    // 50 * ("big_header".len() + 100)) > LIMIT
    for (int i = 0; i < 50; ++i)
      con->encoder.encode_header_fully_indexed(r.header_name_index, std::back_inserter(hdrs_bytes));

    co_await client.sendRawHdr(1, hdrs_bytes);
    co_await client.receiveRstStream(1, errc_e::ENHANCE_YOUR_CALM);
  }
}

SERVER_TEST("server sessions limit") {
  server.get_options().limit_clients_count = 0;
  bool tls = GENERATE(true, false);
  if (tls)
    server.set_ssl_context(test_ssl_ctx());
  auto client = co_await fake_client_connection(ioctx, addr, tls);
  try {
    co_await emulate_client_connection(client);
  } catch (...) {
    co_return;
  }
  // server must drop connection immediately after connect
  co_await client.waitConnectionDropped(deadline_after(1s));
}

SERVER_TEST("server CONTINUATION limit") {
  constexpr size_t LIMIT = 100;
  server.get_options().max_continuation_len_bytes = LIMIT;
  bool tls = GENERATE(true, false);
  if (tls)
    server.set_ssl_context(test_ssl_ctx());
  auto client = co_await fake_client_connection(ioctx, addr, tls);
  co_await emulate_client_connection(client);
  constexpr size_t FIRST_CHUNK = 10;
  static_assert(FIRST_CHUNK < LIMIT);
  bytes_t headers(FIRST_CHUNK, 'a');  // data makes no sense here

  // send HEADERS without END_HEADERS
  h2frame f;
  f.hdr.length = uint32_t(headers.size());
  f.hdr.type = frame_e::HEADERS;
  f.hdr.flags = flags::EMPTY_FLAGS;  // no END_HEADERS
  f.hdr.streamId = 1;
  f.hdr.flags |= flags::END_STREAM;
  f.data.assign(headers.begin(), headers.end());
  co_await client.sendFrame(std::move(f));

  SECTION("overlimit") {
    headers.resize(LIMIT, 'a');
    co_await client.sendRawContinuation(1, headers, /*end_headers=*/true);
    co_await client.receiveRstStream(1, errc_e::REFUSED_STREAM);
  }
  SECTION("headers.size() == LIMIT") {
    headers.resize(LIMIT - FIRST_CHUNK, 'a');
    co_await client.sendRawContinuation(1, headers, /*end_headers=*/true);
    // invalid headers block, but request accepted and parsed
    co_await client.receiveGoAway(1, errc_e::COMPRESSION_ERROR, ping_e::RESPONSE);
  }
}

REGISTER_TEST_LISTENER(moko3::gtest_listener);
MOKO3_MAIN;
