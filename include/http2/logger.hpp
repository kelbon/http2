
#pragma once

#include <format>
#include <iostream>

// validates FMT_STR to not be empty token, forbids LOG(, arg)
#define HTTP2_FMT_STRING_IS_EMPTY_ invalid_empty_fmt_str
#define HTTP2_FMT_STRING_IS_EMPTY_NO
#define HTTP2_CHECK_NOT_EMPTY_IMPL(TOKEN) HTTP2_FMT_STRING_IS_EMPTY_##TOKEN
#define HTTP2_CHECK_NOT_EMPTY(...) HTTP2_CHECK_NOT_EMPTY_IMPL(__VA_OPT__(NO))

#define HTTP2_DO_LOG(LEVEL, FMT_STR, ...) \
  HTTP2_CHECK_NOT_EMPTY(FMT_STR)          \
  std::cout << std::format("[" #LEVEL "][HTTP/2] " FMT_STR "\n" __VA_OPT__(, ) __VA_ARGS__)

#define HTTP2_LOG_INFO(FMT_STR, ...) HTTP2_DO_LOG(INFO, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#define HTTP2_LOG_ERROR(FMT_STR, ...) HTTP2_DO_LOG(ERROR, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#define HTTP2_LOG_WARN(FMT_STR, ...) HTTP2_DO_LOG(WARN, FMT_STR __VA_OPT__(, ) __VA_ARGS__)

#ifndef NDEBUG
  #define HTTP2_LOG_DEBUG(FMT_STR, ...) HTTP2_DO_LOG(DEBUG, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#else
  #define HTTP2_LOG_DEBUG(FMT_STR, ...) (void)0
#endif

#ifdef HTTP2_ENABLE_TRACE
  #define HTTP2_LOG_TRACE(FMT_STR, ...) HTTP2_DO_LOG(TRACE, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#else
  #define HTTP2_LOG_TRACE(FMT_STR, ...)
#endif

// always require entitiy (log name)
#define HTTP2_LOG(TYPE, STR, ...) HTTP2_LOG_##TYPE(STR " {}" __VA_OPT__(, ) __VA_ARGS__)
