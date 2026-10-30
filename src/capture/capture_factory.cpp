#include "duocapture/capture/capture_source.h"

namespace duo {

std::unique_ptr<ICaptureSource> create_wgc_capture_source();
std::unique_ptr<ICaptureSource> create_ddapi_capture_source();

std::unique_ptr<ICaptureSource> create_capture_source(bool prefer_wgc) {
  if (prefer_wgc) {
    if (auto source = create_wgc_capture_source()) {
      return source;
    }
  }
  return create_ddapi_capture_source();
}

}  // namespace duo
