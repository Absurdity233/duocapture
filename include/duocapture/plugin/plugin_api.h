#pragma once

// DuoCapture 插件 ABI。
//
// 这个头文件同时被主程序和插件包含，所以必须是纯 C 的：
//   * 不出现任何 STL 类型（std::string / std::vector 跨 DLL 边界是灾难）
//   * 不出现虚函数（MSVC 的虚表布局不保证跨编译器/跨版本稳定）
//   * 不抛异常（异常不能跨越 DLL 边界）
//   * 所有字符串都是 UTF-8 的 const char*，由插件持有，生命周期与插件相同
//
// 兼容性策略：用 abi_version + struct_size 双重校验。
//   * abi_version 变了说明有不兼容改动，主程序直接拒绝加载
//   * struct_size 变小说明插件比主程序旧，仍然可以加载（只读已存在的字段）
//   * struct_size 变大说明插件比主程序新，主程序按自己的大小读，安全

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define DUO_PLUGIN_EXPORT __declspec(dllexport)
#else
#define DUO_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// 当前 ABI 版本。任何破坏性改动都要 +1。
#define DUO_PLUGIN_ABI_VERSION 1u

// 插件入口函数名。每个插件 DLL 必须导出这个符号。
#define DUO_PLUGIN_ENTRY_NAME "duo_plugin_entry"

// ---- 基础类型 --------------------------------------------------------------

typedef enum DuoPluginKind {
  DUO_PLUGIN_KIND_UNKNOWN = 0,
  DUO_PLUGIN_KIND_ANALYSIS = 1,  // 拿到解码后的帧做分析
  DUO_PLUGIN_KIND_OUTPUT = 2,    // 额外的输出后端（预留）
} DuoPluginKind;

typedef enum DuoLogLevel {
  DUO_LOG_TRACE = 0,
  DUO_LOG_DEBUG = 1,
  DUO_LOG_INFO = 2,
  DUO_LOG_WARN = 3,
  DUO_LOG_ERROR = 4,
} DuoLogLevel;

// 传给插件的一帧视图。所有字段只读，插件不得修改。
//
// texture 是 ID3D11Texture2D*（DXGI_FORMAT_NV12）。插件必须把它当作
// 不透明指针，转成自己的类型后使用。纹理由解码器持有，**回调返回后
// 随时可能被复用**，插件不能把指针存起来异步用。
typedef struct DuoFrameView {
  void* texture;
  unsigned int width;
  unsigned int height;
  unsigned long long capture_ts_ns;
  int key;
} DuoFrameView;

// 主程序提供给插件的服务。
typedef struct DuoPluginHostServices {
  unsigned int abi_version;
  unsigned int struct_size;

  // 写日志。message 是 UTF-8，插件不需要管理它的生命周期。
  void (*log)(int level, const char* message);

  // D3D11 设备，插件如果要自己跑 GPU 推理（TensorRT / DirectML）需要它。
  // 这两个指针由主程序持有，插件不得 Release。
  void* d3d_device;
  void* d3d_context;

  // 让插件请求主程序做某件事（预留，当前未使用）。
  void* reserved[4];
} DuoPluginHostServices;

// ---- 分析插件 --------------------------------------------------------------

// 插件上报的一个检测框（像素坐标，相对于**原始解码帧**）。
//
// 主程序把它合成到呈现表面，所以插件只负责算坐标，不碰纹理——
// 分析线程和呈现线程共用同一张解码纹理，插件直接往上画会和呈现打架。
typedef struct DuoOverlayBox {
  float x1, y1, x2, y2;  // 原始帧像素坐标
  float score;           // 0..1
  int class_id;
  unsigned int color;    // 0xAARRGGBB；0 = 让宿主按类别自动配色
  unsigned int reserved;
} DuoOverlayBox;

// 分析插件的函数表。instance 是 create 返回的不透明指针。
typedef struct DuoAnalysisPlugin {
  unsigned int abi_version;
  unsigned int struct_size;

  void* instance;

  // 每帧回调，在**分析线程**里调用。
  //
  // 返回值：>= 0 表示成功（可用于回传自定义计数），< 0 表示这一帧处理失败。
  // 主程序不会因为返回负值就卸载插件，只是计入错误统计。
  //
  // 重要：这个回调慢不会影响呈现——主程序用「忙则丢帧」策略，
  // 回调期间新帧会被覆盖丢弃。所以插件可以放心做几十毫秒的计算。
  int (*on_frame)(void* instance, const DuoFrameView* frame);

  // 流重置（切换分辨率、断线重连）时调用，插件应清空内部状态。
  void (*on_reset)(void* instance);

  // 销毁实例。主程序卸载插件前一定会调用。
  void (*destroy)(void* instance);

  // ---- 以下为 ABI 1 之后新增的字段 ------------------------------------
  //
  // 靠 struct_size 做单向校验：老插件 struct_size 小，主程序不会读这些
  // 字段；新插件在大 struct 上填好它们，老主程序读不到但也不会崩。
  // 因此加这些字段不需要升 DUO_PLUGIN_ABI_VERSION。

  // 取本帧要叠加的检测框。返回写入 out_boxes 的个数（最多 max_boxes），
  // 返回 0 表示这一帧不画。在**呈现线程**里调用，允许为 NULL。
  //
  // 指针只在本次调用内有效，主程序不会保存。
  int (*get_overlay_boxes)(void* instance, DuoOverlayBox* out_boxes, int max_boxes);
} DuoAnalysisPlugin;

// ---- 插件描述符 ------------------------------------------------------------

// 插件导出的描述符。由 duo_plugin_entry 返回，生命周期与 DLL 相同，
// 主程序不负责释放。
typedef struct DuoPlugin {
  unsigned int abi_version;
  unsigned int struct_size;

  DuoPluginKind kind;
  const char* name;         // 唯一标识，建议用 "vendor.plugin_name"
  const char* version;      // 语义化版本
  const char* description;  // 一句话说明

  // 创建实例。config 是 UTF-8 的 JSON 或空字符串，由插件自行解析。
  // 成功返回 0 并通过 out_instance 回传实例指针。
  int (*create)(const DuoPluginHostServices* host, const char* config, void** out_instance);

  // 拿到分析函数表。kind != ANALYSIS 时返回 NULL。
  // 返回的指针生命周期与 instance 相同。
  const DuoAnalysisPlugin* (*as_analysis)(void* instance);

  // 销毁实例。与 create 配对。
  void (*destroy)(void* instance);
} DuoPlugin;

// ---- 插件必须导出的入口 ----------------------------------------------------

// 主程序调用这个函数拿描述符。host_abi_version 是主程序支持的版本。
// 插件可以据此决定返回哪个版本的描述符，或者返回 NULL 表示不兼容。
typedef const DuoPlugin* (*DuoPluginEntryFn)(unsigned int host_abi_version);

#ifdef __cplusplus
}  // extern "C"
#endif
