#pragma once

#include "tudp_socket_base.hpp"

#include <boost/asio/ip/udp.hpp>
#undef NO_ERROR

namespace tudp {

struct tudp_client_socket {
 private:
  struct impl;
  std::shared_ptr<impl> pimpl;

 public:
  explicit tudp_client_socket(boost::asio::io_context& ctx);

  // создаёт сразу законнекченный сокет
  // dcid - connection id существующего на сервере соединения
  // remoteendpoint - адрес сервера
  // scid - опционально свой connection id
  tudp_client_socket(boost::asio::io_context& ctx, udp::endpoint remoteendpoint, cid_t dcid,
                     cid_t scid = generate_connection_id());

  // после мува использовать без переприсвоения нельзя
  tudp_client_socket(tudp_client_socket&&) = default;
  tudp_client_socket& operator=(tudp_client_socket&&) = default;

  ~tudp_client_socket() = default;

  void async_connect(udp::endpoint e, move_only_fn_soos<void(const io_error_code&)> cb);

  [[nodiscard("requires co_await")]] auto connect(udp::endpoint e, io_error_code& ec) {
    return dd::suspend_and_t{[&](std::coroutine_handle<> me) {
      async_connect(e, [me, e = &ec](const io_error_code& ec) {
        *e = ec;
        me.resume();
      });
    }};
  }

  // отменяет до этого начатую операцию async_connect или не делает ничего, если такой операции нет
  // `cb` вызывается в operation_aborted
  // может быть использовано для отмены по таймауту
  void cancel_connect();

  // пакетный(неупорядоченный) интерфейс

  std::optional<bytes_t> try_receive_unordered();
  void async_receive_unordered(move_only_fn_soos<void(std::span<const byte_t>)> cb);
  void async_send_unordered(std::span<const byte_t> packet, move_only_fn_soos<void(const io_error_code&)> cb);

  // потоковый интерфейс

  // возвращает количество прочтённых байт
  [[nodiscard]] size_t try_read(std::span<byte_t> buf) noexcept;

  // возвращает количество записанных байт
  size_t try_write(std::span<const byte_t>, io_error_code&);

  void async_read_some(boost::asio::mutable_buffer buf,
                       move_only_fn_soos<void(const io_error_code&, size_t)> cb);

  void async_write_some(boost::asio::const_buffer buf,
                        move_only_fn_soos<void(const io_error_code&, size_t)> cb);

  void shutdown() noexcept;

  boost::asio::ip::udp::endpoint local_endpoint() const noexcept;

  boost::asio::ip::udp::endpoint remote_endpoint() const noexcept;

  // source connection id
  // 0 означает не законнекчено
  cid_t get_scid() const noexcept;
  // destination connection id
  // 0 означает незаконнекчено
  cid_t get_dcid() const noexcept;

  void set_options(tudp_socket_options opts);

  // interface of asio::ssl::stream

  using lowest_layer_type = tudp_client_socket;

  lowest_layer_type& lowest_layer() noexcept {
    return *this;
  }
  const lowest_layer_type& lowest_layer() const noexcept {
    return *this;
  }

  using executor_type = boost::asio::any_io_executor;

  const executor_type& get_executor() noexcept;
};

}  // namespace tudp
