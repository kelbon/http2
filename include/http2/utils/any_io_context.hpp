#pragma once

#include <anyany/anyany.hpp>
#include <anyany/anyany_macro.hpp>

#include "http2/any_connection.hpp"
#include "http2/utils/any_timer.hpp"
#include "http2/asio/aio_context.hpp"
#include "http2/utils/address.hpp"
#include "http2/utils/any_acceptor.hpp"

#include <kelcoro/executor_interface.hpp>
#include <kelcoro/task.hpp>

namespace http2 {

// returns true if executes something
anyany_method2_n(poll_one_m, poll_one, (&self) requires(self.poll_one())->bool);
// returns count of executed tasks
anyany_method2_n(poll_m, poll, (&self) requires(self.poll())->size_t);
anyany_method2_n(run_m, run, (&self) requires(self.run())->size_t);
// stops run
anyany_method2_n(stop_m, stop, (&self) requires(self.stop())->void);
// stop was called
anyany_method2_n(stopped_m, stopped, (&self) requires(self.stopped())->bool);
// allows call .run / poll / poll_one again
anyany_method2_n(restart_m, restart, (&self) requires(self.restart())->void);

anyany_method2_n(running_in_this_thread_m, running_in_this_thread,
                 (&self) requires(self.running_in_this_thread())->bool);

anyany_method2_n(create_timer_m, create_timer, (&self) requires(self.create_timer())->any_timer);
// never invokes task immediately, pushes it into queue always
// may be invoked from another thread
// 'attach' for compatibility with dd::any_executor_ref
anyany_method2_n(attach_task_m, attach, (&self, dd::task_node* n) requires(self.attach(n))->void);

// creates TCP-like connection (client-side)
anyany_method2_n(create_connection_client_m, create_connection_client,
                 (&self, endpoint e, deadline_t d) requires(self.create_connection_client(e, d))
                     ->dd::task<any_connection_t>);

// creates TCP-like acceptor, which creates server-side connections in 'accept'
anyany_method2_n(create_acceptor_m, create_acceptor,
                 (&self, internet_address addr,
                  bool reuse_address) requires(self.create_acceptor(addr, reuse_address))
                     ->any_acceptor);

// TODO! block run / deblock run. Для поддержки mt_server

// movable (SooS == 0)
using any_io_context =
    aa::basic_any_with<aa::default_allocator, /*SooS=*/0, aa::type_info, create_timer_m, attach_task_m,
                       poll_one_m, poll_m, run_m, stop_m, stopped_m, restart_m, running_in_this_thread_m,
                       create_connection_client_m, create_acceptor_m>;

using any_io_context_ref = any_io_context::ref;
using any_io_context_ptr = any_io_context::ptr;

}  // namespace http2
