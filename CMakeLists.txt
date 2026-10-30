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
  # /EHsc 必须显式给：CMake 默认不加，Release 下 std::thread / std::function
  # 的展开会报 C4530，且异常语义不完整。
  add_compile_options(/utf-8 /EHsc /W4 /permissive- /Zc:__cplusplus)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------- portable net library
# 这一层不碰 D3D11 / WASAPI / Win32 UI，Windows 和 Linux 都能编。
# 拆出来是为了让 CI 在 Linux 上测到真实的协议 / FEC / 抖动缓冲 / 封装逻辑，
# 而不是只跑一个 `cmake --build` 的空壳。
add_library(duonet STATIC
  src/common/clock.cpp
  src/common/log.cpp
  src/net/control_channel.cpp
  src/net/packet.cpp
  src/net/packetizer.cpp
  src/net/fec.cpp
  src/net/pacer.cpp
  src/net/jitter_buffer.cpp
  src/net/transport.cpp
  src/decode/sps_parser.cpp
  src/record/mkv_muxer.cpp
  src/pipeline/analysis_tap.cpp
  src/pipeline/selftest.cpp
)

target_include_directories(duonet PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
  target_compile_definitions(duonet PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE
  )
  target_link_libraries(duonet PUBLIC ws2_32 ole32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(duonet PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------- core library
# Windows 专属：采集、编解码、呈现、音频、推流、管线。
# 整个 target 只在 Windows 下生成——非 Windows 上这些源文件连编译单元
# 都不该被创建，否则 `cmake --build` 会去碰 D3D11 头文件而失败。
if(WIN32)
  add_library(duocore STATIC
    src/capture/capture_factory.cpp
    src/capture/color_converter.cpp
    src/capture/ddapi_capture.cpp
    src/capture/wgc_capture.cpp
    src/encode/nvenc_encoder.cpp
    src/decode/nvdec_decoder.cpp
    src/decode/nvdec_cuvid.cpp
    src/present/swapchain_presenter.cpp
    src/present/telemetry_overlay.cpp
    src/audio/wasapi_loopback.cpp
    src/audio/aac_encoder.cpp
    src/audio/aac_decoder.cpp
    src/audio/wasapi_render.cpp
    src/record/stream_pusher.cpp
    src/pipeline/sender.cpp
    src/plugin/plugin_host.cpp
    src/pipeline/receiver.cpp
  )

  target_link_libraries(duocore PUBLIC duonet)

  target_include_directories(duocore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nv-codec-headers-n13.0.19.0/include/ffnvcodec
  )

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

  if(DUO_ENABLE_NVENC)
    target_compile_definitions(duocore PRIVATE DUO_ENABLE_NVENC=1)
  endif()
endif()

# ---------------------------------------------------------------- applications
# 两个程序都依赖 D3D11 / WASAPI / Win32，只在 Windows 上构建。
if(WIN32)
  add_executable(duocapd apps/duocapd/main.cpp)
  target_link_libraries(duocapd PRIVATE duocore)

  add_executable(duocapv apps/duocapv/main.cpp)
  target_link_libraries(duocapv PRIVATE duocore)
endif()

# ---------------------------------------------------------------- plugins
# 插件是独立 DLL，只依赖 plugin_api.h，不链接 duocore。
# 示例插件走 D3D11 回读，只在 Windows 上构建。
option(DUO_BUILD_EXAMPLE_PLUGINS "Build example plugins" ON)

if(WIN32 AND DUO_BUILD_EXAMPLE_PLUGINS)
  add_library(duo_plugin_luma_stats MODULE plugins/luma_stats/luma_stats.cpp)
  target_include_directories(duo_plugin_luma_stats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  set_target_properties(duo_plugin_luma_stats PROPERTIES
    PREFIX ""            # Windows 上不需要 lib 前缀
    OUTPUT_NAME "duo_plugin_luma_stats"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
  )
  if(WIN32)
    target_compile_definitions(duo_plugin_luma_stats PRIVATE
      WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
    target_link_libraries(duo_plugin_luma_stats PRIVATE d3d11 dxgi)
  endif()
endif()

# ---------------------------------------------------------------- tests
# 可移植部分：协议、FEC、抖动缓冲、SPS、分析 tap、控制通道。
# 这些在 Linux CI 上也会跑，不是只在 Windows 上验证。
set(DUO_PORTABLE_TESTS
  tests/test_main.cpp
  tests/net_tests.cpp
  tests/sps_tests.cpp
  tests/pipeline_tests.cpp
  tests/control_tests.cpp
  tests/jitter_adaptive_tests.cpp
)

if(DUO_BUILD_TESTS)
  enable_testing()
  add_executable(duo_net_tests ${DUO_PORTABLE_TESTS})
  target_link_libraries(duo_net_tests PRIVATE duonet)
  add_test(NAME duo_net_tests COMMAND duo_net_tests)

  if(WIN32)
    # 插件宿主只在 Windows 上有实现（LoadLibrary + D3D11 回读）。
    add_executable(duo_tests
      ${DUO_PORTABLE_TESTS}
      tests/plugin_tests.cpp
    )
    target_link_libraries(duo_tests PRIVATE duocore)
    add_test(NAME duo_tests COMMAND duo_tests)
  endif()
endif()
