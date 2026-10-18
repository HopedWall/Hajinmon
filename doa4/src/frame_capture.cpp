// doa4 - on-demand capture of the game image (see frame_capture.h)

#include "frame_capture.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include <rex/logging/macros.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

namespace doa4 {
namespace {

// .raw format: "RAWI", width, height, stride (uint32 little-endian), then the
// rows as R8 G8 B8 X8.
bool SaveRaw(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  const uint32_t header[] = {0x49574152u /* "RAWI" */, image.width, image.height,
                             uint32_t(image.stride)};
  out.write(reinterpret_cast<const char*>(header), sizeof(header));
  out.write(reinterpret_cast<const char*>(image.data.data()), std::streamsize(image.data.size()));
  return bool(out);
}

void CaptureLoop(rex::Runtime* runtime, std::filesystem::path dir) {
  const auto request = dir / "capture.req";
  int index = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::error_code ec;
    if (!std::filesystem::exists(request, ec)) continue;
    // The file may contain the number of frames to capture in a burst
    // (about 10 per second); empty = a single frame.
    int count = 1;
    {
      std::ifstream in(request);
      in >> count;
      if (!in || count < 1) count = 1;
      if (count > 100) count = 100;
    }
    std::filesystem::remove(request, ec);

    auto* graphics = runtime->graphics_system();
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    for (int i = 0; i < count; ++i) {
      if (i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      rex::ui::RawImage image;
      if (!presenter || !presenter->CaptureGuestOutput(image)) {
        REXLOG_WARN("doa4: frame capture failed");
        continue;
      }
      auto path = dir / ("capture_" + std::to_string(index) + ".raw");
      if (SaveRaw(path, image)) {
        REXLOG_INFO("doa4: {}x{} frame saved to {}", image.width, image.height, path.string());
      }
      ++index;
    }
  }
}

}  // namespace

void StartFrameCapture(rex::Runtime* runtime) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // getenv on Windows
  const char* dir = std::getenv("DOA4_CAPTURE_DIR");
#pragma clang diagnostic pop
  if (!dir || !*dir || !runtime) return;
  std::filesystem::path path(dir);
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
  REXLOG_INFO("doa4: frame capture enabled (requests in {})", (path / "capture.req").string());
  std::thread(CaptureLoop, runtime, path).detach();
}

}  // namespace doa4
