
add_library(echoserverlib ${CMAKE_CURRENT_SOURCE_DIR}/echo_server.cpp)

target_include_directories(echoserverlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(echoserverlib PUBLIC http2lib)

set_target_properties(echoserverlib PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20)

add_executable(h2spec_server ${CMAKE_CURRENT_SOURCE_DIR}/h2spec_server.cpp)

target_link_libraries(h2spec_server PUBLIC http2lib)

set_target_properties(h2spec_server PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20)

add_executable(benchmark_server ${CMAKE_CURRENT_SOURCE_DIR}/bench_server.cpp)
target_link_libraries(benchmark_server PUBLIC http2lib)

set_target_properties(benchmark_server PROPERTIES
  CMAKE_CXX_EXTENSIONS OFF
  LINKER_LANGUAGE CXX
  CMAKE_CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20)

add_executable(benchmark_server_tls ${CMAKE_CURRENT_SOURCE_DIR}/bench_server_tls.cpp)
target_link_libraries(benchmark_server_tls PUBLIC http2lib)

set_target_properties(benchmark_server_tls PROPERTIES
  CMAKE_CXX_EXTENSIONS OFF
  LINKER_LANGUAGE CXX
  CMAKE_CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20)
target_compile_definitions(benchmark_server_tls PRIVATE HTTP2_TLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
