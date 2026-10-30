#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "duocapture/common/status.h"
#include "duocapture/common/types.h"
#include "duocapture/decode/decoder.h"
#include "duocapture/plugin/plugin_api.h"

namespace duo {

// 已加载插件的信息快照（给 UI / 日志用）。
struct PluginInfo {
  std::string path;
  std::string name;
  std::string version;
  std::string description;
  DuoPluginKind kind = DUO_PLUGIN_KIND_UNKNOWN;
  bool compatible = false;
  std::string error;  // 不兼容或加载失败时的原因
};

// 插件宿主：负责扫描、加载、校验、卸载插件 DLL。
//
// 安全提示：加载 DLL 就是执行任意代码。所以这里**不做自动加载**——
// 必须由用户显式指定目录（`--plugin-dir`），程序不会去扫当前目录或
// 系统目录。目录里的每个 DLL 仍然会被加载，所以这个目录应当只放
// 可信的插件。
class PluginHost {
 public:
  PluginHost();
  ~PluginHost();

  PluginHost(const PluginHost&) = delete;
  PluginHost& operator=(const PluginHost&) = delete;

  // 提供主程序服务（日志、D3D 设备）。
  //
  // 设备可以在加载插件之后再绑定：接收端的 D3D 设备要等呈现窗口建好
  // 才存在，而插件加载发生在更早。create_analysis 时会用最新的值。
  Status initialize(void* d3d_device, void* d3d_context);
  void set_d3d_device(void* d3d_device, void* d3d_context);

  // 扫描目录下所有 .dll，逐个尝试加载。返回成功加载的数量。
  // 加载失败的插件会记录在 plugins() 里，error 字段说明原因。
  std::size_t load_directory(const std::string& directory);

  // 加载单个插件文件。
  Status load(const std::string& path);

  // 已扫描/加载的所有插件（含失败的）。
  [[nodiscard]] const std::vector<PluginInfo>& plugins() const noexcept { return plugins_; }

  // 为指定插件创建分析实例。config 可为空字符串。
  // 返回的适配器可以直接喂给 AnalysisTap。
  class AnalysisAdapter;
  [[nodiscard]] std::shared_ptr<AnalysisAdapter> create_analysis(const std::string& plugin_name,
                                                                const std::string& config = "");

  // 创建第一个可用的分析插件实例（按加载顺序）。
  [[nodiscard]] std::shared_ptr<AnalysisAdapter> create_first_analysis(
      const std::string& config = "");

  void unload_all();

 private:
  struct Loaded;
  std::vector<std::unique_ptr<Loaded>> loaded_;
  std::vector<PluginInfo> plugins_;
  DuoPluginHostServices services_{};
  void* d3d_device_ = nullptr;
  void* d3d_context_ = nullptr;
};

// 把插件实例适配成 AnalysisTap 能用的回调。
//
// 职责：持有插件实例、把 DecodedFrame 翻译成 DuoFrameView、
// 转发 on_reset / destroy、以及在插件提供 get_overlay_boxes 时
// 缓存一份检测框快照供呈现线程取用。
//
// 线程模型：on_frame 在分析线程跑，抓取检测框；呈现线程通过
// copy_overlay_boxes 拿快照。两边用互斥锁交换整个数组（几十个框，
// 拷贝成本可忽略），插件内部的检测数据不必加锁。
class PluginHost::AnalysisAdapter {
 public:
  ~AnalysisAdapter();

  AnalysisAdapter(const AnalysisAdapter&) = delete;
  AnalysisAdapter& operator=(const AnalysisAdapter&) = delete;

  // 喂给 AnalysisTap::start 的回调与 user 指针。
  [[nodiscard]] static void frame_thunk(void* user, const DecodedFrame& frame);
  [[nodiscard]] void* user_ptr() noexcept { return this; }

  void reset();
  [[nodiscard]] const std::string& plugin_name() const noexcept { return plugin_name_; }
  [[nodiscard]] u64 frames_ok() const noexcept { return frames_ok_; }
  [[nodiscard]] u64 frames_failed() const noexcept { return frames_failed_; }

  // 呈现线程用：把最近一帧的检测框拷贝到 out（最多 max_boxes），
  // 返回拷贝的个数。没有可画内容时返回 0。
  int copy_overlay_boxes(DuoOverlayBox* out, int max_boxes) const;

  // 与 copy_overlay_boxes 同签名，供 Receiver 当成 C 回调注册。
  [[nodiscard]] static int overlay_boxes_thunk(void* user, DuoOverlayBox* out, int max_boxes) {
    return static_cast<const AnalysisAdapter*>(user)->copy_overlay_boxes(out, max_boxes);
  }

 private:
  friend class PluginHost;
  AnalysisAdapter(std::string name, const DuoAnalysisPlugin* api, void* instance,
                  void (*destroy_fn)(void*));

  std::string plugin_name_;
  const DuoAnalysisPlugin* api_ = nullptr;
  void* instance_ = nullptr;
  void (*destroy_fn_)(void*) = nullptr;
  u64 frames_ok_ = 0;
  u64 frames_failed_ = 0;

  // 主程序看到的函数表大小。只有 >= 含 get_overlay_boxes 的版本才去读该字段，
  // 否则读到的会是插件 struct 之外的内存。
  unsigned int api_struct_size_ = 0;

  mutable std::mutex overlay_mutex_;
  std::vector<DuoOverlayBox> overlay_boxes_;
};

}  // namespace duo
