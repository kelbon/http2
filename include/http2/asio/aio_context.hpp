#pragma once

// all used asio headers with Windows.h garbage

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>

#undef NO_ERROR
#undef Yield
#undef NO_DATA
#undef socket
#undef DELETE
