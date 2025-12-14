#pragma once

#include <span>
#include <array>
#include <optional>

#include <http2/utils/memory.hpp>

#include <kelcoro/task.hpp>
#include <kelcoro/job.hpp>

#include "http2/asio/tudp/tudp_protocol.hpp"
#include "http2/asio/tudp/tudp_acceptor.hpp"

namespace tudp {

// Transaction ID
using stun_tid_t = std::array<byte_t, 12>;

[[nodiscard]] stun_tid_t make_random_stun_tid() noexcept;

// `tid` это просто случайный набор байт, сгенерированный произвольно
[[nodiscard]] std::array<byte_t, 20> stun_build_binding_request(stun_tid_t tid);

// `tid` должен совпадать с тем, с которым отправлялся запрос
[[nodiscard]] std::optional<udp::endpoint> parse_stun_response(std::span<const byte_t> response,
                                                               stun_tid_t tid);

[[nodiscard]] std::vector<byte_t> stun_build_binding_success_response(stun_tid_t tid,
                                                                      udp::endpoint mapped_ep);

// возвращает tid из запроса
std::optional<stun_tid_t> parse_stun_binding_request(std::span<const byte_t> packet);

// test only
std::vector<endpoint> get_valid_stun_servers_list();

// делает попытки пока не наступит дедлайн
// `get_stun_server_addr` вызывается в начале каждой попытки STUN запроса,
// при исключении возвращается nullopt
// возвращает ip+port этого компьютера в глобальной сети (или локальной, если сервер в той же сети, что и мы)
dd::task<std::optional<udp::endpoint>> stun_request(tudp_acceptor&,
                                                    move_only_fn_soos<endpoint()> get_stun_server_addr,
                                                    deadline_t = deadline_after(std::chrono::seconds(5)));

// отправляет раз в `period` STUN запрос на следующий взятый адрес (если сейчас у acceptor нет соединений)
// это важно для поддержания NAT дыры, пока коннектов нет
// если из `get_stun_server_addr` вылетает исключение - останавливается
// следит за жизнью acceptor через weak_impl
dd::job maintain_nat_hole(tudp_acceptor& acceptor, duration_t period,
                          move_only_fn_soos<endpoint()> get_stun_server_addr);

}  // namespace tudp
