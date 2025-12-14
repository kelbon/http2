#include "http2/asio/tudp/tudp_acceptor.hpp"

#include <deque>

#include "http2/asio/asio_executor.hpp"
#include "http2/asio/tudp/stun.hpp"
#include "http2/asio/tudp/tudp_server_socket.hpp"
#include "http2/utils/timer.hpp"

namespace tudp {

struct inactive_detector {
 private:
  size_t changes = 0;
  http2::timer_t periodic;
  http2::timer_t deadline_timer;
  duration_t deadline_timeout;

 public:
  inactive_detector(boost::asio::io_context& ctx) : periodic(ctx), deadline_timer(ctx) {
  }

  inactive_detector(inactive_detector&&) = delete;
  void operator=(inactive_detector&&) = delete;

  void start(move_only_fn_soos<void()> on_inactive_detected, duration_t deadline_timeout_) {
    assert(on_inactive_detected);
    deadline_timeout = deadline_timeout_;
    periodic.arm_periodic(std::chrono::milliseconds(100));
    periodic.set_callback([activity = 0, this]() mutable {
      if (activity != changes) {
        changes = activity;
        return;
      }
      if (!deadline_timer.armed()) {
        deadline_timer.arm(deadline_timeout);
      }
    });
    deadline_timer.set_callback(std::move(on_inactive_detected));
  }

  void stop() noexcept {
    periodic.cancel();
    deadline_timer.cancel();
    periodic.set_callback({});
    deadline_timer.set_callback({});
  }

  void activity_happen() noexcept {
    ++changes;
    if (deadline_timer.armed())
      deadline_timer.cancel();
  }
};

struct tudp_server_socket::impl : tudp_socket_base {
 private:
  friend tudp_acceptor::impl;

  // обновляется acceptor'ом, когда новые данные приходят с нового адреса
  boost::asio::ip::udp::endpoint destination;
  // назначается в acceptor, 0 после shutdown и до async_accept
  std::shared_ptr<tudp_acceptor::impl> creator = nullptr;
  inactive_detector detector;

  // impl в tudp_acceptor.cpp
  boost::asio::ip::udp::socket& owner_sock() noexcept;
  const boost::asio::ip::udp::socket& owner_sock() const noexcept;

  virtual void do_async_send(std::span<const byte_t> buf,
                             move_only_fn_soos<void(const io_error_code&, size_t)> foo) override final {
    owner_sock().async_send_to(boost::asio::const_buffer(buf.data(), buf.size()), destination,
                               std::move(foo));
  }

 public:
  std::shared_ptr<impl> shared_from_this() noexcept {
    return std::static_pointer_cast<impl>(tudp_socket_base::shared_from_this());
  }

  boost::asio::ip::udp::endpoint local_endpoint() const noexcept {
    return owner_sock().local_endpoint();
  }

  boost::asio::ip::udp::endpoint remote_endpoint() const noexcept {
    return destination;
  }

  // создаёт незаконнекченный сокет (требуется async_connect)
  explicit impl(boost::asio::io_context& ctx) noexcept : tudp_socket_base(ctx), detector(ctx) {
  }

  impl(impl&&) = delete;
  void operator=(impl&&) = delete;

  ~impl() {
    shutdown();
  }

  using tudp_socket_base::async_read_some;
  using tudp_socket_base::async_write_some;
  using tudp_socket_base::set_options;
  // impl в tudp_acceptor.cpp
  void shutdown() noexcept;
  using tudp_socket_base::try_read;

 private:
  // вызывается когда получен ACK на конкретный пакет
  void receive_ack(uint64_t nmb, cid_t scid) {
    if (nmb > sent_packet_nmb && nmb != TUDP_CONNECT_PACKET_NMB) [[unlikely]] {
      HTTP2_DO_LOG(WARN, "ack packet which is not sent by this connection");
      return;
    }
    if (auto it = sent.find(nmb); it != sent.end()) {
      sent.erase(it);
    }
  }

  void receive_data_dg(tudp_data_datagram const& dg) noexcept {
    detector.activity_happen();
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
    // ACK отсылает acceptor
  }

  void receive_ack_dg(tudp_ack_datagram const& dg) noexcept {
    assert(dg.dcid == scid);
    detector.activity_happen();
    auto payload = dg.payload;
    while (!payload.empty())
      receive_ack(take_uint64(payload), dg.scid);
  }
};

tudp_server_socket::tudp_server_socket(boost::asio::io_context& ctx) : pimpl(std::make_shared<impl>(ctx)) {
}

[[nodiscard]] size_t tudp_server_socket::try_read(std::span<byte_t> buf) noexcept {
  return pimpl->try_read(buf);
}

size_t tudp_server_socket::try_write(std::span<const byte_t> buf, io_error_code& ec) {
  return pimpl->try_write(buf, ec);
}

void tudp_server_socket::async_read_some(boost::asio::mutable_buffer buf,
                                         move_only_fn_soos<void(const io_error_code&, size_t)> cb) {
  pimpl->async_read_some(buf, std::move(cb));
}

void tudp_server_socket::async_write_some(boost::asio::const_buffer buf,
                                          move_only_fn_soos<void(const io_error_code&, size_t)> cb) {
  pimpl->async_write_some(buf, std::move(cb));
}

void tudp_server_socket::shutdown() noexcept {
  pimpl->shutdown();
}

void tudp_server_socket::set_options(tudp_socket_options o) {
  pimpl->set_options(o);
}

cid_t tudp_server_socket::get_scid() const noexcept {
  return pimpl ? pimpl->get_scid() : 0;
}

cid_t tudp_server_socket::get_dcid() const noexcept {
  return pimpl ? pimpl->get_dcid() : 0;
}

boost::asio::ip::udp::endpoint tudp_server_socket::local_endpoint() const noexcept {
  return pimpl->local_endpoint();
}

boost::asio::ip::udp::endpoint tudp_server_socket::remote_endpoint() const noexcept {
  return pimpl->remote_endpoint();
}

const tudp_server_socket::executor_type& tudp_server_socket::get_executor() noexcept {
  return pimpl->get_executor();
}

struct tudp_acceptor::impl : std::enable_shared_from_this<tudp_acceptor::impl> {
 private:
  friend tudp_server_socket::impl;
  /*
  - делает bind на udpsock и запускает асинхронный цикл получения udp датаграм на udpsock
  - при получении пакетов пытается их распарсить и те что получилось маршрутизирует между открытыми
  соединениями по dcid
  */

  // invariant: во время работы size() >= TUDP_MAX_DATAGRAM_SIZE
  bytes_t buf;
  boost::asio::io_context& ioctx;
  // tudp_server_socket::impl держит shared_ptr на *this и каждый из них нельзя мувать
  // в деструкторе убирает себя отсюда
  tudp_server_socket::impl* m_sock = nullptr;
  // назначено только во время accept.
  // либо вызовется с успехом, либо отменится через cancel_accept
  std::coroutine_handle<> accepter = nullptr;

  // этот сокет нужен для bind и прослушивания адреса
  udp::socket udpsock;
  bool listens = false;
  // эндпоинт последнего полученного пакета
  udp::endpoint sender_ep;
  udp::endpoint my_endpoint;
  std::optional<udp::endpoint> global_endpoint;
  move_only_fn_soos<void(std::optional<udp::endpoint>)> stun_response_waiter;
  // не меняется после создания, генерируется единожды
  stun_tid_t stun_tid = make_random_stun_tid();

  // полезная дока (там есть в т.ч. рекомендуемые промежутки между пингами)
  // Network Address Translation (NAT) Behavioral Requirements for Unicast UDP
  //  (https://www.rfc-editor.org/rfc/rfc4787.html#section-4.3)

 public:
  impl(boost::asio::io_context& ctx, udp::endpoint ep, bool reuse_address)
      : ioctx(ctx), udpsock(ctx), my_endpoint(ep) {
    udpsock.open(ep.protocol());

    if (reuse_address)
      udpsock.set_option(boost::asio::socket_base::reuse_address(reuse_address));
    udpsock.bind(ep);
  }

  impl(impl&&) = delete;
  void operator=(impl&&) = delete;

  ~impl() {
    close();
  }

  boost::asio::ip::udp::endpoint local_endpoint() const noexcept {
    return udpsock.local_endpoint();
  }

  std::optional<udp::endpoint> get_global_endpoint() const noexcept {
    return global_endpoint;
  }

  // ненадёжное отправление, может потеряться
  void async_stun_request(udp::endpoint stun_server_addr,
                          move_only_fn_soos<void(std::optional<udp::endpoint>)> cb, bool force_rehash) {
    if (!listens)
      listen();  // нужно слушать ответы
    if (!force_rehash && global_endpoint) {
      boost::asio::post(get_executor(), [cb = std::move(cb), ep = global_endpoint]() mutable { cb(ep); });
      return;
    }
    if (stun_response_waiter) {
      stun_response_waiter = [me = std::move(stun_response_waiter),
                              cb = std::move(cb)](std::optional<udp::endpoint> ep) mutable {
        me(ep);
        cb(ep);
      };
    } else {
      stun_response_waiter = std::move(cb);
    }
    std::unique_ptr req = std::make_unique<std::array<byte_t, 20>>(stun_build_binding_request(stun_tid));
    auto buf = boost::asio::const_buffer(req->data(), req->size());
    udpsock.async_send_to(buf, stun_server_addr,
                          [req = std::move(req), stun_server_addr](io_error_code const& ec, size_t) {
                            if (ec)
                              HTTP2_LOG_WARN("acceptor fails to write STUN request, err: {}", ec.message());
                          });
  }

  void listen() {
    if (listens)
      return;
    if (!udpsock.is_open()) {
      udpsock.open(my_endpoint.protocol());
      udpsock.bind(my_endpoint);
    }
    start_server_read_loop();
    listens = true;
  }

  void close() {
    udpsock.close();
    if (m_sock) {
      m_sock->shutdown();
    }
    listens = false;
    cancel_accept();
    cancel_stun_request();
  }

  dd::task<tudp_server_socket> accept_from(udp::endpoint ep, io_error_code& ec, http2::deadline_t deadline) {
    // acceptor сейчас обслуживает лишь один сокет (для простоты)
    assert(!m_sock);  // уже идёт accept.

    boost::asio::steady_timer timer(udpsock.get_executor());

    timer.expires_at(deadline.tp);
    timer.async_wait([this](io_error_code const& ec) {
      if (!ec)
        cancel_accept();
    });
    tudp_server_socket sock(ioctx);

    if (ep.protocol() != local_endpoint().protocol()) {
      ec = boost::asio::error::address_family_not_supported;
      co_return sock;
    }
    tudp_server_socket::impl& s = *sock.pimpl;

    if (!listens)
      listen();
    s.scid = generate_connection_id();
    m_sock = &s;
    s.start_write_loop();
    s.creator = shared_from_this();
    s.destination = ep;
    // посылаем hello с подтверждением
    s.send_packet(s.scid, 0, TUDP_CONNECT_PACKET_NMB, {});
    accepter = co_await dd::this_coro::handle;
    // ждём успеха или отмены
    co_await std::suspend_always{};
    if (m_sock == nullptr)
      ec = boost::asio::error::operation_aborted;
    else
      assert(m_sock == sock.pimpl.get());
    co_await http2::yield_on_ioctx(ioctx);

    co_return sock;
  }

  void cancel_accept() {
    if (accepter) {
      m_sock = nullptr;  // сначала ставим nullptr, чтобы показать отменённость операции
      std::exchange(accepter, nullptr).resume();
    }
  }

  void cancel_stun_request() {
    if (stun_response_waiter) {
      std::exchange(stun_response_waiter, {})(std::nullopt);
    }
  }

  const boost::asio::any_io_executor& get_executor() noexcept {
    return udpsock.get_executor();
  }

  size_t active_connections_count() const noexcept {
    return m_sock && m_sock->dcid != 0 ? 1 : 0;
  }

 private:
  void start_server_read_loop() {
    udpsock.cancel();  // гарантируем что только 1 цикл
    buf.resize(TUDP_MAX_DATAGRAM_SIZE);
    udpsock.async_receive_from(boost::asio::mutable_buffer(buf.data(), buf.size()), sender_ep,
                               read_from_loop_callback{shared_from_this()});
  }

  void send_ack_to(boost::asio::ip::udp::endpoint p, cid_t scid, cid_t dcid, uint64_t nmb) noexcept {
    // ignores possible bad alloc
    bytes_t acks(form_ack_datagram(scid, dcid, nmb));
    auto buf = boost::asio::const_buffer(acks.data(), acks.size());
    udpsock.async_send_to(buf, p, [acks = std::move(acks)](const io_error_code& ec, size_t s) {
      assert(ec || s == acks.size());
    });
  }

  tudp_server_socket::impl* route(cid_t dcid) noexcept {
    return m_sock && m_sock->scid == dcid ? m_sock : nullptr;
  }

  void connect_with(tudp_server_socket::impl* s, cid_t dcid) {
    m_sock->dcid = dcid;
    m_sock->detector.start([s = m_sock] { s->shutdown(); }, m_sock->options.idle_timeout);
    assert(accepter);
    std::exchange(accepter, nullptr).resume();
  }

  [[nodiscard]] bool receive_data_dg(tudp_data_datagram const& dg) noexcept {
    HTTP2_LOG_TRACE("tudp server received data, dcid: {}, scid: {}, plsz: {} ", dg.dcid, dg.scid,
                    dg.payload.size());
    if (!m_sock)
      return false;

    if (is_connect_request(dg)) [[unlikely]] {
      // уже соединены
      if (m_sock->dcid == dg.scid) {
        // дублирование сообщения или потерялся ACK и отправитель продублировал connect запрос
        send_ack_to(sender_ep, m_sock->scid, dg.scid, dg.packet_nmb);
        return true;
      }
      // ещё не законнекчен
      else if (m_sock->dcid == 0 && sender_ep == m_sock->destination) {
        send_ack_to(sender_ep, m_sock->scid, dg.scid, dg.packet_nmb);
        connect_with(m_sock, dg.scid);
        return true;
      }
      return false;
    }
    // DATA
    if (auto* s = route(dg.dcid)) {
      // посылаем ACK только для не откинутых запросов
      if (s->destination != sender_ep) [[unlikely]]
        s->destination = sender_ep;  // отправитель переехал
      send_ack_to(s->destination, s->scid, dg.scid, dg.packet_nmb);
      s->receive_data_dg(dg);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool receive_ack_dg(tudp_ack_datagram const& dg) noexcept {
    HTTP2_LOG_TRACE("tudp server received ack, dcid: {}, scid: {}, plsz: {} ", dg.dcid, dg.scid,
                    dg.payload.size());
    if (auto* s = route(dg.dcid)) {
      if (s->dcid == 0) {
        if (s->destination != sender_ep)
          return false;  // пока не соединились запрещаем переезд
        // получили ack на hello
        connect_with(s, dg.scid);
      }
      if (s->destination != sender_ep) [[unlikely]]
        s->destination = sender_ep;  // отправитель переехал

      s->receive_ack_dg(dg);
      return true;
    }
    return false;
  }

  void handle_unparsable_packet(std::span<const byte_t> data) {
    // TUDP специально построен так, чтобы STUN запросы/ответы попадали сюда. (у STUN 00 в начале)
    std::optional addr = parse_stun_response(data, stun_tid);  // stun_tid не меняется с создания acceptor
    if (addr) {
      global_endpoint = addr;
      if (stun_response_waiter)
        std::exchange(stun_response_waiter, {})(global_endpoint);
    }
  }

  void send_pong_to(udp::endpoint u, cid_t scid, cid_t dcid) {
    std::unique_ptr ptr = std::make_unique<std::array<byte_t, 17>>(form_pong_datagram(scid, dcid));
    boost::asio::const_buffer asiobuf(ptr->data(), ptr->size());
    udpsock.async_send_to(asiobuf, u, [_ = std::move(ptr)](const io_error_code&, size_t) {});
  }

  void receive_packet(std::span<const byte_t> data) noexcept {
    auto on_data = [&](tudp_data_datagram const& dg) { return receive_data_dg(dg); };
    auto on_ack = [&](tudp_ack_datagram const& dg) { return receive_ack_dg(dg); };
    auto on_unordered_data = [&](tudp_unordered_data_datagram const& dg) { return false; };
    auto on_ping = [&](tudp_ping_datagram const& dg) {
      if (auto* c = route(dg.dcid))
        c->detector.activity_happen();
      send_pong_to(sender_ep, dg.scid, dg.dcid);
      return true;
    };
    auto on_pong = [&](tudp_pong_datagram const& dg) {
      if (auto* c = route(dg.dcid))
        c->detector.activity_happen();
      return true;
    };

    if (!visit_packet(data, on_data, on_ack, on_unordered_data, on_ping, on_pong))
      handle_unparsable_packet(data);
  }

  struct read_from_loop_callback {
    // см. tudp_client_socket.cpp аналогичное место с read_loop_callback
    std::weak_ptr<impl> weak_self;

    void operator()(const io_error_code& ec, size_t readen) {
      if (ec) {
        if (ec == boost::asio::error::operation_aborted) [[unlikely]]
          return;
        else
          ;  // HTTP2_DO_LOG(WARN, "read loop(acceptor) error: {}", ec.message());
      }
      auto self = weak_self.lock();
      if (!self)
        return;
      self->receive_packet(std::span<const byte_t>(self->buf.data(), readen));
      // loop
      self->udpsock.async_receive_from(boost::asio::mutable_buffer(self->buf.data(), self->buf.size()),
                                       self->sender_ep, std::move(*this));
    }
  };
};

boost::asio::ip::udp::socket& tudp_server_socket::impl::owner_sock() noexcept {
  assert(creator);
  return creator->udpsock;
}

boost::asio::ip::udp::socket const& tudp_server_socket::impl::owner_sock() const noexcept {
  assert(creator);
  return creator->udpsock;
}

tudp_acceptor::tudp_acceptor(boost::asio::io_context& ctx, boost::asio::ip::udp::endpoint ep,
                             bool reuse_address)
    : pimpl(std::make_shared<impl>(ctx, std::move(ep), reuse_address)) {
}

boost::asio::ip::udp::endpoint tudp_acceptor::local_endpoint() const noexcept {
  return pimpl->local_endpoint();
}

void tudp_server_socket::impl::shutdown() noexcept {
  if (creator) {
    assert(creator->m_sock == this);
    creator->m_sock = nullptr;
    creator = nullptr;
  }
  detector.stop();
  tudp_socket_base::shutdown();
}

std::optional<udp::endpoint> tudp_acceptor::global_endpoint_cached() const noexcept {
  return pimpl->get_global_endpoint();
}

void tudp_acceptor::async_stun_request(udp::endpoint stun_server_addr,
                                       move_only_fn_soos<void(std::optional<udp::endpoint>)> cb,
                                       bool force_rehash) {
  return pimpl->async_stun_request(stun_server_addr, std::move(cb), force_rehash);
}

void tudp_acceptor::listen() {
  return pimpl->listen();
}

void tudp_acceptor::close() {
  pimpl->close();
}

dd::task<tudp_server_socket> tudp_acceptor::accept_from(udp::endpoint ep, io_error_code& ec,
                                                        http2::deadline_t deadline) {
  return pimpl->accept_from(std::move(ep), ec, deadline);
}

void tudp_acceptor::cancel_accept() {
  if (pimpl)
    pimpl->cancel_accept();
}

void tudp_acceptor::cancel_stun_request() {
  return pimpl->cancel_stun_request();
}

size_t tudp_acceptor::active_connections_count() const noexcept {
  return pimpl ? pimpl->active_connections_count() : 0;
}

const boost::asio::any_io_executor& tudp_acceptor::get_executor() noexcept {
  return pimpl->get_executor();
}

}  // namespace tudp
