#include "http2/asio/tudp/tudp_acceptor.hpp"

#include <deque>

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
  void receive_ack(uint64_t nmb) {
    if (nmb > sent_packet_nmb && nmb != TUDP_CONNECT_PACKET_NMB) [[unlikely]] {
      HTTP2_DO_LOG(WARN, "ack packet which is not sent by this connection");
      return;
    }
    if (auto it = sent.find(nmb); it != sent.end())
      sent.erase(it);
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
      receive_ack(take_uint64(payload));
  }
};

tudp_server_socket::tudp_server_socket(boost::asio::io_context& ctx) : pimpl(std::make_shared<impl>(ctx)) {
}

std::optional<bytes_t> tudp_server_socket::try_receive_unordered() {
  return pimpl->try_receive_unordered();
}

void tudp_server_socket::async_receive_unordered(
    move_only_fn_soos<void(std::span<const byte_t>, io_error_code const&)> cb) {
  return pimpl->async_receive_unordered(std::move(cb));
}

void tudp_server_socket::async_send_unordered(std::span<byte_t> packet,
                                              move_only_fn_soos<void(const io_error_code&)> cb) {
  return pimpl->async_send_unordered(packet, std::move(cb));
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
  // каждый tudp_server_socket::impl держит shared_ptr на *this и каждый из них нельзя мувать
  // в деструкторе они убирают себя из connections, тем самым гарантируя, что не переживут создателя
  // invariant: не содержит нулей (ни cid, ни сокетов)
  // source connection id -> socket
  std::unordered_map<cid_t, tudp_server_socket::impl*> connections;
  // назначено только во время accept
  move_only_fn_soos<void(const io_error_code&)> accept_callback;
  std::optional<udp::endpoint> accept_from_ep;
  boost::asio::steady_timer accept_from_timer;
  cid_t accept_from_scid = 0;
  // если true то accept_from_ep != nullopt
  // accept_from_timer ещё взведён, ждём ack от противоположной стороны чтобы завершить accept_from
  bool waiting_ack = false;
  // массив source connection id пришедших запросов на соединение
  std::deque<std::pair<cid_t, udp::endpoint>> accepted_already;
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
      : ioctx(ctx), accept_from_timer(ctx), udpsock(ctx), my_endpoint(ep) {
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
    for (auto&& [_, con] : connections)
      con->shutdown();
    listens = false;
    cancel_accept();
    cancel_stun_request();
  }

  void async_accept(tudp_server_socket& ss, move_only_fn_soos<void(const io_error_code&)> cb) {
    assert(ss.pimpl);
    tudp_server_socket::impl& s = *ss.pimpl;
    assert(s.scid == 0 && s.dcid == 0);  // already connected
    if (!listens)
      listen();
    do {
      s.scid = generate_connection_id();  // server connection id
    } while (route(s.scid));  // избегаем совпадения (маловероятного)

    if (!accepted_already.empty()) {
      on_accept(pop_accepted(), s, cb);
      return;
    }
    accept_callback = [this, res = &s, cb = std::move(cb)](io_error_code const& ec) mutable {
      if (ec) {
        boost::asio::post(udpsock.get_executor(), std::bind_front(std::move(cb), ec));
        return;
      }
      // использует `accepted_already`
      on_accept(pop_accepted(), *res, cb);
    };
  }

  void async_accept_from(udp::endpoint ep, tudp_server_socket& ss,
                         move_only_fn_soos<void(const io_error_code&)> cb) {
    assert(ss.pimpl);
    if (ep.protocol() != local_endpoint().protocol()) {
      cb(boost::asio::error::address_family_not_supported);
      return;
    }
    tudp_server_socket::impl& s = *ss.pimpl;
    assert(s.scid == 0 && s.dcid == 0);  // already connected
    if (!listens)
      listen();
    do {
      s.scid = generate_connection_id();  // server connection id
    } while (route(s.scid));  // избегаем совпадения (маловероятного)
    accept_from_scid = s.scid;
    // начинаем периодически писать коннект запросы
    accept_from_timer.expires_after(std::chrono::milliseconds(100));
    accept_from_timer.async_wait(write_connect_loop_cb(shared_from_this(), s.scid));

    if (std::optional x = pop_with_endpoint(ep)) {
      on_accept(std::move(*x), s, cb);
      return;
    }
    accept_from_ep = ep;
    accept_callback = [this, res = &s, cb = std::move(cb)](io_error_code const& ec) mutable {
      if (ec) {
        boost::asio::post(udpsock.get_executor(), std::bind_front(std::move(cb), ec));
        return;
      }
      assert(waiting_ack);
      waiting_ack = false;
      // вызывается только после складывания туда пары с правильным endpoint
      std::optional x = pop_with_endpoint(*accept_from_ep);
      assert(x);
      accept_from_timer.cancel();
      on_accept(std::move(*x), *res, cb);
    };
  }

  dd::task<tudp_server_socket> accept_from(udp::endpoint ep, io_error_code& ec, http2::deadline_t deadline) {
    boost::asio::steady_timer timer(udpsock.get_executor());

    timer.expires_at(deadline.tp);
    timer.async_wait([this](io_error_code const& ec) {
      if (!ec)
        cancel_accept();
    });
    tudp_server_socket sock(ioctx);

    co_await dd::suspend_and_t{[&](std::coroutine_handle<> me) {
      async_accept_from(ep, sock, [&, me](const io_error_code& ec2) {
        ec = ec2;
        boost::asio::post(ioctx, me);
      });
    }};

    co_return sock;
  }

  void cancel_accept() {
    if (accept_callback) {
      std::exchange(accept_callback, {})(boost::asio::error::operation_aborted);
    }
    accept_from_ep = std::nullopt;
    accept_from_timer.cancel();
    accept_from_scid = 0;
  }

  void cancel_stun_request() {
    if (stun_response_waiter) {
      std::exchange(stun_response_waiter, {})(std::nullopt);
    }
  }

  size_t active_connections_count() const noexcept {
    return connections.size();
  }

  const boost::asio::any_io_executor& get_executor() noexcept {
    return udpsock.get_executor();
  }

 private:
  void start_server_read_loop() {
    udpsock.cancel();  // гарантируем что только 1 цикл
    buf.resize(TUDP_MAX_DATAGRAM_SIZE);
    udpsock.async_receive_from(boost::asio::mutable_buffer(buf.data(), buf.size()), sender_ep,
                               read_from_loop_callback{shared_from_this()});
  }

  std::optional<std::pair<uint64_t, udp::endpoint>> pop_with_endpoint(const udp::endpoint& ep) {
    auto it = accepted_already.begin();
    for (; it != accepted_already.end(); ++it) {
      if (it->second == ep) {
        auto res = std::move(*it);
        accepted_already.erase(it);
        return res;
      }
    }
    return std::nullopt;
  }

  std::pair<uint64_t, udp::endpoint> pop_accepted() {
    std::pair x = std::move(accepted_already.front());
    accepted_already.pop_front();
    return x;
  }
  // precondition: !accepted_already.empty()
  void on_accept(std::pair<uint64_t, udp::endpoint> p, tudp_server_socket::impl& s,
                 std::invocable<io_error_code> auto&& cb) {
    auto [dcid, ep] = p;

    s.dcid = dcid;
    s.start_write_loop();
    s.creator = shared_from_this();
    s.destination = ep;
    // Note: устанавливать опции в серверный сокет нужно до accept..
    s.detector.start([&s] { s.shutdown(); }, s.options.idle_timeout);
    connections.try_emplace(s.scid, &s);
    send_ack_to(ep, s.scid, dcid, TUDP_CONNECT_PACKET_NMB);
    boost::asio::post(udpsock.get_executor(), std::bind_front(std::move(cb), io_error_code{}));
  }

  void send_ack_to(boost::asio::ip::udp::endpoint p, cid_t scid, cid_t dcid, uint64_t nmb) noexcept {
    // ignores possible bad alloc
    bytes_t acks(form_ack_datagram(scid, dcid, nmb));
    auto buf = boost::asio::const_buffer(acks.data(), acks.size());
    udpsock.async_send_to(buf, p, [acks = std::move(acks)](const io_error_code& ec, size_t s) {
      assert(ec || s == acks.size());
    });
  }

  // находит соединение по ID ИСХОДА
  [[nodiscard]] tudp_server_socket::impl* route(cid_t dcid) noexcept {
    auto it = connections.find(dcid);
    return it != connections.end() ? it->second : nullptr;
  }

  // находит соединение по ID НАЗНАЧЕНИЯ
  [[nodiscard]] tudp_server_socket::impl* already_connected_to(cid_t scid) noexcept {
    for (auto&& [_, c] : connections) {
      if (c->dcid == scid)
        return c;
    }
    return nullptr;
  }

  [[nodiscard]] bool already_accepted(cid_t scid) noexcept {
    for (auto&& [id, ep] : accepted_already) {
      if (id == scid)
        return true;
    }
    return false;
  }

  [[nodiscard]] bool receive_data_dg(tudp_data_datagram const& dg) noexcept {
    HTTP2_LOG_TRACE("tudp server received data, dcid: {}, scid: {}, plsz: {} ", dg.dcid, dg.scid,
                    dg.payload.size());
    if (is_connect_request(dg)) [[unlikely]] {
      if (auto* c = already_connected_to(dg.scid)) {
        // отправляем ответное hello, на случай если это попытка соединиться из accept_from
        bytes_t hello = form_hello_datagram(c->scid);
        udpsock.send_to(boost::asio::const_buffer(hello.data(), hello.size()), sender_ep);
        // дублирование сообщения или потерялся ACK и отправитель продублировал connect запрос
        send_ack_to(sender_ep, c->scid, dg.scid, dg.packet_nmb);
        return true;
      }
      if (already_accepted(dg.scid)) {
        if (accept_from_ep && *accept_from_ep == sender_ep) {
          // мы и есть получатель соединения, так что отправляем ACK
          send_ack_to(sender_ep, accept_from_scid, dg.scid, dg.packet_nmb);
        } else {
          // не отправляем ACK, потому что ACK отправит tudp_server_socket который возьмёт соединение
        }
        return true;
      }
      accepted_already.push_back({dg.scid, sender_ep});
      if (accept_callback) {
        if (!accept_from_ep) {
          // калбек пошлёт ACK когда кто-то примет соединение.
          // А до этих пор клиент будет дальше отправлять повторы
          std::exchange(accept_callback, {})(io_error_code{});
        } else if (*accept_from_ep == sender_ep) {
          waiting_ack = true;  // позже при получении ACK разбудим калбек accept_from
        }
      }
      return true;
    }
    // DATA for someone
    if (tudp_server_socket::impl* s = route(dg.dcid)) [[likely]] {
      // посылаем ACK только для не откинутых запросов
      if (s->destination != sender_ep) [[unlikely]]
        s->destination = sender_ep;  // клиент переехал
      send_ack_to(s->destination, s->scid, dg.scid, dg.packet_nmb);
      s->receive_data_dg(dg);
      return true;
    }
    return false;
  }

  [[nodiscard]] bool receive_ack_dg(tudp_ack_datagram const& dg) noexcept {
    HTTP2_LOG_TRACE("tudp server received ack, dcid: {}, scid: {}, plsz: {} ", dg.dcid, dg.scid,
                    dg.payload.size());
    bool result = false;
    if (waiting_ack) {
      assert(accept_from_ep);
      if (*accept_from_ep == sender_ep) {
        accepted_already.push_front({dg.scid, sender_ep});
        std::exchange(accept_callback, {})(io_error_code{});
        result = true;
      }
    }
    if (tudp_server_socket::impl* s = route(dg.dcid)) [[likely]] {
      if (s->destination != sender_ep) [[unlikely]]
        s->destination = sender_ep;  // клиент переехал
      s->receive_ack_dg(dg);
      return true;
    }
    return result;
  }

  [[nodiscard]] bool receive_unordered_data_dg(tudp_unordered_data_datagram const& dg) noexcept {
    if (tudp_server_socket::impl* s = route(dg.dcid)) [[likely]] {
      if (s->destination != sender_ep) [[unlikely]]
        s->destination = sender_ep;  // клиент переехал
      return s->receive_unordered_data_dg(dg);
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
    auto on_unordered_data = [&](tudp_unordered_data_datagram const& dg) {
      return receive_unordered_data_dg(dg);
    };
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

  struct write_connect_loop_cb {
    std::shared_ptr<impl> self = nullptr;
    cid_t scid = 0;

    void operator()(const io_error_code& ec) {
      if (ec) {
        if (ec != boost::asio::error::operation_aborted) [[unlikely]]
          HTTP2_DO_LOG(WARN, "write loop(acceptor) ends with error: {}", ec.message());
        return;
      }
      if (self.use_count() == 1 || !self->accept_from_ep)
        return;
      assert(self->accept_from_ep->protocol() == self->udpsock.local_endpoint().protocol());
      bytes_t b = form_hello_datagram(scid);
      // вероятно тут крайне сложно получить ошибку
      self->udpsock.send_to(boost::asio::const_buffer(b.data(), b.size()), *self->accept_from_ep);
      // loop
      self->accept_from_timer.expires_after(std::chrono::milliseconds(100));
      self->accept_from_timer.async_wait(*this);
    }
  };

  struct read_from_loop_callback {
    // см. tudp_client_socket.cpp аналогичное место с read_loop_callback
    std::shared_ptr<impl> self = nullptr;

    void operator()(const io_error_code& ec, size_t readen) {
      if (ec) {
        if (ec == boost::asio::error::operation_aborted) [[unlikely]]
          return;
        else
          ;  // HTTP2_DO_LOG(WARN, "read loop(acceptor) error: {}", ec.message());
      }
      if (self.use_count() == 1)
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
    auto count = creator->connections.erase(scid);
    (void)count;
    assert(count == 1);
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

void tudp_acceptor::async_accept(tudp_server_socket& s, move_only_fn_soos<void(const io_error_code&)> cb) {
  pimpl->async_accept(s, std::move(cb));
}

void tudp_acceptor::async_accept_from(udp::endpoint ep, tudp_server_socket& s,
                                      move_only_fn_soos<void(const io_error_code&)> cb) {
  pimpl->async_accept_from(ep, s, std::move(cb));
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
