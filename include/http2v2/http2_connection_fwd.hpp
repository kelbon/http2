
#pragma once

#include "http2v2/utils/fn_ref.hpp"
#include "http2v2/utils/memory.hpp"

#include <span>
#include <string_view>

#include <boost/intrusive_ptr.hpp>

namespace http2v2 {

struct request_node;

void intrusive_ptr_add_ref(request_node *p) noexcept;
void intrusive_ptr_release(request_node *p) noexcept;

// TODE rename (stream_ptr)
using node_ptr = boost::intrusive_ptr<request_node>; // NOLINT

struct http2_connection;

void intrusive_ptr_add_ref(http2_connection *) noexcept;
void intrusive_ptr_release(http2_connection *) noexcept;

using http2_connection_ptr_t = boost::intrusive_ptr<http2_connection>;

using on_header_fn_ptr =
    fn_ptr<void(std::string_view name, std::string_view value)>; // NOLINT

using on_data_part_fn_ptr =
    fn_ptr<void(std::span<byte_t const> bytes, bool lastPart)>; // NOLINT

} // namespace http2v2
