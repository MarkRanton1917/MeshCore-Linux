cmake_minimum_required(VERSION 3.16)
project(MeshCore LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)          # std::span and std::optional in the headers
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Off by default; -DSANITIZE=ON for the ASan/UBSan run.
option(SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

add_library(meshcore
  src/packet/packet.cpp
  src/crypto/core.cpp
  src/crypto/protocol.cpp
  src/crypto/aes128.c
  src/identity/identity.cpp
  src/routing/routing.cpp
  src/room/room.cpp
  src/platform/platform.cpp
  src/radio/radio.cpp
  src/radio/udp_radio.cpp
  src/telemetry/telemetry.cpp
)

target_include_directories(meshcore PUBLIC src src/packet src/crypto src/identity src/routing src/room src/platform src/radio src/telemetry)
target_link_libraries(meshcore PUBLIC PkgConfig::SODIUM)
target_compile_options(meshcore PRIVATE -Wall -Wextra)

if(SANITIZE)
  target_compile_options(meshcore PUBLIC -fsanitize=address,undefined -g)
  target_link_options(meshcore PUBLIC -fsanitize=address,undefined)
endif()

add_executable(meshcore-node src/main.cpp)
target_link_libraries(meshcore-node PRIVATE meshcore)
target_compile_options(meshcore-node PRIVATE -Wall -Wextra)

enable_testing()

foreach(suite test_packet test_crypto test_identity test_routing test_room test_platform test_radio test_telemetry)
  add_executable(${suite} test/${suite}.cpp)
  target_include_directories(${suite} PRIVATE test)
  target_link_libraries(${suite} PRIVATE meshcore)
  target_compile_options(${suite} PRIVATE -Wall -Wextra)

  # test_crypto writes key files, so give both suites a scratch directory.
  add_test(NAME ${suite} COMMAND ${suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
