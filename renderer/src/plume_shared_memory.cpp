// Hajinmon native renderer - guest physical memory on the GPU.

#include "plume_shared_memory.h"

#include <algorithm>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(plume_defer_uploads, false, "GPU/Plume",
                    "Experimental (breaks Hayabusa on AMD): upload guest memory no draw of the frame has read yet before the frame "
                    "instead of between draws (fewer GPU stalls)");

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
  read_pages_.assign((kBufferSize >> page_size_log2()) / 64, 0);
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
  uint32_t page_size_log2 = this->page_size_log2();
  for (const auto& [first_page, page_count] : upload_page_ranges) {
    uint32_t start = first_page << page_size_log2;
    uint32_t length = page_count << page_size_log2;
    // Before copying, so writes made during the copy invalidate the pages again.
    MakeRangeValid(start, length, false);
    PlumeRenderer::Allocation upload = renderer_.Upload(length, 256);
    std::memcpy(upload.data, memory().TranslatePhysical<const uint8_t*>(start), length);
    // Pages a draw of this frame has already read must be replaced at this
    // point of the frame; the others can be copied before the whole frame.
    bool read = false;
    for (uint32_t page = first_page; page < first_page + page_count && !read; ++page) {
      read = (read_pages_[page >> 6] >> (page & 63)) & 1;
    }
    if (!read && REXCVAR_GET(plume_defer_uploads)) {
      deferred_copies_.push_back({start, length, upload.buffer, upload.offset});
      continue;
    }
    if (!copying_) {
      list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderBufferBarrier(buffer_.get(), plume::RenderBufferAccess::WRITE));
      copying_ = true;
    }
    list->copyBufferRegion(plume::RenderBufferReference(buffer_.get(), start),
                           plume::RenderBufferReference(upload.buffer, upload.offset), length);
  }
  return true;
}

void PlumeSharedMemory::MarkRead(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last =
      uint32_t((std::min(uint64_t(start) + length, uint64_t(kBufferSize)) - 1) >> page_size_log2());
  for (uint32_t page = page_first; page <= page_last; ++page) {
    uint64_t& block = read_pages_[page >> 6];
    if (!block) {
      read_blocks_.push_back(page >> 6);
    }
    block |= uint64_t(1) << (page & 63);
  }
}

void PlumeSharedMemory::EndFrame() {
  FinishUploads();
  if (!deferred_copies_.empty()) {
    // Between the previous frame's draws and this frame's.
    plume::RenderCommandList* list = renderer_.upload_command_list();
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderBufferBarrier(buffer_.get(), plume::RenderBufferAccess::WRITE));
    for (const DeferredCopy& copy : deferred_copies_) {
      list->copyBufferRegion(plume::RenderBufferReference(buffer_.get(), copy.start),
                             plume::RenderBufferReference(copy.source, copy.source_offset),
                             copy.length);
    }
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderBufferBarrier(buffer_.get(), plume::RenderBufferAccess::READ));
    deferred_copies_.clear();
  }
  for (uint32_t block : read_blocks_) {
    read_pages_[block] = 0;
  }
  read_blocks_.clear();
}

void PlumeSharedMemory::FinishUploads() {
  if (copying_) {
    renderer_.command_list()->barriers(
        plume::RenderBarrierStage::GRAPHICS,
        plume::RenderBufferBarrier(buffer_.get(), plume::RenderBufferAccess::READ));
    copying_ = false;
  }
}

}  // namespace hajinmon::renderer
