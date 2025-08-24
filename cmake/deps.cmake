
include(cmake/get_cpm.cmake)

CPMAddPackage(
  NAME OpenSSL
  GITHUB_REPOSITORY janbar/openssl-cmake
  GIT_TAG 1.1.1w-20231130
  OPTIONS "WITH_APPS OFF"
  EXCLUDE_FROM_ALL YES
  SYSTEM OFF
)

CPMAddPackage(
  NAME kelcoro
  GIT_REPOSITORY https://github.com/kelbon/kelcoro
  GIT_TAG origin/main
  OPTIONS "KELCORO_ENABLE_TESTING OFF"
)

CPMAddPackage(
  NAME AnyAny
  GIT_REPOSITORY https://github.com/kelbon/AnyAny
  GIT_TAG origin/main
  OPTIONS "AA_ENABLE_TESTING OFF"
)

# BEFORE hpack, so dependency with name 'Boost' will contain
# both intrusive required for HPACK and other deps, required for HTTP2
set(BOOST_INCLUDE_LIBRARIES intrusive system smart_ptr asio process stacktrace)
CPMAddPackage(
  NAME Boost
  VERSION 1.87.0
  URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz
  OPTIONS "BOOST_ENABLE_CMAKE ON"
)
unset(BOOST_INCLUDE_LIBRARIES)

CPMAddPackage(
  NAME HPACK
  GIT_REPOSITORY https://github.com/kelbon/HPACK
  GIT_TAG        origin/master
  OPTIONS "HPACK_ENABLE_TESTING OFF"
)

CPMAddPackage(
  NAME LOGIC_GUARDS
  GIT_REPOSITORY https://github.com/kelbon/logic_guards
  GIT_TAG        origin/master
  OPTIONS "ZAL_ENABLE_TESTING OFF"
)

find_package(Threads REQUIRED)

CPMAddPackage(
  NAME STRSWITCH
  GIT_REPOSITORY https://github.com/kelbon/strswitch
  GIT_TAG        v1.0
  OPTIONS "STRSWITCH_ENABLE_TESTING OFF"
)
