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

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
add_subdirectory(lib/json EXCLUDE_FROM_ALL)

add_subdirectory(lib/radiolib EXCLUDE_FROM_ALL)

# Only the SX126x driver is needed; the rest is compiled out. PUBLIC because the
# RadioLib headers are guarded by these same macros — a consumer that did not see
# them would compile against a different library than the one it links against.
target_compile_definitions(RadioLib PUBLIC
  RADIOLIB_EXCLUDE_CC1101=1
  RADIOLIB_EXCLUDE_NRF24=1
  RADIOLIB_EXCLUDE_RF69=1
  RADIOLIB_EXCLUDE_SX1231=1
  RADIOLIB_EXCLUDE_SI443X=1
  RADIOLIB_EXCLUDE_RFM2X=1
  RADIOLIB_EXCLUDE_SX127X=1
  # BuildOpt.h defines this one itself as (1) off-STM32 and does not guard it, so
  # the value has to match token for token or every translation unit warns.
  "RADIOLIB_EXCLUDE_STM32WLX=(1)"
  RADIOLIB_EXCLUDE_SX128X=1
  RADIOLIB_EXCLUDE_AFSK=1
  RADIOLIB_EXCLUDE_AX25=1
  RADIOLIB_EXCLUDE_HELLSCHREIBER=1
  RADIOLIB_EXCLUDE_MORSE=1
  RADIOLIB_EXCLUDE_RTTY=1
  RADIOLIB_EXCLUDE_SSTV=1
  RADIOLIB_EXCLUDE_DIRECT_RECEIVE=1
  RADIOLIB_EXCLUDE_BELL=1
  RADIOLIB_EXCLUDE_APRS=1
  RADIOLIB_EXCLUDE_LORAWAN=1
  RADIOLIB_EXCLUDE_LR11X0=1
  RADIOLIB_EXCLUDE_FSK4=1
  RADIOLIB_EXCLUDE_PAGER=1
)

add_library(meshcore
  src/packet/packet.cpp
  src/crypto/core.cpp
  src/crypto/protocol.cpp
  src/crypto/aes128.c
  src/identity/identity.cpp
  src/routing/routing.cpp
  src/repeater/repeater.cpp
  src/room/room.cpp
  src/platform/platform.cpp
  src/radio/radio.cpp
  src/radio/udp_radio.cpp
  src/telemetry/telemetry.cpp
)

target_include_directories(meshcore PUBLIC src src/packet src/crypto src/identity src/routing src/repeater src/room src/platform src/radio src/telemetry)
target_link_libraries(meshcore PUBLIC PkgConfig::SODIUM PRIVATE nlohmann_json::nlohmann_json)
target_compile_options(meshcore PRIVATE -Wall -Wextra)

if(SANITIZE)
  target_compile_options(meshcore PUBLIC -fsanitize=address,undefined -g)
  target_link_options(meshcore PUBLIC -fsanitize=address,undefined)
endif()

add_executable(meshcore-node src/main.cpp)
target_link_libraries(meshcore-node PRIVATE meshcore)
target_compile_options(meshcore-node PRIVATE -Wall -Wextra)

enable_testing()

foreach(suite test_packet test_crypto test_identity test_routing test_repeater test_room test_platform test_radio test_telemetry)
  add_executable(${suite} test/${suite}.cpp)
  target_include_directories(${suite} PRIVATE test)
  target_link_libraries(${suite} PRIVATE meshcore)
  target_compile_options(${suite} PRIVATE -Wall -Wextra)

  # test_crypto writes key files, so give both suites a scratch directory.
  add_test(NAME ${suite} COMMAND ${suite} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
