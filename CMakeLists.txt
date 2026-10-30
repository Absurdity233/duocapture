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
  src/capture/capture_factory.cpp
  src/capture/color_converter.cpp
  src/capture/ddapi_capture.cpp
  src/capture/wgc_capture.cpp
  src/encode/nvenc_encoder.cpp
  src/decode/nvdec_decoder.cpp
  src/decode/sps_parser.cpp
  src/present/swapchain_presenter.cpp
  src/pipeline/sender.cpp
  src/pipeline/receiver.cpp
  src/pipeline/selftest.cpp
)

target_include_directories(duocore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers-n13.0.19.0/include/ffnvcodec
)

if(WIN32)
  target_compile_definitions(duocore PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE
    DUO_HAVE_D3D11=1
  )
  # C++/WinRT 投影随 Windows SDK 提供，但不在默认 include 搜索路径里。
  file(GLOB DUO_WINSDK_INCLUDE_DIRS "C:/Program Files (x86)/Windows Kits/10/Include/10.*")
  list(SORT DUO_WINSDK_INCLUDE_DIRS)
  list(LENGTH DUO_WINSDK_INCLUDE_DIRS DUO_WINSDK_COUNT)

  if(DUO_WINSDK_COUNT GREATER 0)
    math(EXPR DUO_WINSDK_LAST "${DUO_WINSDK_COUNT} - 1")
    list(GET DUO_WINSDK_INCLUDE_DIRS ${DUO_WINSDK_LAST} DUO_WINSDK_LATEST)
    set(DUO_CPPWINRT_INCLUDE "${DUO_WINSDK_LATEST}/cppwinrt")
    if(EXISTS "${DUO_CPPWINRT_INCLUDE}")
      target_include_directories(duocore PUBLIC "${DUO_CPPWINRT_INCLUDE}")
      message(STATUS "C++/WinRT: ${DUO_CPPWINRT_INCLUDE}")
    else()
      message(WARNING "未找到 C++/WinRT 投影，WGC 后端将不可用")
    endif()
  else()
    message(WARNING "未找到 Windows SDK，WGC 后端将不可用")
  endif()

  target_link_libraries(duocore PUBLIC
    ws2_32 d3d11 dxgi d3dcompiler mfplat mfuuid mfreadwrite mf mfcore ole32
    windowsapp
  )
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
    tests/sps_tests.cpp
  )
  target_link_libraries(duo_tests PRIVATE duocore)
  add_test(NAME duo_tests COMMAND duo_tests)
endif()
