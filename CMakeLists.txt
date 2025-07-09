
cmake_minimum_required(VERSION 3.20)
project(http2_v2)

add_library(http2_v2    ${CMAKE_CURRENT_SOURCE_DIR}/src/http_client.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/seastar_transport.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_client.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_protocol.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_connection.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_connection_establishment.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_send_frames.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_server_reader.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_writer.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/http2_server_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/req_cast.cpp)

target_include_directories(http2_v2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(http2_v2 PUBLIC
    ${CMAKE_THREAD_LIBS_INIT}
    ssl crypto
    anyanylib
    kelcorolib
    hpacklib
    Fmt::fmt
    utils
    seastar_utils
)
