
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

template <typename SIGNATURE>
using move_only_fn = aa::any_with<aa::call<SIGNATURE>, aa::move>;

}  // namespace http2
