
cmake_minimum_required(VERSION 3.20)
project(http2)

option(KELHTTP2_ENABLE_TESTING  "Set to ON to enable tests"      OFF)
option(KELHTTP2_USE_CPM "Set to ON to enable use CPM for get dependencies" ON)
option(KELHTTP2_ENABLE_SANITIZERS "build with sanitizers" OFF)

if (KELHTTP2_ENABLE_SANITIZERS)
  # enable sanitize flags globally, all targets require to be compiled with them

  # fno omit frame pointer to improve trace from sanitizers
  add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address -fsanitize=undefined)

endif()

if (KELHTTP2_USE_CPM)
  include(cmake/deps.cmake)
endif()

add_library(http2lib ${CMAKE_CURRENT_SOURCE_DIR}/src/http_base.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_client.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_protocol.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_connection.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_connection_establishment.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_send_frames.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_server_reader.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_writer.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_server_session.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_server.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/timer.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/unique_name.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/asio/factory.cpp
                  ${CMAKE_CURRENT_SOURCE_DIR}/src/asio/ssl_context.cpp
            )

target_include_directories(http2lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(http2lib PUBLIC
    ${CMAKE_THREAD_LIBS_INIT}
    ssl crypto
    anyanylib
    kelcorolib
    hpacklib
    zallib
    strswitchlib
    Boost::intrusive
    Boost::smart_ptr
    Boost::asio
)

set_target_properties(http2lib PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20)

if (WIN32)
    if (NOT DEFINED HTTP2_WINDOWS_VERSION) 
        set(HTTP2_WINDOWS_VERSION 0x0A00 CACHE STRING "windows version for boost asio")
    endif()
    target_compile_definitions(http2lib PUBLIC _WIN32_WINNT=${HTTP2_WINDOWS_VERSION})
endif()

if (KELHTTP2_ENABLE_TESTING)
    include(CTest)
    add_subdirectory(tests)
    add_subdirectory(tests/servers)
endif()

add_library(http2_fuzzinglib   ${CMAKE_CURRENT_SOURCE_DIR}/src/fuzzing/any_request_template.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/fuzzing/emulated_client.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/fuzzing/fuzzer.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/src/fuzzing/request_template.cpp)

target_include_directories(http2_fuzzinglib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(http2_fuzzinglib PUBLIC http2lib)

set_target_properties(http2_fuzzinglib PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20)

include(cmake/visit_targets.cmake)

generate_launch_json_file()
