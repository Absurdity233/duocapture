#include "duocapture/plugin/plugin_host.h"

#include <cstddef>
#include <cstring>

#include "duocapture/common/log.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <dirent.h>
#endif

namespace duo {
namespace {

// 每帧最多带给宿主多少个框。超出部分丢弃——这是叠加显示，不是数据通道。
constexpr int kMaxOverlayBoxes = 128;

// ---- 跨平台动态库薄封装 ------------------------------------------------
struct DynLib {
  void* handle = nullptr;

  bool open(const std::string& path, std::string& error) {
#if defined(_WIN32)
    handle = reinterpret_cast<void*>(
        ::LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
    if (handle == nullptr) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "LoadLibrary 失败 (错误码 %lu)", ::GetLastError());
      error = buf;
      return false;
    }
#else
    handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      const char* err = ::dlerror();
      error = err != nullptr ? err : "dlopen 失败";
      return false;
    }
#endif
    return true;
  }

  void* symbol(const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return ::dlsym(handle, name);
#endif
  }

  void close() {
    if (handle == nullptr) {
      return;
    }
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(handle));
#else
    ::dlclose(handle);
#endif
    handle = nullptr;
  }
};

const char* kind_name(DuoPluginKind kind) {
  switch (kind) {
    case DUO_PLUGIN_KIND_ANALYSIS: return "analysis";
    case DUO_PLUGIN_KIND_OUTPUT: return "output";
    default: return "unknown";
  }
}

// 日志桥：把插件的 C 调用转成主程序日志。
void host_log(int level, const char* message) {
  if (message == nullptr) {
    return;
  }
  switch (level) {
    case DUO_LOG_TRACE: DUO_TRACE("[plugin] %s", message); break;
    case DUO_LOG_DEBUG: DUO_DEBUG("[plugin] %s", message); break;
    case DUO_LOG_INFO: DUO_INFO("[plugin] %s", message); break;
    case DUO_LOG_WARN: DUO_WARN("[plugin] %s", message); break;
    default: DUO_ERROR("[plugin] %s", message); break;
  }
}

bool ends_with_ci(const std::string& text, const std::string& suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  const std::size_t offset = text.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[offset + i])));
    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
    if (a != b) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> list_plugin_files(const std::string& directory) {
  std::vector<std::string> out;
#if defined(_WIN32)
  WIN32_FIND_DATAA data{};
  const std::string pattern = directory + "\\*";
  HANDLE handle = ::FindFirstFileA(pattern.c_str(), &data);
  if (handle == INVALID_HANDLE_VALUE) {
    return out;
  }
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    const std::string name = data.cFileName;
    if (ends_with_ci(name, ".dll")) {
      out.push_back(directory + "\\" + name);
    }
  } while (::FindNextFileA(handle, &data));
  ::FindClose(handle);
#else
  DIR* dir = ::opendir(directory.c_str());
  if (dir == nullptr) {
    return out;
  }
  while (dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (ends_with_ci(name, ".so")) {
      out.push_back(directory + "/" + name);
    }
  }
  ::closedir(dir);
#endif
  return out;
}

}  // namespace

// ---- Loaded ------------------------------------------------------------

struct PluginHost::Loaded {
  DynLib lib;
  const DuoPlugin* descriptor = nullptr;
};

// ---- AnalysisAdapter ---------------------------------------------------

PluginHost::AnalysisAdapter::AnalysisAdapter(std::string name, const DuoAnalysisPlugin* api,
                                            void* instance, void (*destroy_fn)(void*))
    : plugin_name_(std::move(name)),
      api_(api),
      instance_(instance),
      destroy_fn_(destroy_fn),
      api_struct_size_(api != nullptr ? api->struct_size : 0) {}

PluginHost::AnalysisAdapter::~AnalysisAdapter() {
  if (destroy_fn_ != nullptr && instance_ != nullptr) {
    destroy_fn_(instance_);
  }
  instance_ = nullptr;
}

void PluginHost::AnalysisAdapter::frame_thunk(void* user, const DecodedFrame& frame) {
  auto* self = static_cast<AnalysisAdapter*>(user);
  if (self == nullptr || self->api_ == nullptr || self->api_->on_frame == nullptr ||
      self->instance_ == nullptr) {
    return;
  }

  DuoFrameView view{};
  view.texture = frame.texture;
  view.width = frame.width;
  view.height = frame.height;
  view.capture_ts_ns = frame.capture_ts_ns;
  view.key = frame.key ? 1 : 0;

  const int rc = self->api_->on_frame(self->instance_, &view);
  if (rc >= 0) {
    ++self->frames_ok_;
  } else {
    ++self->frames_failed_;
  }

  // 顺手抓一份检测框给呈现线程。用 struct_size 判断插件是否提供该字段：
  // 老插件的 struct 到 destroy 为止，读 get_overlay_boxes 会越界。
  constexpr unsigned int kOverlayFieldEnd = static_cast<unsigned int>(
      offsetof(DuoAnalysisPlugin, get_overlay_boxes) + sizeof(void*));
  if (self->api_struct_size_ < kOverlayFieldEnd || self->api_->get_overlay_boxes == nullptr) {
    return;
  }

  DuoOverlayBox boxes[kMaxOverlayBoxes];
  const int count = self->api_->get_overlay_boxes(self->instance_, boxes, kMaxOverlayBoxes);
  const int clamped = count < 0 ? 0 : (count > kMaxOverlayBoxes ? kMaxOverlayBoxes : count);

  std::lock_guard<std::mutex> lock(self->overlay_mutex_);
  self->overlay_boxes_.assign(boxes, boxes + clamped);
}

int PluginHost::AnalysisAdapter::copy_overlay_boxes(DuoOverlayBox* out, int max_boxes) const {
  if (out == nullptr || max_boxes <= 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(overlay_mutex_);
  const int count =
      static_cast<int>(overlay_boxes_.size()) > max_boxes ? max_boxes
                                                          : static_cast<int>(overlay_boxes_.size());
  for (int i = 0; i < count; ++i) {
    out[i] = overlay_boxes_[static_cast<std::size_t>(i)];
  }
  return count;
}

void PluginHost::AnalysisAdapter::reset() {
  if (api_ != nullptr && api_->on_reset != nullptr && instance_ != nullptr) {
    api_->on_reset(instance_);
  }
}

// ---- PluginHost --------------------------------------------------------

PluginHost::PluginHost() {
  services_.abi_version = DUO_PLUGIN_ABI_VERSION;
  services_.struct_size = sizeof(DuoPluginHostServices);
  services_.log = &host_log;
  services_.d3d_device = nullptr;
  services_.d3d_context = nullptr;
}

PluginHost::~PluginHost() { unload_all(); }

Status PluginHost::initialize(void* d3d_device, void* d3d_context) {
  d3d_device_ = d3d_device;
  d3d_context_ = d3d_context;
  services_.d3d_device = d3d_device;
  services_.d3d_context = d3d_context;
  return Status::Ok;
}

void PluginHost::set_d3d_device(void* d3d_device, void* d3d_context) {
  d3d_device_ = d3d_device;
  d3d_context_ = d3d_context;
  services_.d3d_device = d3d_device;
  services_.d3d_context = d3d_context;
}

std::size_t PluginHost::load_directory(const std::string& directory) {
  std::size_t loaded_count = 0;
  for (const std::string& path : list_plugin_files(directory)) {
    if (ok(load(path))) {
      ++loaded_count;
    }
  }
  return loaded_count;
}

Status PluginHost::load(const std::string& path) {
  PluginInfo info{};
  info.path = path;

  auto record_failure = [this, &info](const std::string& reason) {
    info.compatible = false;
    info.error = reason;
    plugins_.push_back(std::move(info));
    DUO_WARN("插件加载失败 %s: %s", info.path.c_str(), info.error.c_str());
  };

  auto lib = std::make_unique<Loaded>();
  std::string open_error;
  if (!lib->lib.open(path, open_error)) {
    record_failure(open_error);
    return Status::IoError;
  }

  auto entry = reinterpret_cast<DuoPluginEntryFn>(lib->lib.symbol(DUO_PLUGIN_ENTRY_NAME));
  if (entry == nullptr) {
    record_failure(std::string("缺少导出函数 ") + DUO_PLUGIN_ENTRY_NAME);
    return Status::NotSupported;
  }

  const DuoPlugin* descriptor = entry(DUO_PLUGIN_ABI_VERSION);
  if (descriptor == nullptr) {
    record_failure("插件拒绝当前主程序 ABI 版本");
    return Status::NotSupported;
  }

  // 双重校验：版本必须完全一致，结构体不能比主程序认识的小。
  if (descriptor->abi_version != DUO_PLUGIN_ABI_VERSION) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ABI 版本不匹配：插件 %u，主程序 %u",
                  descriptor->abi_version, DUO_PLUGIN_ABI_VERSION);
    record_failure(buf);
    return Status::NotSupported;
  }
  if (descriptor->struct_size < sizeof(DuoPlugin)) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "插件描述符过小：%u < %zu", descriptor->struct_size,
                  sizeof(DuoPlugin));
    record_failure(buf);
    return Status::NotSupported;
  }
  if (descriptor->create == nullptr || descriptor->destroy == nullptr) {
    record_failure("插件描述符缺少 create / destroy");
    return Status::NotSupported;
  }

  lib->descriptor = descriptor;
  info.name = descriptor->name != nullptr ? descriptor->name : "(未命名)";
  info.version = descriptor->version != nullptr ? descriptor->version : "?";
  info.description = descriptor->description != nullptr ? descriptor->description : "";
  info.kind = descriptor->kind;
  info.compatible = true;

  DUO_INFO("插件已加载: %s v%s [%s] %s", info.name.c_str(), info.version.c_str(),
           kind_name(info.kind), info.description.c_str());

  plugins_.push_back(std::move(info));
  loaded_.push_back(std::move(lib));
  return Status::Ok;
}

std::shared_ptr<PluginHost::AnalysisAdapter> PluginHost::create_analysis(
    const std::string& plugin_name, const std::string& config) {
  for (const auto& lib : loaded_) {
    if (lib->descriptor == nullptr) {
      continue;
    }
    const char* name = lib->descriptor->name;
    if (name == nullptr || plugin_name != name) {
      continue;
    }
    if (lib->descriptor->kind != DUO_PLUGIN_KIND_ANALYSIS ||
        lib->descriptor->as_analysis == nullptr) {
      DUO_WARN("插件 %s 不是分析插件", plugin_name.c_str());
      return nullptr;
    }

    void* instance = nullptr;
    const int rc = lib->descriptor->create(&services_, config.c_str(), &instance);
    if (rc != 0 || instance == nullptr) {
      DUO_WARN("插件 %s 创建实例失败 (rc=%d)", plugin_name.c_str(), rc);
      return nullptr;
    }

    const DuoAnalysisPlugin* api = lib->descriptor->as_analysis(instance);
    if (api == nullptr || api->on_frame == nullptr) {
      DUO_WARN("插件 %s 没有提供 on_frame", plugin_name.c_str());
      lib->descriptor->destroy(instance);
      return nullptr;
    }
    if (api->abi_version != DUO_PLUGIN_ABI_VERSION) {
      DUO_WARN("插件 %s 的分析 ABI 版本不匹配", plugin_name.c_str());
      lib->descriptor->destroy(instance);
      return nullptr;
    }

    return std::shared_ptr<AnalysisAdapter>(
        new AnalysisAdapter(plugin_name, api, instance, lib->descriptor->destroy));
  }
  return nullptr;
}

std::shared_ptr<PluginHost::AnalysisAdapter> PluginHost::create_first_analysis(
    const std::string& config) {
  for (const auto& lib : loaded_) {
    if (lib->descriptor != nullptr && lib->descriptor->kind == DUO_PLUGIN_KIND_ANALYSIS) {
      const char* name = lib->descriptor->name;
      if (name != nullptr) {
        return create_analysis(name, config);
      }
    }
  }
  return nullptr;
}

void PluginHost::unload_all() {
  // 必须先销毁所有插件实例再卸库，否则 destroy 会调用到已卸载的代码。
  // 调用方应保证所有 AnalysisAdapter 已经释放；这里只能尽力而为，
  // 因为 shared_ptr 可能还持有引用。
  loaded_.clear();
}

}  // namespace duo
