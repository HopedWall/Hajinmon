// Hajinmon native renderer - primitive and index buffer processing.
//
// The SDK base class converts what the host can't draw directly (triangle
// fans, line loops, quad lists, 32-bit index masking...). Converted index
// buffers live in the frame's upload memory, the built-in ones (for
// auto-indexed conversions) in a buffer created once.

#pragma once

#include <memory>
#include <vector>

#include <rex/graphics/primitive_processor.h>

#include "plume_renderer.h"

namespace hajinmon::renderer {

class PlumePrimitiveProcessor : public rex::graphics::PrimitiveProcessor {
 public:
  PlumePrimitiveProcessor(const rex::graphics::RegisterFile& register_file,
                          rex::memory::Memory& memory, rex::graphics::SharedMemory& shared_memory,
                          PlumeRenderer& renderer)
      : PrimitiveProcessor(register_file, memory, shared_memory), renderer_(renderer) {}
  ~PlumePrimitiveProcessor() override;

  bool Initialize();
  void Shutdown();
  // Call after each frame.
  void EndFrame();

  // The index buffer of a draw, for kHostConverted and kHostBuiltin* results.
  plume::RenderBufferReference GetIndexBuffer(const ProcessingResult& result) const;

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(rex::graphics::xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  PlumeRenderer& renderer_;
  std::unique_ptr<plume::RenderBuffer> builtin_index_buffer_;
  // This frame's converted index buffers, by handle.
  std::vector<plume::RenderBufferReference> converted_index_buffers_;
};

}  // namespace hajinmon::renderer
