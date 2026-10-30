// NVDEC / D3D11VA 解码器。
//
// M1 落地时优先走 D3D11VA（ID3D11VideoDecoder），理由：
//   * 不绑定厂商，A 端是什么卡都能跑
//   * 解码输出直接落在 D3D11 纹理上，与呈现层零拷贝对接
// 只有在需要 4:4:4 或特殊 profile 时才考虑 NVDEC 直连。

#include "duocapture/decode/decoder.h"

namespace duo {

std::unique_ptr<IDecoder> create_decoder() { return nullptr; }

}  // namespace duo
