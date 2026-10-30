# 插件开发

DuoCapture 支持把**分析逻辑**做成独立 DLL，不用改主程序、不用重新编译。

## 为什么这么设计

主程序的 `AnalysisTap` 已经在做「忙则丢帧、绝不阻塞解码」的隔离。插件只是把这个
回调的落点换成一个动态库。所以插件可以放心做几十毫秒的计算——慢的代价是丢帧，
不是卡画面。

## ABI 约束（重要）

`include/duocapture/plugin/plugin_api.h` 是**纯 C 头文件**，必须保持这样：

| 禁止 | 原因 |
|---|---|
| STL 类型跨边界（`std::string` / `std::vector`） | 各模块的堆和布局不保证一致，会双重释放或踩内存 |
| 虚函数 / 类继承 | MSVC 虚表布局不保证跨编译器、跨版本稳定 |
| 抛异常跨边界 | C++ 异常无法安全跨越 DLL 边界 |
| 返回内部指针给宿主长期持有 | 插件卸载后指针悬空 |

插件内部**可以用任意 C++**，只要不越过 ABI 边界。

## 版本协商

每个描述符头两个字段固定是 `abi_version` + `struct_size`：

```c
if (descriptor->abi_version != DUO_PLUGIN_ABI_VERSION) -> 拒绝加载
if (descriptor->struct_size < sizeof(DuoPlugin))         -> 拒绝加载
```

`struct_size` 只做「不能比主程序认识的小」的单向校验——这样主程序升级加了字段，
老插件仍能加载（新字段主程序自己填默认值）。

**任何破坏性改动都必须把 `DUO_PLUGIN_ABI_VERSION` +1。**

## 最小插件

```cpp
#include "duocapture/plugin/plugin_api.h"

static int on_frame(void* instance, const DuoFrameView* frame) {
    // frame->texture 是 ID3D11Texture2D*（NV12）
    return 0;  // >= 0 成功
}

static const DuoAnalysisPlugin kApi = {
    DUO_PLUGIN_ABI_VERSION, sizeof(DuoAnalysisPlugin),
    nullptr, &on_frame, nullptr, nullptr,
};

static int create(const DuoPluginHostServices* host, const char* config, void** out) {
    *out = nullptr;  // 无状态插件可以返回空
    return 0;
}

static const DuoAnalysisPlugin* as_analysis(void*) { return &kApi; }
static void destroy(void*) {}

static const DuoPlugin kPlugin = {
    DUO_PLUGIN_ABI_VERSION, sizeof(DuoPlugin),
    DUO_PLUGIN_KIND_ANALYSIS,
    "vendor.my_plugin", "1.0.0", "一句话说明",
    &create, &as_analysis, &destroy,
};

extern "C" DUO_PLUGIN_EXPORT const DuoPlugin* duo_plugin_entry(unsigned int host_abi) {
    return host_abi == DUO_PLUGIN_ABI_VERSION ? &kPlugin : nullptr;
}
```

完整可运行示例见 `plugins/luma_stats/`（统计平均亮度、检测黑帧）。

## 帧数据的生命周期

`DuoFrameView::texture` 指向解码器持有的 NV12 纹理。

**回调返回后纹理随时可能被复用。** 插件必须：

- 在 `on_frame` 内同步处理完
- 或者立刻 `CopyResource` 到自己的 staging 纹理再异步用

绝不能把 `texture` 指针存下来等回调返回后再访问。

## 拿到 D3D 设备

`DuoPluginHostServices::d3d_device` / `d3d_context` 由主程序提供，用于：

- GPU 推理（TensorRT / DirectML）
- 回读纹理（`CopyResource` + `Map`）

注意这两个指针**主程序持有，插件不得 Release**。

时序：插件加载发生在呈现窗口建好之前，所以 `create()` 时这两个字段可能还是
`nullptr`。接收端在窗口就绪后会调用 `set_d3d_device` 更新——但那是宿主内部的
状态，插件拿到的 `DuoPluginHostServices*` 是同一个结构体，字段会被就地更新。
**建议在 `on_frame` 里每次检查非空，而不是在 `create` 时缓存。**

## 加载

```bash
duocapv --play --plugin-dir build/plugins
duocapv --play --plugin-dir build/plugins --plugin duo.luma_stats --plugin-config "step=8,log=20"
```

**安全提示：加载 DLL 就是执行任意代码。** 主程序不会自动扫描当前目录或系统目录，
必须显式指定 `--plugin-dir`。这个目录里只应放可信的插件。

## 调试

插件日志通过 `host->log(level, message)` 输出，会带 `[plugin]` 前缀进主程序日志：

```
[INFO ] plugin_host.cpp:85 [plugin] luma_stats: 第 20 帧，平均亮度 52，黑帧 0
```

加载失败会明确说明原因（缺入口、ABI 不匹配、结构体过小等）。
