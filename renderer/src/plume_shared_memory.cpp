// Hajinmon native renderer - guest physical memory on the GPU.

#include "plume_shared_memory.h"

#include <cstring>

#include <rex/logging/macros.h>
#include <rex/system/xmemory.h>

namespace hajinmon::renderer {

PlumeSharedMemory::PlumeSharedMemory(rex::memory::Memory& memory, PlumeRenderer& renderer)
    : SharedMemory(memory), renderer_(renderer) {}

PlumeSharedMemory::~PlumeSharedMemory() { Shutdown(); }

bool PlumeSharedMemory::Initialize() {
  buffer_ = renderer_.device()->createBuffer(plume::RenderBufferDesc::DefaultBuffer(
      kBufferSize, plume::RenderBufferFlag::STORAGE | plume::RenderBufferFlag::INDEX));
  if (!buffer_) {
    REXLOG_ERROR("Plume: could not create the {} MB shared memory buffer", kBufferSize >> 20);
    return false;
  }
  buffer_->setName("Shared memory");
  InitializeCommon();
  return true;
}

void PlumeSharedMemory::Shutdown() {
  ShutdownCommon();
  buffer_.reset();
}

bool PlumeSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  plume::RenderCommandList* list = renderer_.command_list();
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderBufferBarrier(buffer_.get(), plume::RenderBufferAccess::WRITE));
  uint32_t page_size_log2 = this->page_size_log2();
  for (const auto& [first_page, page_count] : upload_page_ranges) {
    uint32_t start = first_page << page_size_log2;
    uint32_t length = page_count << page_size_log2;
    // Before copying, so writes made during the copy invalidate the pages again.
    MakeRangeValid(start, length, false);
    PlumeRenderer::Allocation upload = renderer_.Upload(length, 256);
    std::memcpy(upload.data, memory().TranslatePhysical<const uint8_t*>(start), length);
    list->copyBufferRegion(plume::RenderBufferReference(buffer_.get(), start),
                           plume::RenderBufferReference(upload.buffer, upload.offset), length);
  }
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderBufferBarrier(buffer_.get(), plume::RenderBufferAccess::READ));
  return true;
}

}  // namespace hajinmon::renderer
