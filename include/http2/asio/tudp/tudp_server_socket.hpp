#pragma once

#include "tudp_socket_base.hpp"
#include "tudp_acceptor.hpp"

#include <boost/asio/ip/udp.hpp>
#undef NO_ERROR

namespace tudp {

struct tudp_server_socket {
 private:
  friend struct tudp_acceptor::impl;
  // in tudp_acceptor.cpp
  struct impl;
  std::shared_ptr<impl> pimpl;

 public:
  explicit tudp_server_socket(boost::asio::io_context& ctx);

  // после мува использовать без переприсвоения нельзя
  tudp_server_socket(tudp_server_socket&&) = default;
  tudp_server_socket& operator=(tudp_server_socket&&) = default;

  ~tudp_server_socket() = default;

  // пакетный(неупорядоченный) интерфейс

  std::optional<bytes_t> try_receive_unordered();
  void async_receive_unordered(move_only_fn_soos<void(std::span<const byte_t>, io_error_code const&)> cb);
  // precondition: пакет должен начинаться с TUDP_UD_PREFIX_LEN нулей
  void async_send_unordered(std::span<byte_t> packet, move_only_fn_soos<void(const io_error_code&)> cb);

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

  // желательно выставить до accept, чтобы все настройки были применены
  void set_options(tudp_socket_options);

  // source connection id
  // 0 означает не законнекчено
  cid_t get_scid() const noexcept;
  // destination connection id
  // 0 означает незаконнекчено
  cid_t get_dcid() const noexcept;

  boost::asio::ip::udp::endpoint local_endpoint() const noexcept;

  boost::asio::ip::udp::endpoint remote_endpoint() const noexcept;

  // interface of asio::ssl::stream
  using lowest_layer_type = tudp_server_socket;

  using executor_type = boost::asio::any_io_executor;

  const executor_type& get_executor() noexcept;

  lowest_layer_type& lowest_layer() noexcept {
    return *this;
  }
  const lowest_layer_type& lowest_layer() const noexcept {
    return *this;
  }
};

}  // namespace tudp
