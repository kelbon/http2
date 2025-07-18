

#include "http2/http2_connection_establishment.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/utils/reusable_buffer.hpp"

namespace http2 {

// TODO nullptr and no throw
dd::task<http2_connection_ptr_t> establish_http2_session_client(http2_connection_ptr_t con,
                                                                http2_client_options options) {
  using enum frame_e;

  constexpr auto H2FHL = FRAME_HEADER_LEN;

  assert(con);
  assert(options.maxReceiveFrameSize <= FRAME_LEN_MAX);
  con->localSettings = settings_t{
      .headerTableSize = options.forceDisableHpack ? 0 : options.hpackDyntabSize,
      .enablePush = false,
      // means nothing, since server do not start streams
      .maxConcurrentStreams = settings_t::MAX_MAX_CONCURRENT_STREAMS,
      .initialStreamWindowSize = MAX_WINDOW_SIZE,
      .maxFrameSize = options.maxReceiveFrameSize,
      .deprecatedPriorityDisabled = true,
  };
  con->decoder = hpack::decoder(con->localSettings.headerTableSize);
  con->streamid = 1;  // client

  io_error_code ec;

  {
    // https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4-4
    // "The client sends the client connection preface as the first application
    // data octets of a connection"

    bytes_t connectionRequest;
    form_connection_initiation(con->localSettings, std::back_inserter(connectionRequest));
    HTTP2_LOG(TRACE, "sending client preface for {}", (void*)con.get());
    (void)co_await con->write(connectionRequest, ec);
    if (ec) {
      HTTP2_LOG(ERROR, "cannot write HTTP/2 client connection preface, err: {}", ec.what());
      throw network_exception("cannot write HTTP/2 client connection preface, err: {}", ec.what());
    }
  }

  // read server connection preface (settings frame)

  // read server preface
  //
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4-6
  // "potentially empty SETTINGS frame (Section 6.5) that MUST be the first
  // frame the server sends in the HTTP/2 connection. The SETTINGS frames
  // received from a peer as part of the connection preface MUST be acknowledged
  // (see Section 6.5.3) after sending the connection preface."

  // So order MUST BE settings (NOT ACK) + settings (ACK)

  byte_t buf[H2FHL];
  co_await con->read(std::span(buf, H2FHL), ec);

  if (ec) {
    HTTP2_LOG(ERROR, "cannot read HTTP/2 server preface, {}", ec.what());
    throw network_exception("cannot read HTTP/2 server preface, {}", ec.what());
  }

  frame_header header = frame_header::parse(buf);

  if (header.type != SETTINGS || header.length > FRAME_LEN_MAX) {
    HTTP2_LOG(ERROR, "first server frame is not settings, its {} with len {}", (int)header.type,
              header.length);
    throw connection_error{};
  }
  if (header.flags & flags::ACK) {
    HTTP2_LOG(ERROR, "invalid server preface with ACK flag");
    throw connection_error{};
  }

  bytes_t bytes(header.length);
  co_await con->read(bytes, ec);
  if (ec) {
    HTTP2_LOG(ERROR, "cannot read accepted settings frame from server");
    throw network_exception(ec);
  }
  settings_frame::parse(header, bytes, server_settings_visitor(con->remoteSettings, /*first frame*/ true));

  // initialize remote settings-based things

  con->encoder = hpack::encoder(con->remoteSettings.headerTableSize);
  con->remoteSettings.maxFrameSize = std::min(con->remoteSettings.maxFrameSize, options.maxSendFrameSize);

  // answer settings ACK "as soon as possible"

  accepted_settings_frame().form(buf);
  HTTP2_LOG(TRACE, "sending settings ACK for {}", (void*)con.get());
  (void)co_await con->write(std::span(buf, H2FHL), ec);
  if (ec) {
    throw network_exception("cannot send accepted settings frame to server, {}", ec.what());
  }

  // SETTINGS frame with ACK flag will be handled later in
  // 'http2_connection::serverSettingsChanged' as regular frame

  HTTP2_LOG(TRACE, "connection successfully established, decoder size: {}",
            con->remoteSettings.headerTableSize);

  co_return con;
}

// TODO nullptr and no throw
dd::task<http2_connection_ptr_t> establish_http2_session_server(http2_connection_ptr_t con,
                                                                http2_server_options options) {
  assert(con);
  io_error_code ec;
  constexpr size_t PREFACE_SZ = std::size(CONNECTION_PREFACE);

  // https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4-4
  // client MUST start its connection with a connection preface
  // client-preface == client magic bytes + settings, which MAY be empty
  // this guararntees, that server can just read and validate preface +
  // settings, then send its own settings and settings ACK

  reusable_buffer buf;
  {  // read preface
    std::span preface = buf.getExactly(PREFACE_SZ);
    co_await con->read(preface, ec);
    if (ec) {
      HTTP2_LOG(ERROR,
                "[SERVER] client session establishment failed: reading "
                "preface, err: {}",
                ec.what());
      throw network_exception(ec);
    }
    if (!std::ranges::equal(preface, std::span(CONNECTION_PREFACE))) {
      HTTP2_LOG(ERROR,
                "[SERVER] client session establishment failed: "
                "incorrect client preface");
      throw protocol_error{};
    }
  }
  frame_header settingsheader;
  {  // read settings frame
    std::span settingsframe = buf.getExactly(FRAME_HEADER_LEN);
    co_await con->read(settingsframe, ec);
    if (ec) {
      HTTP2_LOG(ERROR,
                "[SERVER] client session establishment failed: reading client "
                "settings header, err: {}",
                ec.what());
      throw network_exception(ec);
    }
    settingsheader = frame_header::parse(settingsframe);
  }
  if (settingsheader.type != frame_e::SETTINGS) {
    HTTP2_LOG(ERROR,
              "[SERVER] client session establishment failed: expected SETTINGS "
              "frame, got {}",
              (int)settingsheader.type);
    throw protocol_error{};
  }

  {  // read settings data
    std::span settingsdata = buf.getExactly(settingsheader.length);
    co_await con->read(settingsdata, ec);
    if (ec) {
      HTTP2_LOG(ERROR,
                "[SERVER] client session establishment failed: reading client "
                "settings data, err: {}",
                ec.what());
      throw network_exception(ec);
    }
    settings_frame::parse(settingsheader, settingsdata,
                          client_settings_visitor(con->remoteSettings, /*first frame*/ true));
  }

  // write ACK and my settiings

  con->localSettings.headerTableSize = options.forceDisableHpack ? 0 : options.hpackDyntabSize;
  con->localSettings.maxConcurrentStreams = settings_t::MAX_MAX_CONCURRENT_STREAMS;
  con->localSettings.initialStreamWindowSize = MAX_WINDOW_SIZE;
  con->localSettings.maxFrameSize = FRAME_LEN_MAX;
  con->localSettings.deprecatedPriorityDisabled = true;
  {
    std::vector<byte_t> bytes;
    settings_frame::form(con->localSettings, std::back_inserter(bytes));
    // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5-5
    // "The SETTINGS frames received from a peer as part of the connection
    // preface MUST be acknowledged after sending the connection preface" server
    // preface == settings
    accepted_settings_frame().form(std::back_inserter(bytes));
    co_await con->write(bytes, ec);
    if (ec) {
      HTTP2_LOG(ERROR,
                "[SERVER] client session establishment failed: cannot send ACK "
                "frame to client, err: {}",
                ec.what());
      throw network_exception(ec);
    }
  }
  // до момента ACK настроек от клиента нельзя создавать декодер с локально известными настройками, потому что
  // клиент может начать слать запросы с размером таблицы по умолчанию, что приведёт к ошибке
  // con->decoder = hpack::decoder(con->localSettings.headerTableSize);
  con->encoder = hpack::encoder(con->remoteSettings.headerTableSize);
  // client settings ACK will be handled by server reader

  HTTP2_LOG(TRACE, "[SERVER] client session {} successfully established", (void*)con.get());
  co_return con;
}

}  // namespace http2
