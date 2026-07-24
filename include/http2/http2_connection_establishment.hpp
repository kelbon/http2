
#pragma once

#include "http2/http2_client_options.hpp"
#include "http2/http2_connection_fwd.hpp"
#include "http2/http2_server_options.hpp"

#include <kelcoro/task.hpp>

namespace http2 {

// creates client connection with server
// accepts unestablished session 'con' and returns established connection or
// exception
dd::task<h2connection_ptr> establish_http2_session_client(h2connection_ptr con, http2_client_options options);

// creates server connection with client
// accepts unestablished session 'con' and returns established connection or
// exception
dd::task<h2connection_ptr> establish_http2_session_server(h2connection_ptr con, http2_server_options);

}  // namespace http2
