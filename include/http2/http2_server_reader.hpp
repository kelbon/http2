
#pragma once

#include <kelcoro/task.hpp>

namespace http2 {
struct server_session;
}

namespace http2 {

// precondition: session.connection != nullptr
// 'onrequest' will be called when request fully received, it stored in node.req
// do not throws (except bad_alloc for coroutine)
// 'session' must be alive until coro ended
// returns status on fail (reqerr_e)
dd::task<int> start_server_reader_for(http2::server_session& session);

}  // namespace http2
