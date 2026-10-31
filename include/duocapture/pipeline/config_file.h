#pragma once

#include <string>

#include "duocapture/common/status.h"
#include "duocapture/pipeline/receiver_config.h"
#include "duocapture/pipeline/sender_config.h"

namespace duo {

// 把 JSON 配置文件叠加到 config 上。
//
// 语义是「叠加」而不是「替换」：文件里没写的键保留结构体里已有的值。
// 这让调用方可以先加载文件、再让命令行参数覆盖——命令行是更具体的意图。
//
// error 里是逐条的错误（带文件名和行号），一次把能发现的问题全列出来，
// 而不是遇到第一个就返回。
Status apply_sender_config_file(const std::string& path, SenderConfig& config,
                                std::string& error);
Status apply_receiver_config_file(const std::string& path, ReceiverConfig& config,
                                  std::string& error);

}  // namespace duo
