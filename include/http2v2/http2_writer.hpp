
#pragma once

#include "http2v2/errors.hpp"
#include "http2v2/http2_connection_fwd.hpp"
#include "http2v2/utils/deadline.hpp"
#include "http2v2/utils/fn_ref.hpp"

#include <kelcoro/job.hpp>
#include <kelcoro/task.hpp>
#include <seastar/core/gate.hh>

namespace http2v2 {

using writer_sleepcb_t =
    move_only_fn<dd::task<void>(duration_t, io_error_code &)>;
using writer_on_network_err_t = move_only_fn<void() noexcept>;

// creates writer associated with connection.
// Writer uses con->writer when awaiting new job
// con->requests is a writer job. Writer grabs nodes from 'con->requests' one by
// one and writes them into 'con'. handles control flow on sending side Note:
// when server uses this function, pseudoheaders in requests are ignored and
// 'status' from node used as required pseudoheader
//
// 'sleepcb' used when writer sleeps until its possible to write (control flow)
// 'onnetworkerr' used when network error happens (before closing writer)
// 'forcedisablehpack' forces writer to update HPACK dynamic table to zero size
// when first request sent
//
// precondition: con && sleepcb  && onnetworkerr. Callbacks should not throw and
// behave as values, not references
dd::job start_writer_for_client(http2_connection_ptr_t con, writer_sleepcb_t,
                                writer_on_network_err_t, bool forcedisablehpack,
                                seastar::gate::holder);

dd::job start_writer_for_server(http2_connection_ptr_t con, writer_sleepcb_t,
                                writer_on_network_err_t, bool forcedisablehpack,
                                seastar::gate::holder);

} // namespace http2v2
