
macro(add_http2_test NAME)
    add_executable(test_${NAME} ${CMAKE_CURRENT_SOURCE_DIR}/test_${NAME}.cpp)
    target_link_libraries(test_${NAME} PUBLIC http2lib ${ARGN})

    set_target_properties(test_${NAME} PROPERTIES CXX_STANDARD 20)
    add_test(test_${NAME} test_${NAME})
endmacro()

add_http2_test(smoke_tls)
target_compile_definitions(test_smoke_tls PRIVATE HTTP2_TLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_http2_test(smoke_notls)
add_http2_test(merged_segments)
add_http2_test(fuzz http2_fuzzinglib echoserverlib)
add_http2_test(fuzz_continuations http2_fuzzinglib echoserverlib)
add_http2_test(h2spec http2_fuzzinglib echoserverlib Boost::process)
add_http2_test(connect_requests http2_fuzzinglib echoserverlib Boost::process Boost::stacktrace)
