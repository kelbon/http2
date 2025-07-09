
#pragma once

#include <anyany/anyany.hpp>

namespace http2v2 {

template <typename SIGNATURE>
using fn_ref = aa::ref<aa::call<SIGNATURE>>; // NOLINT

template <typename SIGNATURE>
using fn_cref = aa::cref<aa::call<SIGNATURE>>; // NOLINT

template <typename SIGNATURE>
using fn_ptr = aa::ptr<aa::call<SIGNATURE>>; // NOLINT

template <typename SIGNATURE>
using fn_cptr = aa::cptr<aa::call<SIGNATURE>>; // NOLINT

template <typename SIGNATURE>
using move_only_fn = aa::any_with<aa::call<SIGNATURE>, aa::move>; // NOLINT

} // namespace http2v2
