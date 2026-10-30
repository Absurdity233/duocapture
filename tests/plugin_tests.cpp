// 插件系统的回归测试。
//
// 插件系统的风险集中在 ABI 边界上，这些错误在运行时才暴露、且症状怪异：
//   * 版本号不匹配没拦住 → 读到越界字段，随机崩溃
//   * struct_size 校验方向反了 → 老插件被误拒 / 新插件读越界
//   * 目录里混入非插件 DLL → 整个扫描中断
#include <string>
#include <vector>

#include "duocapture/common/types.h"
#include "duocapture/plugin/plugin_api.h"
#include "duocapture/plugin/plugin_host.h"
#include "testfw.h"

DUO_TEST(plugin_api_struct_sizes_are_stable) {
  // 这些大小是 ABI 的一部分。改动它们必须同时升 ABI 版本，
  // 否则老插件会按旧布局读写，直接踩内存。
  // void*(8) + u32(4) + u32(4) + u64(8) + int(4) + padding(4) = 32
  DUO_CHECK_EQ(sizeof(DuoFrameView), static_cast<std::size_t>(32));
  DUO_CHECK_EQ(DUO_PLUGIN_ABI_VERSION, 1u);

  // 头两个字段必须是 abi_version + struct_size，所有描述符都靠它们做校验
  DuoPlugin p{};
  DUO_CHECK_EQ(offsetof(DuoPlugin, abi_version), static_cast<std::size_t>(0));
  DUO_CHECK_EQ(offsetof(DuoPlugin, struct_size), sizeof(unsigned int));
}

DUO_TEST(plugin_host_rejects_missing_directory) {
  duo::PluginHost host;
  host.initialize(nullptr, nullptr);
  // 不存在的目录应该安静地返回 0，而不是崩溃
  const std::size_t n = host.load_directory("Z:\\definitely_not_here_12345");
  DUO_CHECK_EQ(n, static_cast<std::size_t>(0));
  DUO_CHECK(host.plugins().empty());
}

DUO_TEST(plugin_host_rejects_non_plugin_file) {
  duo::PluginHost host;
  host.initialize(nullptr, nullptr);
  // 传一个肯定不是插件的文件（这里是源码文件本身）
  const duo::Status st = host.load("tests/plugin_tests.cpp");
  DUO_CHECK(!duo::ok(st));
  // 失败也要被记录下来，方便 UI 显示「为什么没加载」
  DUO_CHECK_EQ(host.plugins().size(), static_cast<std::size_t>(1));
  if (!host.plugins().empty()) {
    DUO_CHECK(!host.plugins()[0].compatible);
    DUO_CHECK(!host.plugins()[0].error.empty());
  }
}

DUO_TEST(plugin_host_create_analysis_returns_null_for_unknown) {
  duo::PluginHost host;
  host.initialize(nullptr, nullptr);
  DUO_CHECK(host.create_analysis("no.such.plugin") == nullptr);
  DUO_CHECK(host.create_first_analysis() == nullptr);
}

DUO_TEST(plugin_kind_enum_values_are_stable) {
  // 这些值会写进插件描述符，改动会破坏二进制兼容
  DUO_CHECK_EQ(static_cast<int>(DUO_PLUGIN_KIND_UNKNOWN), 0);
  DUO_CHECK_EQ(static_cast<int>(DUO_PLUGIN_KIND_ANALYSIS), 1);
  DUO_CHECK_EQ(static_cast<int>(DUO_PLUGIN_KIND_OUTPUT), 2);
}
