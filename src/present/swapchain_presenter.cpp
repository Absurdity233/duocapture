// 交换链呈现器。
//
// M1 落地要点：
//   * DXGI_SWAP_EFFECT_FLIP_DISCARD
//   * SetMaximumFrameLatency(1) + waitable object
//   * Present(0, DXGI_PRESENT_ALLOW_TEARING)，且交换链创建时带
//     DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
//   * 只用"最新帧"：拿到新帧就覆盖，旧帧直接丢弃，绝不排队
//
// 老式 swapchain 或默认 vsync 会白送 1-2 帧延迟，这里不能省。

#include "duocapture/present/presenter.h"

namespace duo {

}  // namespace duo
