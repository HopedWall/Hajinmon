// Hajinmon native renderer - graphics pipelines.
//
// One pipeline layout serves every draw (see the resource interface in
// hlsl_translator.h). Pipelines are created on first use for each combination
// of shaders and fixed-function state, taken from the guest registers.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

#include "plume_renderer.h"
#include "shader_cache.h"

namespace hajinmon::renderer {

class PipelineCache {
 public:
  explicit PipelineCache(PlumeRenderer& renderer) : renderer_(renderer) {}
  ~PipelineCache();

  bool Initialize();
  void Shutdown();

  plume::RenderPipelineLayout* layout() const { return layout_.get(); }
  // Set 0: the shared memory and the frame's constant memory.
  static plume::RenderDescriptorSetDesc GetFrameSetDesc();

  struct Description {
    uint64_t vertex_shader;
    // 0 without a pixel shader.
    uint64_t pixel_shader;
    rex::graphics::xenos::PrimitiveType primitive_type;
    uint32_t color_count;
    plume::RenderFormat color_formats[4];
    bool has_depth;
    uint32_t color_mask;
    uint32_t blend_control[4];
    uint32_t depth_control;
    uint32_t stencil_ref_mask;
    uint32_t stencil_ref_mask_bf;
    uint32_t cull_mode;
    uint32_t sample_count;
    bool alpha_to_coverage;
    // Polygon offset, in host depth units and host pixels.
    int32_t depth_bias;
    float depth_bias_slope_scaled;
  };
  // Returns null if the pipeline can't be created. Thread-safe: pipelines are
  // created outside the lock, and a pipeline another thread is creating is
  // waited for.
  plume::RenderPipeline* GetPipeline(const Description& description,
                                     const CompiledShader& vertex_shader,
                                     const CompiledShader* pixel_shader);

  size_t pipeline_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pipelines_.size();
  }

  // The pipelines created in earlier sessions are recorded in `directory`
  // and created again in the background at startup (once their shaders are
  // compiled), so the game doesn't stall creating them when first drawn.
  void StartPrewarm(const std::filesystem::path& directory, ShaderCache& shader_cache);

 private:
  plume::RenderShader* GetShader(uint64_t hash, const CompiledShader& compiled);
  void StopPrewarm();

  // Bump when Description or the pipeline state built from it changes.
  static constexpr uint32_t kRecordVersion = 1;

  PlumeRenderer& renderer_;
  std::unique_ptr<plume::RenderPipelineLayout> layout_;
  std::unique_ptr<plume::RenderShader> rectangle_list_shader_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders_;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines_;
  mutable std::mutex mutex_;
  std::condition_variable created_condition_;
  std::unordered_set<uint64_t> creating_;
  std::filesystem::path record_path_;
  std::thread prewarm_thread_;
  std::atomic<bool> prewarm_stop_{false};
};

}  // namespace hajinmon::renderer
