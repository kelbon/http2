
add_library(http2_fuzzinglib   ${CMAKE_CURRENT_SOURCE_DIR}/any_request_template.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/emulated_client.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/fuzzer.cpp
                               ${CMAKE_CURRENT_SOURCE_DIR}/request_template.cpp)


target_include_directories(http2_fuzzinglib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(http2_fuzzinglib PUBLIC http2lib moko3lib)

set_target_properties(http2_fuzzinglib PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20)
