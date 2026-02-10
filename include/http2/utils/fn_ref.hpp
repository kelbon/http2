
#pragma once

#include <anyany/anyany.hpp>

namespace http2 {

template <typename SIGNATURE>
using fn_ref = aa::ref<aa::call<SIGNATURE>>;

template <typename SIGNATURE>
using fn_cref = aa::cref<aa::call<SIGNATURE>>;

template <typename SIGNATURE>
using fn_ptr = aa::ptr<aa::call<SIGNATURE>>;

template <typename SIGNATURE>
using fn_cptr = aa::cptr<aa::call<SIGNATURE>>;

// move only with SooS == 0 for less sizeof (used in h2stream)
template <typename Signature>
using move_only_fn = aa::basic_any_with<aa::default_allocator, 0, aa::call<Signature>>;

template <typename Signature>
using move_only_fn_soos = aa::any_with<aa::call<Signature>, aa::move>;

// both copy/movable
template <typename Signature>
using fn = aa::any_with<aa::call<Signature>, aa::copy>;

}  // namespace http2
