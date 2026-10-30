#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "duocapture/common/types.h"
#include "duocapture/encode/encoder.h"

#if defined(_WIN32)
#include <d3d11.h>
#endif

namespace duo {

#if defined(_WIN32)

// 直接调用 nvEncodeAPI64.dll 的 NVENC 封装。
//
// 不采用静态导入库，运行时 LoadLibrary 并解析函数表——这样没有 NVIDIA 驱动
// 的机器上程序依然能启动，只是在创建编码器时失败。这也让 CI 能在没有 GPU
// 的机器上编译和跑网络层测试。
class NvencEncoder final : public IEncoder {
 public:
  struct Stats {
    u64 frames_encoded = 0;
    u64 frames_submitted = 0;
    u64 bytes_out = 0;
    double avg_encode_ms = 0.0;
  };

  NvencEncoder();
  ~NvencEncoder() override;

  NvencEncoder(const NvencEncoder&) = delete;
  NvencEncoder& operator=(const NvencEncoder&) = delete;

  // device 必须是采集/转换所用的同一块 ID3D11Device。
  // 纹理跨设备共享会引入一次额外拷贝，低延迟链路上不划算。
  Status initialize(ID3D11Device* device, ID3D11DeviceContext* context);

  Status configure(const EncoderConfig& config) override;
  Status encode(const FrameView& frame, EncodedFrame& out) override;
  Status request_keyframe() override;

  [[nodiscard]] const char* backend_name() const override;
  [[nodiscard]] const Stats& stats() const noexcept;
  [[nodiscard]] const EncoderConfig& config() const noexcept;

  // H.264 / HEVC 输出 Annex-B 起始码；AV1 输出带 OBU 的裸流。
  [[nodiscard]] bool outputs_annex_b() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#else

class NvencEncoder final : public IEncoder {
 public:
  struct Stats {};
  NvencEncoder();
  ~NvencEncoder() override;
  Status initialize(void*, void*) { return Status::NotSupported; }
  Status configure(const EncoderConfig&) override { return Status::NotSupported; }
  Status encode(const FrameView&, EncodedFrame&) override { return Status::NotSupported; }
  Status request_keyframe() override { return Status::NotSupported; }
  [[nodiscard]] const char* backend_name() const override { return "nvenc-unavailable"; }
  [[nodiscard]] const Stats& stats() const noexcept {
    static const Stats empty{};
    return empty;
  }
  [[nodiscard]] const EncoderConfig& config() const noexcept {
    static const EncoderConfig empty{};
    return empty;
  }
  [[nodiscard]] bool outputs_annex_b() const noexcept { return false; }
  struct Impl {};
  std::unique_ptr<Impl> impl_;
};

#endif

}  // namespace duo
