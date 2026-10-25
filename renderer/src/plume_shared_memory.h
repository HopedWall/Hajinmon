// Hajinmon native renderer - guest physical memory on the GPU.
//
// A 512 MB buffer mirroring the guest's physical memory, which the translated
// shaders read vertices from and which guest index buffers are bound from. The
// SDK base class tracks the pages the CPU writes (through page protection) and
// asks for the stale ones to be uploaded when a draw needs them.

#pragma once

#include <memory>
#include <vector>

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

  // Uploads leave the buffer in the copy state, so several in a row share the
  // barriers. To call before the GPU reads it (draws) and before the command
  // list is submitted.
  void FinishUploads();

  // The ranges draws read (vertices and indices). Uploads of pages no draw of
  // the frame has read yet are done before the frame's command list instead
  // of between draws, without stalling the GPU with barriers mid-frame.
  void MarkRead(uint32_t start, uint32_t length);
  // At the end of the frame (before EndFrame): records those uploads.
  void EndFrame();

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  PlumeRenderer& renderer_;
  std::unique_ptr<plume::RenderBuffer> buffer_;
  bool copying_ = false;

  // One bit per page read by a draw of the current frame, and the 64-page
  // blocks to clear at the end of the frame.
  std::vector<uint64_t> read_pages_;
  std::vector<uint32_t> read_blocks_;
  struct DeferredCopy {
    uint32_t start;
    uint32_t length;
    plume::RenderBuffer* source;
    uint64_t source_offset;
  };
  std::vector<DeferredCopy> deferred_copies_;
};

}  // namespace hajinmon::renderer
