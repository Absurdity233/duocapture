#pragma once

namespace duo {

// 网络层自检：打包 → FEC → 乱序 + 丢包 → 抖动缓冲重组 → 内容校验。
// 不依赖 GPU 与网络，可在任意机器上跑。
bool run_net_selftest(bool verbose);

}  // namespace duo
