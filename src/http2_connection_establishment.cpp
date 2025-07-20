

#include "http2/http2_connection_establishment.hpp"

#include "http2/http2_connection.hpp"
#include "http2/http2_send_frames.hpp"
#include "http2/utils/reusable_buffer.hpp"

namespace http2 {

static void validate_first_server_frame_header(const frame_header& header) {
  if (header.type != frame_e::SETTINGS || header.length > FRAME_LEN_MAX) {
    HTTP2_LOG(ERROR, "first server frame is not settings, its {} with len {}", (int)header.type,
              header.length);
    throw protocol_error(errc_e::CONNECT_ERROR,
                         std::format("first server frame is not settings, frame: {}", header));
  }
  if (header.flags & flags::ACK) {
    HTTP2_LOG(ERROR, "invalid server preface SETTINGS with ACK flag");
    throw protocol_error(errc_e::CONNECT_ERROR, "invalid server preface SETTINGS with ACK flag");
  }
}

static void validate_client_magic(std::span<byte_t> magic) {
  if (!std::ranges::equal(magic, std::span(CONNECTION_PREFACE))) {
    HTTP2_LOG(ERROR,
              "[SERVER] client session establishment failed: "
              "incorrect client preface");
    throw protocol_error(errc_e::PROTOCOL_ERROR,
                         std::format("invalid client magic, expected: {}, received: {}",
                                     std::string_view((const char*)CONNECTION_PREFACE),
                                     std::string_view((const char*)magic.data(), magic.size())));
  }
}

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
      // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2-2.10.2
      .maxFrameSize = std::max(options.maxReceiveFrameSize, MIN_MAX_FRAME_LEN),
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

  validate_first_server_frame_header(header);

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
  constexpr size_t MAGIC_SZ = std::size(CONNECTION_PREFACE);

  // https://www.rfc-editor.org/rfc/rfc9113.html#section-3.4-4
  // client MUST start its connection with a connection preface
  // client-preface == client magic bytes + settings, which MAY be empty
  // this guararntees, that server can just read and validate preface +
  // settings, then send its own settings and settings ACK

  reusable_buffer buf;
  {  // read client magic
    std::span magic = buf.getExactly(MAGIC_SZ);
    co_await con->read(magic, ec);
    if (ec) {
      HTTP2_LOG(ERROR,
                "[SERVER] client session establishment failed: reading "
                "preface, err: {}",
                ec.what());
      throw network_exception(ec);
    }
    validate_client_magic(magic);
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
  validate_settings_not_ack_frame(settingsheader);

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
  // https://www.rfc-editor.org/rfc/rfc9113.html#section-6.5.2-2.10.2
  con->localSettings.maxFrameSize = std::max(options.maxReceiveFrameSize, MIN_MAX_FRAME_LEN);
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
