add_executable(tgbm_fake_server ${CMAKE_CURRENT_SOURCE_DIR}/fake_server.cpp)
target_link_libraries(tgbm_fake_server PUBLIC http2lib)

set_target_properties(tgbm_fake_server PROPERTIES
  CMAKE_CXX_EXTENSIONS OFF
  LINKER_LANGUAGE CXX
  CMAKE_CXX_STANDARD_REQUIRED ON
  CXX_STANDARD 20)

add_executable(tgbm_http2_notls_client ${CMAKE_CURRENT_SOURCE_DIR}/client_smoke_no_tls.cpp)
target_link_libraries(tgbm_http2_notls_client PUBLIC http2lib)

set_target_properties(tgbm_http2_notls_client PROPERTIES
  CXX_STANDARD 20)

add_test(tgbm_http2_notls_client tgbm_http2_notls_client)
