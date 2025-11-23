#include "http2/asio/tudp/tudp_client_socket.hpp"

namespace tudp {

struct tudp_client_socket::impl : tudp_socket_base, std::enable_shared_from_this<tudp_client_socket::impl> {
 private:
  friend struct tudp_client_socket;

  udp::socket sock;
  // tudp impl-reader state
  // invariant: when connected size() >= TUDP_MAX_DATAGRAM_SIZE
  bytes_t tudp_reader_buf;
  // используется только во время коннекта
  move_only_fn_soos<void(const io_error_code&)> connect_callback;
  boost::asio::steady_timer ping_timer;

  virtual void do_async_send(std::span<const byte_t> buf,
                             move_only_fn_soos<void(const io_error_code&, size_t)> foo) override final {
    sock.async_send(boost::asio::const_buffer(buf.data(), buf.size()), std::move(foo));
  }

 public:
  // создаёт незаконнекченный сокет (требуется async_connect)
  explicit impl(boost::asio::io_context& ctx) noexcept : tudp_socket_base(ctx), sock(ctx), ping_timer(ctx) {
  }

  // создаёт сразу законнекченный сокет
  // dcid - connection id существующего на сервере соединения
  // remoteendpoint - адрес сервера
  // scid - опционально свой connection id
  impl(boost::asio::io_context& ctx, boost::asio::ip::udp::endpoint remoteendpoint, cid_t dcid,
       cid_t scid = generate_connection_id())
      : impl(ctx) {
    this->scid = scid;
    this->dcid = dcid;

    sock.connect(remoteendpoint);
  }

  impl(impl&&) = delete;
  void operator=(impl&&) = delete;

  ~impl() {
    shutdown();
  }

  boost::asio::ip::udp::endpoint local_endpoint() const noexcept {
    return sock.local_endpoint();
  }

  boost::asio::ip::udp::endpoint remote_endpoint() const noexcept {
    return sock.remote_endpoint();
  }

  using tudp_socket_base::async_receive_unordered;
  using tudp_socket_base::async_send_unordered;
  using tudp_socket_base::try_receive_unordered;

  using tudp_socket_base::async_read_some;
  using tudp_socket_base::async_write_some;
  using tudp_socket_base::set_options;
  using tudp_socket_base::try_read;

  void shutdown() {
    ping_timer.cancel();
    if (sock.is_open()) {
      sock.cancel();
      sock.close();
    }
    tudp_socket_base::shutdown();
  }

  void async_connect(boost::asio::ip::udp::endpoint e, move_only_fn_soos<void(const io_error_code&)> cb) {
    assert(dcid == 0 && !connect_callback.has_value());
    if (scid == 0)
      scid = generate_connection_id();  // client connection id
    sent_packet_nmb = 0;                // for case when connect after shutdown
    readen_packet_nmb = 0;

    assert(sent.empty());
    assert(received.empty());
    assert(!reader);

    start_write_loop();
    io_error_code ec;
    ec = sock.connect(e, ec);
    if (ec) {
      boost::asio::post(get_executor(), std::bind_front(std::move(cb), ec));
      return;
    }
    // start read loop before sending connect request, so we will not lose any data from other
    // endpoint (it will resend it anyway, but its wasting time)
    start_read_loop();
    ping_timer.expires_after(options.ping_period);
    ping_timer.async_wait(ping_loop_callback{this});

    connect_callback = std::move(cb);
    // send our scid to server
    // `dcid` заполняется при получении ACK на этот пакет в `receive_packet`
    send_packet(scid, 0, TUDP_CONNECT_PACKET_NMB, {});
  }

  void cancel_connect() {
    if (connect_callback) {
      boost::asio::post(get_executor(), [cb = std::move(connect_callback)]() mutable {
        return cb(boost::asio::error::operation_aborted);
      });
    }
  }

 private:
  void send_ping() {
    auto buf = form_ping_datagram(scid, dcid);
    std::shared_ptr ptr = std::make_shared<decltype(buf)>(buf);
    boost::asio::const_buffer asiobuf(ptr->data(), ptr->size());
    sock.async_send(asiobuf, [_ = std::move(ptr)](const io_error_code&, size_t) {});
  }

  void start_read_loop() {
    tudp_reader_buf.resize(TUDP_MAX_DATAGRAM_SIZE);
    sock.async_receive(boost::asio::mutable_buffer(tudp_reader_buf.data(), tudp_reader_buf.size()),
                       read_loop_callback{shared_from_this()});
  }

  // вызывается когда получен ACK на конкретный пакет
  void receive_ack(uint64_t nmb) {
    if (nmb > sent_packet_nmb && nmb != TUDP_CONNECT_PACKET_NMB) [[unlikely]] {
      HTTP2_DO_LOG(WARN, "ack packet which is not sent by this connection");
      return;
    }
    if (auto it = sent.find(nmb); it != sent.end())
      sent.erase(it);
  }

  void handle_unparsable_packet_client(std::span<const byte_t> data) noexcept {
    // noop
  }

  void receive_packet(std::span<const byte_t> data) {
    auto on_data = [&](tudp_data_datagram const& dg) {
      // filter duplicates
      if (!already_received_packet_nmbs.has_point(dg.packet_nmb)) [[likely]] {
        already_received_packet_nmbs.add_point(dg.packet_nmb);
        received.try_emplace(dg.packet_nmb, bytes_t(dg.payload.begin(), dg.payload.end()));
        if (reader) {
          size_t readen = try_read(reader_wants);
          if (readen > 0) {
            notify_reader(get_executor(), std::move(reader), readen);
          }
        }
      }
      // отсылаем ACK в любом случае, даже если дубликат. Возможно был потерян сам ACK
      send_ack(dg.packet_nmb);
      return true;
    };

    auto on_ack = [&](tudp_ack_datagram const& dg) {
      if (dcid == 0) [[unlikely]] {
        // connecting, dg.payload должна содержать один ACK на TUDP_CONNECT_PACKET_NMB
        dcid = dg.scid;
        if (connect_callback) {
          boost::asio::post(get_executor(),
                            [cb = std::move(connect_callback)]() mutable { cb(io_error_code{}); });
        }
      } else if (dg.dcid != scid) [[unlikely]]
        return false;
      std::span payload = dg.payload;
      while (!payload.empty()) {
        receive_ack(take_uint64(payload));
      }
      return true;
    };

    auto on_unordered_data = [&](tudp_unordered_data_datagram const& dg) {
      return receive_unordered_data_dg(dg);
    };

    auto on_ping = [&](tudp_ping_datagram) {
      auto buf = form_pong_datagram(scid, dcid);
      std::shared_ptr ptr = std::make_shared<decltype(buf)>(buf);
      boost::asio::const_buffer asiobuf(ptr->data(), ptr->size());
      sock.async_send(asiobuf, [_ = std::move(ptr)](const io_error_code&, size_t) {});
      return true;
    };
    auto on_pong = [](auto&&) { return true; };

    if (!visit_packet(data, on_data, on_ack, on_unordered_data, on_ping, on_pong))
      handle_unparsable_packet_client(data);
  }

  struct ping_loop_callback {
    impl* self = nullptr;

    void operator()(const io_error_code& ec) {
      if (ec)
        return;

      self->send_ping();

      self->ping_timer.expires_after(self->options.ping_period);
      self->ping_timer.async_wait(*this);
    }
  };

  struct read_loop_callback {
    // в Boost asio только для udp сокетов .cancel отменяет не сразу, а в позже в следующих циклах ioctx
    // так что без шареда возможна ситуация, когда сокет уже удалён, а операция вызвана без ошибки
    std::shared_ptr<impl> self = nullptr;

    void operator()(const io_error_code& ec, size_t readen) {
      if (ec) {
        if (ec != boost::asio::error::operation_aborted) [[unlikely]]
          HTTP2_DO_LOG(WARN, "read loop ends with error: {}", ec.message());
        return;
      }
      if (self.use_count() == 1)
        return;
      self->receive_packet(std::span<const byte_t>(self->tudp_reader_buf.data(), readen));
      // loop
      self->sock.async_receive(
          boost::asio::mutable_buffer(self->tudp_reader_buf.data(), self->tudp_reader_buf.size()),
          std::move(*this));
    }
  };
};

tudp_client_socket::tudp_client_socket(boost::asio::io_context& ctx) : pimpl(std::make_shared<impl>(ctx)) {
}

tudp_client_socket::tudp_client_socket(boost::asio::io_context& ctx,
                                       boost::asio::ip::udp::endpoint remoteendpoint, cid_t dcid, cid_t scid)
    : pimpl(std::make_shared<impl>(ctx, std::move(remoteendpoint), dcid, scid)) {
}

void tudp_client_socket::async_connect(boost::asio::ip::udp::endpoint e,
                                       move_only_fn_soos<void(const io_error_code&)> cb) {
  pimpl->async_connect(std::move(e), std::move(cb));
}

void tudp_client_socket::cancel_connect() {
  if (pimpl)
    pimpl->cancel_connect();
}

std::optional<bytes_t> tudp_client_socket::try_receive_unordered() {
  return pimpl->try_receive_unordered();
}

void tudp_client_socket::async_receive_unordered(move_only_fn_soos<void(std::span<const byte_t>)> cb) {
  return pimpl->async_receive_unordered(std::move(cb));
}

void tudp_client_socket::async_send_unordered(std::span<const byte_t> packet,
                                              move_only_fn_soos<void(const io_error_code&)> cb) {
  return pimpl->async_send_unordered(packet, std::move(cb));
}

size_t tudp_client_socket::try_read(std::span<byte_t> buf) noexcept {
  return pimpl->try_read(buf);
}

size_t tudp_client_socket::try_write(std::span<const byte_t> buf, io_error_code& ec) {
  return pimpl->try_write(buf, ec);
}

void tudp_client_socket::async_read_some(boost::asio::mutable_buffer buf,
                                         move_only_fn_soos<void(const io_error_code&, size_t)> cb) {
  return pimpl->async_read_some(buf, std::move(cb));
}

void tudp_client_socket::async_write_some(boost::asio::const_buffer buf,
                                          move_only_fn_soos<void(const io_error_code&, size_t)> cb) {
  return pimpl->async_write_some(buf, std::move(cb));
}

void tudp_client_socket::shutdown() noexcept {
  pimpl->shutdown();
}

boost::asio::ip::udp::endpoint tudp_client_socket::local_endpoint() const noexcept {
  return pimpl->local_endpoint();
}

boost::asio::ip::udp::endpoint tudp_client_socket::remote_endpoint() const noexcept {
  return pimpl->remote_endpoint();
}

cid_t tudp_client_socket::get_scid() const noexcept {
  return pimpl ? pimpl->scid : 0;
}

cid_t tudp_client_socket::get_dcid() const noexcept {
  return pimpl ? pimpl->dcid : 0;
}

void tudp_client_socket::set_options(tudp_socket_options opts) {
  pimpl->set_options(opts);
}

const tudp_client_socket::executor_type& tudp_client_socket::get_executor() noexcept {
  return pimpl->get_executor();
}

}  // namespace tudp
