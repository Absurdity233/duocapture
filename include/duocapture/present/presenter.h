#pragma once

#include "duocapture/common/status.h"
#include "duocapture/decode/decoder.h"

namespace duo {

class IPresenter {
 public:
  virtual ~IPresenter() = default;

  virtual Status present(const DecodedFrame& frame) = 0;
  [[nodiscard]] virtual const char* backend_name() const = 0;
};

}  // namespace duo
