#pragma once

#include "tudp_protocol.hpp"

namespace tudp {

struct tudp_server_socket;

// слушает на UDP адресе и позволяет инициализировать tudp_server_socket'ы
// Все из них на самом деле на одном UDP адресе, данные между ними распределяет tudp_acceptor
// Например это облегчает пробитие NAT, так как нужно будет пробивать только один IP + port
// и сильно облегчает процесс коннекта - опять же из-за NAT.
// Ну и просто уменьшает нагрузку на систему - меньше сокетов
// TODO синхронизация когда сокет ушёл на другой поток
//
// Note: после установления соединения по TUDP в дальнейшем соединение переживает смену адреса клиента, даже
// если он за NAT (теоретически можно сделать переживание смены адреса у обоих ендпоинтов без NAT, но на
// стороне клиента это не реализовано т.к. редкая ситуация)
// - например, если ip клиента меняется - он вероятно не знает об этом, acceptor видит, что пакет
// пришёл с нового адреса и выставляет новый адрес в tudp_server_socket для отправки по правильному адресу.
// Клиентский сокет продолжает посылать пинги, поэтому дыра в NAT будет поддерживаться и серверный
// сокет в конечном счёте отправит данные клиенту
// TODO? специальный пакет для обновления адреса? Чтобы сервер мог сказать, что теперь посылай туда? И всем
// клиентам этот пакет послал, дождался ACK + запрещал в это время другие пакеты посылать, а затем закрывался
// бы?
// Note: tudp_accetor вместе со всеми порождёнными им серверными сокетами нужно использовать на одном потоке
struct tudp_acceptor {
  struct impl;

 private:
  std::shared_ptr<impl> pimpl;

 public:
  // ep - адрес который слушается
  // Насчёт `reuse_address`: документацию к азио найти +- невозможно, продебажил реализацию и узнал
  // * на linux непонятно что будет делать, документация противоречива
  // * на Windows ставит также REUSE_ADDRESS, но это означает только что можно забиндиться в сокет в состоянии
  // TIME_WAIT (для UDP кажется бесполезно)
  tudp_acceptor(boost::asio::io_context& ctx, udp::endpoint ep, bool reuse_address = true);
  // после мува использовать без переприсвоения нельзя
  tudp_acceptor(tudp_acceptor&&) = default;
  tudp_acceptor& operator=(tudp_acceptor&&) = default;
  ~tudp_acceptor() = default;

  udp::endpoint local_endpoint() const noexcept;

  // возвращает последний закешированный результат `async_stun_request`
  std::optional<udp::endpoint> global_endpoint_cached() const noexcept;

  // отправляет STUN запрос и ожидает ответа, после этого возвращает полученный адрес и хеширует
  // не пересылает запрос, он может потеряться
  void async_stun_request(udp::endpoint stun_server_addr,
                          move_only_fn_soos<void(std::optional<udp::endpoint>)> cb,
                          bool force_rehash = false);

  void listen();

  void close();

  void async_accept(tudp_server_socket& s, move_only_fn_soos<void(const io_error_code&)> cb);

  // принимает соединение только от конкретного ендпоинта
  // во время accept непрерывно посылает свои connect пакеты туда же
  // (на той стороне тоже должен быть async_accept_from)
  // вызываем калбек только после получения ACK от противоположной стороны
  // family (v4/v6) `ep` должен совпадать с тем который в был в конструкторе
  void async_accept_from(udp::endpoint, tudp_server_socket& s,
                         move_only_fn_soos<void(const io_error_code&)> cb);

  dd::task<tudp_server_socket> accept_from(udp::endpoint ep, io_error_code& ec,
                                           http2::deadline_t deadline = deadline_t::never());

  // отменяет до этого начатую операцию async_accept или не делает ничего, если такой операции нет
  // `cb` вызывается в operation_aborted
  // может быть использовано для отмены по таймауту
  void cancel_accept();

  void cancel_stun_request();

  size_t active_connections_count() const noexcept;

  const boost::asio::any_io_executor& get_executor() noexcept;

  // может быть использовано, чтобы отслеживать время жизни acceptor
  std::weak_ptr<impl> weak_impl() noexcept {
    return pimpl;
  }
};

}  // namespace tudp
