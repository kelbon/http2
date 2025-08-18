add_executable(test_http2_tls_smoke ${CMAKE_CURRENT_SOURCE_DIR}/test_smoke_tls.cpp)
target_link_libraries(test_http2_tls_smoke PUBLIC http2lib)

target_compile_definitions(test_http2_tls_smoke PRIVATE HTTP2_TLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(test_http2_tls_smoke PROPERTIES CXX_STANDARD 20)

add_test(test_http2_tls_smoke test_http2_tls_smoke)

add_executable(test_http2_notls_smoke ${CMAKE_CURRENT_SOURCE_DIR}/test_smoke_notls.cpp)
target_link_libraries(test_http2_notls_smoke PUBLIC http2lib)

set_target_properties(test_http2_notls_smoke PROPERTIES CXX_STANDARD 20)

add_test(test_http2_notls_smoke test_http2_notls_smoke)

add_executable(test_merged_segments ${CMAKE_CURRENT_SOURCE_DIR}/test_merged_segments.cpp)
target_link_libraries(test_merged_segments PUBLIC http2lib)

set_target_properties(test_merged_segments PROPERTIES CXX_STANDARD 20)

add_test(test_merged_segments test_merged_segments)

add_executable(test_fuzz ${CMAKE_CURRENT_SOURCE_DIR}/test_fuzz.cpp)
target_link_libraries(test_fuzz PUBLIC http2_fuzzinglib echoserverlib)

set_target_properties(test_fuzz PROPERTIES CXX_STANDARD 20)

add_test(test_fuzz test_fuzz)

add_executable(test_fuzz_continuations ${CMAKE_CURRENT_SOURCE_DIR}/test_fuzz_continuations.cpp)
target_link_libraries(test_fuzz_continuations PUBLIC http2_fuzzinglib echoserverlib)

set_target_properties(test_fuzz_continuations PROPERTIES CXX_STANDARD 20)

add_test(test_fuzz_continuations test_fuzz_continuations)
