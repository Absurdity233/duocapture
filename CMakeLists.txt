cmake_minimum_required(VERSION 3.20)
project(duocapture VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DUO_BUILD_TESTS "Build unit tests" ON)
option(DUO_ENABLE_NVENC "Build NVENC encoder (requires NVIDIA Video Codec SDK)" OFF)

# 编译选项必须在创建任何目标之前设置。
if(MSVC)
  # 源码含中文注释，必须全局 /utf-8，否则按 GBK 解码会吞掉后续行。
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------- core library
add_library(duocore STATIC
  src/common/clock.cpp
  src/common/log.cpp
  src/net/packet.cpp
  src/net/packetizer.cpp
  src/net/fec.cpp
  src/net/pacer.cpp
  src/net/jitter_buffer.cpp
  src/net/transport.cpp
  src/capture/ddapi_capture.cpp
  src/encode/nvenc_encoder.cpp
  src/decode/nvdec_decoder.cpp
  src/present/swapchain_presenter.cpp
  src/pipeline/selftest.cpp
)

target_include_directories(duocore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
  target_compile_definitions(duocore PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE
  )
  target_link_libraries(duocore PUBLIC ws2_32 d3d11 dxgi)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(duocore PUBLIC Threads::Threads)
endif()

if(DUO_ENABLE_NVENC)
  target_compile_definitions(duocore PRIVATE DUO_ENABLE_NVENC=1)
endif()

# ---------------------------------------------------------------- applications
add_executable(duocapd apps/duocapd/main.cpp)
target_link_libraries(duocapd PRIVATE duocore)

add_executable(duocapv apps/duocapv/main.cpp)
target_link_libraries(duocapv PRIVATE duocore)

# ---------------------------------------------------------------- tests
if(DUO_BUILD_TESTS)
  enable_testing()
  add_executable(duo_tests
    tests/test_main.cpp
    tests/net_tests.cpp
  )
  target_link_libraries(duo_tests PRIVATE duocore)
  add_test(NAME duo_tests COMMAND duo_tests)
endif()
