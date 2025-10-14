
#pragma once

#include "http2/utils/fn_ref.hpp"
#include "http2/utils/memory.hpp"

#include <span>
#include <string_view>

#include <boost/intrusive_ptr.hpp>

namespace http2 {

struct h2stream;

void intrusive_ptr_add_ref(h2stream* p) noexcept;
void intrusive_ptr_release(h2stream* p) noexcept;

using stream_ptr = boost::intrusive_ptr<h2stream>;

struct http2_connection;

void intrusive_ptr_add_ref(http2_connection*) noexcept;
void intrusive_ptr_release(http2_connection*) noexcept;

using http2_connection_ptr_t = boost::intrusive_ptr<http2_connection>;

using on_header_fn_ptr = fn_ptr<void(std::string_view name, std::string_view value)>;

using on_data_part_fn_ptr = fn_ptr<void(std::span<byte_t const> bytes, bool lastPart)>;

}  // namespace http2
