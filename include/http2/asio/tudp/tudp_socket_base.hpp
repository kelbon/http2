#pragma once

#include <deque>
#include <map>
#include <unordered_set>

#include <http2/utils/merged_segments.hpp>

#include "tudp_protocol.hpp"

namespace tudp {

struct tudp_socket_options {
  // период между проверками для каких пакетов нужно повторить отправку
  duration_t ack_check_period = std::chrono::milliseconds(5);
  // сколько времени для каждого пакета ждать подтверждения, прежде чем повторно его посылать
  // Note: может динамически меняться, например из-за отслеживания реального RTT
  duration_t resend_period = std::chrono::milliseconds(50);
  // пакеты будут отсылаться `max_retry_count` раз перед тем как сказать, что соединение сломано
  size_t max_retry_count = size_t(-1);
  // максимальный размер пакета, на который разбиваются ПОТОКОВЫЕ данные
  // (не влияет на размер unordered пакетов)
  // значение по умолчанию выбрано на основе разбиения пакетов на уровне IP (около 1500 байт) минус оверхед на
  // UDP, TUDP и IP протоколы
  // в некоторых ipv4 сетях разбиение пакетов может быть уже на 576 байтах
  // Само по себе разбиение не должно влиять, но оно увеличивает вероятность потери UDP датаграммы
  // (потеря одной части == потеря всей датаграммы)
  // с другой стороны, при уменьшении max_data_packet_size увеличивается оверхед на передачу (больше пакетов)
  // https://www.rfc-editor.org/rfc/rfc791
  // "All hosts must be prepared to accept datagrams of up to 576 octets"
  size_t max_data_packet_size = 1300;
  // не влияет на серверные сокеты, пинги шлют только клиентские
  duration_t ping_period = std::chrono::seconds(20);
  // только для серверных сокетов. Промежуток времени неактивности клиента, после которого соединение
  // считается потерянным
  duration_t idle_timeout = std::chrono::seconds(60);
  // TODO max_send_buf / max_receive_buf чтобы память не могла переполниться
};

// служит основной для реализации tudp_client_socket / tudp_server_socket
// * Даёт возможность посылать данные потоко - без потери с сохранением порядка
// своего рода TCP реализованное над UDP
// * параллельно этому можно посылать и принимать неупорядоченные пакеты - обычные UDP датаграммы с маленьким
// тегом от tudp - просто для переиспользования одного сокета и удобства
struct tudp_socket_base {
 protected:
  // source connection id
  cid_t scid = 0;
  // destination connection id
  cid_t dcid = 0;
  // номер который будет в следующем DATA пакете
  size_t sent_packet_nmb = 0;
  // next read packet (in stream)
  size_t readen_packet_nmb = 0;
  // отправленные, но ещё не подтверждённые пакеты
  std::unordered_set<sent_packet, sent_packet::packet_hash, sent_packet::packet_equal> sent;
  // полученные данные (распарешнные), которые ещё не до конца собраны или не вычитаны
  // invariant: не содержит пустых блоков
  std::map<size_t, bytes_t> received;  // !TODO вынести в отдельную сущность, которую можно тестировать и тд
  http2::merged_segments already_received_packet_nmbs;
  move_only_fn_soos<void(const io_error_code&, size_t)> reader;
  // выставлено только если `reader` != nullptr
  std::span<byte_t> reader_wants;
  // ожидающий следующего пакета неупорядоченных данных
  move_only_fn_soos<void(std::span<const byte_t>)> reader_unordered;
  std::deque<bytes_t> unordered_packets;
  // периодически проверяет `sent` и отсылает пакеты снова, если долго не получено подтверждение
  boost::asio::steady_timer ack_requester;
  tudp_socket_options options;
  // выставляется если не получилось получить ACK за max_retry_count попыток для какого-то из пакетов
  // все операции фейлятся после этого
  bool broken = false;

  virtual void do_async_send(std::span<const byte_t> buf,
                             move_only_fn_soos<void(const io_error_code&, size_t)>) = 0;

 public:
  decltype(auto) get_executor() noexcept {
    return ack_requester.get_executor();
  }

  explicit tudp_socket_base(boost::asio::io_context& ctx) noexcept : ack_requester(ctx) {
  }

  tudp_socket_base(tudp_socket_base&&) = delete;
  void operator=(tudp_socket_base&&) = delete;

  virtual ~tudp_socket_base() = default;

  // пакетный (неупорядоченный) интерфейс

  // читает из потока неупорядоченных пакетов. Никак не влияет на стримовый поток
  // возвращает std::nullopt, если сейчас пакетов нет
  std::optional<bytes_t> try_receive_unordered() {
    assert(!reader_unordered);
    if (unordered_packets.empty())
      return std::nullopt;
    on_scope_exit {
      unordered_packets.pop_front();
    };
    return std::optional<bytes_t>(std::in_place, std::move(unordered_packets.front()));
  }

  // читает из потока неупорядоченных пакетов. Никак не влияет на стримовый поток
  void async_receive_unordered(move_only_fn_soos<void(std::span<const byte_t>)> cb) {
    assert(!reader_unordered);
    std::optional packet = try_receive_unordered();
    if (packet) {
      boost::asio::post(get_executor(),
                        [cb = std::move(cb), p = std::move(packet)]() mutable { cb(*std::move(p)); });
      return;
    }
    reader_unordered = std::move(cb);
  }

  // precondition: пакет должен начинаться с `unordered_data_prefix(this->get_scid(), this->get_dcid())`
  // packet.size() <= 64`000
  void async_send_unordered(std::span<const byte_t> packet,
                            move_only_fn_soos<void(const io_error_code&)> cb) {
    assert(packet.size() <= 64000);
    assert(dcid && "unconnected");
    assert(std::ranges::equal(std::span(packet).subspan(17), unordered_data_prefix(scid, dcid)));
    do_async_send(std::span(packet),
                  [cb = std::move(cb)](io_error_code const& ec, size_t) mutable { cb(ec); });
  }

  // потоковый интерфейс

  // возвращает количество прочтённых байт
  [[nodiscard]] size_t try_read(std::span<byte_t> buf) noexcept {
    if (received.empty())
      return 0;
    size_t readen = 0;
    while (!received.empty() && buf.size() > 0 && received.begin()->first == readen_packet_nmb) {
      auto it = received.begin();
      auto& [_, packet] = *it;

      // читаем продолжение "потока"
      if (buf.size() >= packet.size()) {
        memcpy(buf.data(), packet.data(), packet.size());
        readen += packet.size();
        buf = buf.subspan(packet.size());
        it = received.erase(it);
        ++readen_packet_nmb;
      } else {  // buf.size() < packet.size()
        memcpy(buf.data(), packet.data(), buf.size());
        readen += buf.size();
        packet.erase(packet.begin(), packet.begin() + buf.size());
        break;
      }
    }
    return readen;
  }

  void async_read_some(boost::asio::mutable_buffer buf,
                       move_only_fn_soos<void(const io_error_code&, size_t)> cb) {
    assert(!reader);
    if (broken) [[unlikely]] {
      boost::asio::post(get_executor(),
                        [cb = std::move(cb)]() mutable { cb(boost::asio::error::network_down, 0); });
      return;
    }
    std::span<byte_t> sbuf((byte_t*)buf.data(), buf.size());
    if (size_t readen = try_read(sbuf); readen > 0) {
      notify_reader(get_executor(), std::move(cb), readen);
      return;
    }
    reader = std::move(cb);
    reader_wants = sbuf;
  }

  // возвращает количество записанных байт
  size_t try_write(std::span<const byte_t> buf, io_error_code& ec) {
    assert(options.max_data_packet_size > 0 && !ec);
    assert(dcid && "unconnected");
    if (broken) [[unlikely]] {
      ec = boost::asio::error::network_down;
      return 0;
    }
    const size_t written = buf.size();
    while (!buf.empty()) {
      size_t packet_size = std::min(buf.size(), options.max_data_packet_size);
      send_packet(scid, dcid, sent_packet_nmb++, buf.subspan(0, packet_size));
      buf = buf.subspan(packet_size);
    }
    return written;
  }

  void async_write_some(boost::asio::const_buffer buf,
                        move_only_fn_soos<void(const io_error_code&, size_t)> cb) {
    io_error_code ec;
    size_t written = try_write({(const byte_t*)buf.data(), buf.size()}, ec);
    if (ec) [[unlikely]] {
      cb(ec, 0);
      return;
    }
    cb(io_error_code{}, written);
  }

  void shutdown() noexcept {
    cancel_all_with_error(boost::asio::error::operation_aborted);
    received.clear();
    dcid = 0;
    sent_packet_nmb = 0;
    readen_packet_nmb = 0;
    already_received_packet_nmbs.clear();
    broken = false;
  }

  void set_options(tudp_socket_options opts) {
    // restrict
    opts.ack_check_period = std::max<duration_t>(std::chrono::nanoseconds(10), opts.ack_check_period);
    opts.max_retry_count = std::max<size_t>(opts.max_retry_count, 1);
    opts.resend_period = std::max<duration_t>(opts.resend_period, std::chrono::milliseconds(1));
    opts.max_data_packet_size = std::clamp<size_t>(opts.max_data_packet_size, 100, 60'000);
    opts.idle_timeout = std::max<duration_t>(opts.idle_timeout, std::chrono::seconds(10));
    // на большинстве NAT за 5 минут могут закрыться дыры
    opts.ping_period =
        std::clamp<duration_t>(opts.ping_period, std::chrono::seconds(1), std::chrono::minutes(5));
    // apply
    if (opts.ack_check_period != options.ack_check_period) {
      options.ack_check_period = opts.ack_check_period;
      start_write_loop();
    }
    options = opts;
  }

  cid_t get_scid() const noexcept {
    return scid;
  }
  cid_t get_dcid() const noexcept {
    return dcid;
  }

 protected:
  [[nodiscard]] bool receive_unordered_data_dg(tudp_unordered_data_datagram const& dg) noexcept {
    assert(scid == dg.dcid);
    assert(dg.scid == dcid);
    if (reader_unordered) {
      std::exchange(reader_unordered, {})(dg.payload);
    } else {
      unordered_packets.push_back(bytes_t(dg.payload.begin(), dg.payload.end()));
    }
    return true;
  }

  void cancel_all_with_error(boost::asio::error::basic_errors e) {
    sent.clear();
    ack_requester.cancel();
    if (reader) {
      std::exchange(reader, {})(e, 0);
      reader_wants = {};
    }
  }

  void send_ack(uint64_t nmb) {
    std::shared_ptr acks = std::make_shared<bytes_t>(form_ack_datagram(scid, dcid, nmb));
    std::span<const byte_t> buf(*acks);
    do_async_send(buf, [acks = std::move(acks)](const io_error_code& ec, size_t s) {
      assert(ec || s == acks->size());
    });
  }

  void resend_packet(sent_packet const& packet) {
    packet.when_sent = timestamp();
    if (packet.retries_done >= options.max_retry_count) [[unlikely]] {
      broken = true;
      cancel_all_with_error(boost::asio::error::network_down);
      return;
    }
    ++packet.retries_done;
    do_async_send(std::span<const byte_t>(packet.payload), [](io_error_code const&, size_t) {});
  }

  // `dcid` must be 0 for connection
  void send_packet(cid_t scid, cid_t dcid, size_t nmb, std::span<const byte_t> payload) {
    // сохраняем пакет, чтобы можно было сделать ретрай позже
    // он будет убран из `sent` когда будет получен ACK
    auto [it, emplaced] = sent.insert(sent_packet{
        .n = nmb,
        .payload = form_data_datagram(scid, dcid, nmb, payload),
        .when_sent = {},
    });
    assert(emplaced);  // nmb must be unique
    resend_packet(*it);
  }

  void start_write_loop() {
    // гарантируем один цикл записи
    ack_requester.cancel();
    ack_requester.expires_after(options.ack_check_period);
    ack_requester.async_wait(ack_requester_callback{this});
  }

  struct ack_requester_callback {
    tudp_socket_base* self = nullptr;

    void operator()(io_error_code const& ec) {
      if (ec)
        return;
      // loop
      self->ack_requester.expires_after(self->options.ack_check_period);
      self->ack_requester.async_wait(*this);

      auto now = timestamp();
      for (sent_packet const& p : self->sent) {
        if (now - p.when_sent < self->options.resend_period)
          continue;
        (void)self->resend_packet(p);
      }
    }
  };
};

}  // namespace tudp
