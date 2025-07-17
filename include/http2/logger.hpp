
#pragma once

#include <format>
#include <iostream>

#define HTTP2_DO_LOG(LEVEL, FMT_STR, ...)             \
  std::format_to(std::ostreambuf_iterator(std::cout), \
                 "[" #LEVEL "] HTTP/2 " FMT_STR "\n" __VA_OPT__(, ) __VA_ARGS__)

#define HTTP2_LOG_INFO(FMT_STR, ...) HTTP2_DO_LOG(INFO, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#define HTTP2_LOG_ERROR(FMT_STR, ...) HTTP2_DO_LOG(ERROR, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#define HTTP2_LOG_WARN(FMT_STR, ...) HTTP2_DO_LOG(WARN, FMT_STR __VA_OPT__(, ) __VA_ARGS__)

#ifndef NDEBUG
  #define HTTP2_LOG_DEBUG(FMT_STR, ...) HTTP2_DO_LOG(DEBUG, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#else
  #define HTTP2_LOG_DEBUG(FMT_STR, ...) (void)0
#endif

#ifdef HTTP2_ENABLE_TRACE
  #define HTTP2_LOG_TRACE(FMT_STR, ...) HTTP2_DO_LOG(DEBUG, FMT_STR __VA_OPT__(, ) __VA_ARGS__)
#else
  #define HTTP2_LOG_TRACE(FMT_STR, ...)
#endif

#define HTTP2_LOG(TYPE, STR, ...) HTTP2_LOG_##TYPE(STR __VA_OPT__(, ) __VA_ARGS__)
