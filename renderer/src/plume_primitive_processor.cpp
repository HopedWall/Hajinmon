// Hajinmon native renderer - primitive and index buffer processing.

#include "plume_primitive_processor.h"

namespace hajinmon::renderer {

using rex::graphics::xenos::IndexFormat;

PlumePrimitiveProcessor::~PlumePrimitiveProcessor() { Shutdown(); }

bool PlumePrimitiveProcessor::Initialize() {
  // 32-bit indices are supported, triangle fans (missing on Direct3D 12), line
  // loops and quad lists are converted, rectangle lists are expanded by a
  // geometry shader, and point sprites are not expanded yet.
  return InitializeCommon(true, false, false, false, true, true);
}

void PlumePrimitiveProcessor::Shutdown() {
  ShutdownCommon();
  builtin_index_buffer_.reset();
  converted_index_buffers_.clear();
}

void PlumePrimitiveProcessor::EndFrame() {
  ClearPerFrameCache();
  converted_index_buffers_.clear();
}

bool PlumePrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  // Read by the GPU directly from upload memory: it is small.
  builtin_index_buffer_ = renderer_.device()->createBuffer(
      plume::RenderBufferDesc::UploadBuffer(size_bytes, plume::RenderBufferFlag::INDEX));
  if (!builtin_index_buffer_) {
    return false;
  }
  void* mapping = builtin_index_buffer_->map();
  fill_callback(mapping);
  builtin_index_buffer_->unmap();
  return true;
}

void* PlumePrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  uint32_t index_size = format == IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  uint64_t size = uint64_t(index_size) * index_count +
                  (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0);
  PlumeRenderer::Allocation allocation = renderer_.Upload(size, 16);
  uint8_t* data = allocation.data;
  uint64_t offset = allocation.offset;
  if (coalign_for_simd) {
    ptrdiff_t coalignment = GetSimdCoalignmentOffset(data, coalignment_original_address);
    data += coalignment;
    offset += coalignment;
  }
  backend_handle_out = converted_index_buffers_.size();
  converted_index_buffers_.emplace_back(allocation.buffer, offset);
  return data;
}

plume::RenderBufferReference PlumePrimitiveProcessor::GetIndexBuffer(
    const ProcessingResult& result) const {
  switch (result.index_buffer_type) {
    case ProcessedIndexBufferType::kHostConverted:
      return converted_index_buffers_[result.host_index_buffer_handle];
    case ProcessedIndexBufferType::kHostBuiltinForAuto:
    case ProcessedIndexBufferType::kHostBuiltinForDMA:
      return plume::RenderBufferReference(
          builtin_index_buffer_.get(),
          GetBuiltinIndexBufferOffsetBytes(result.host_index_buffer_handle));
    default:
      return {};
  }
}

}  // namespace hajinmon::renderer
