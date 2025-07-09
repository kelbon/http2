
#pragma once

#include "http2v2/any_connection.hpp"
#include "http2v2/errors.hpp"
#include "http2v2/utils/deadline.hpp"
#include "http2v2/utils/memory.hpp"

#include <kelcoro/task.hpp>
#include <yacore/net/address.hpp>

namespace http2v2 {

struct transport_factory_i {
  // postcondition: .has_value() == true
  virtual dd::task<any_connection_t>
  createConnection(sockaddr_or_fqdn_t, seastar::socket_address srcaddr,
                   deadline_t) = 0;
  virtual dd::task<void> sleep(duration_t d, io_error_code &ec) = 0;
  // specially aborts all 'sleep' calls currently in progress
  virtual void abortSleeps() noexcept = 0;
  virtual ~transport_factory_i() = default;
};

using any_transport_factory = std::unique_ptr<transport_factory_i>; // NOLINT

} // namespace http2v2
