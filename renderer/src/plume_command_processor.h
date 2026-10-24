// Hajinmon native renderer - command processor.
//
// The SDK base class parses the GPU command stream (PM4 packets) on its own
// thread and calls the Issue* methods for draws, resolves and frame swaps.
//
// Milestone 3: draws are rendered with translated shaders to host render
// targets (see render_target_cache.h), resolves copy them to textures, and the
// texture the game swaps to is presented.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>

#include "pipeline_cache.h"
#include "plume_primitive_processor.h"
#include "plume_shared_memory.h"
#include "render_target_cache.h"
#include "shader_cache.h"
#include "shader_catalog.h"
#include "texture_cache.h"

namespace hajinmon::renderer {

class PlumeGraphicsSystem;

class PlumeCommandProcessor : public rex::graphics::CommandProcessor {
 public:
  PlumeCommandProcessor(PlumeGraphicsSystem* graphics_system, rex::system::KernelState* kernel_state);
  ~PlumeCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;

  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                    uint32_t guest_address, const uint32_t* host_address,
                                    uint32_t dword_count) override;

  bool IssueDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  // Constant block kinds, deduplicated within a frame.
  enum Block {
    kBlockSystem,
    kBlockVertexFloat,
    kBlockPixelFloat,
    kBlockBoolLoop,
    kBlockFetch,
    kBlockCount,
  };
  // Returns the offset of the block in the frame's constant memory, or
  // UINT32_MAX if it is full.
  uint32_t WriteBlock(Block block, const void* data, uint32_t size);
  // Why draws are not done, for the statistics.
  enum SkipReason {
    kSkipNoVertexShader,
    kSkipNoRasterization,
    kSkipShaderNotReady,
    kSkipPrimitive,
    kSkipRenderTargets,
    kSkipConstantsFull,
    kSkipCulled,
    kSkipPipeline,
    kSkipIndexBuffer,
    kSkipCount,
  };
  bool Skip(SkipReason reason) {
    ++skipped_[reason];
    return false;
  }
  bool Draw();
  void LogStats();
  void EnsureCatalog();

  PlumeGraphicsSystem* plume_graphics_system_;
  ShaderCatalog catalog_;
  std::unique_ptr<ShaderCache> shader_cache_;
  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::Shader>> shaders_;

  std::unique_ptr<PlumeSharedMemory> shared_memory_;
  std::unique_ptr<PlumePrimitiveProcessor> primitive_processor_;
  std::unique_ptr<RenderTargetCache> render_target_cache_;
  std::unique_ptr<TextureCache> texture_cache_;
  std::unique_ptr<PipelineCache> pipeline_cache_;
  std::unique_ptr<plume::RenderDescriptorSet> frame_sets_[PlumeRenderer::kFramesInFlight];

  struct BlockState {
    std::vector<uint8_t> data;
    uint32_t offset = UINT32_MAX;
  };
  BlockState blocks_[kBlockCount];

  // Statistics, logged every few seconds.
  uint64_t frames_ = 0;
  uint64_t draws_ = 0;
  uint64_t draws_done_ = 0;
  uint64_t copies_ = 0;
  uint64_t presented_ = 0;
  uint64_t skipped_[kSkipCount] = {};
  std::chrono::steady_clock::time_point stats_start_;
};

}  // namespace hajinmon::renderer
