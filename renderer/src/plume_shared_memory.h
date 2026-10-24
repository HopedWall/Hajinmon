// Hajinmon native renderer - guest physical memory on the GPU.
//
// A 512 MB buffer mirroring the guest's physical memory, which the translated
// shaders read vertices from and which guest index buffers are bound from. The
// SDK base class tracks the pages the CPU writes (through page protection) and
// asks for the stale ones to be uploaded when a draw needs them.

#pragma once

#include <memory>

#include <rex/graphics/shared_memory.h>

#include "plume_renderer.h"

namespace hajinmon::renderer {

class PlumeSharedMemory : public rex::graphics::SharedMemory {
 public:
  PlumeSharedMemory(rex::memory::Memory& memory, PlumeRenderer& renderer);
  ~PlumeSharedMemory() override;

  bool Initialize();
  void Shutdown();

  plume::RenderBuffer* buffer() const { return buffer_.get(); }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  PlumeRenderer& renderer_;
  std::unique_ptr<plume::RenderBuffer> buffer_;
};

}  // namespace hajinmon::renderer
