// Hajinmon native renderer - command processor.
//
// The SDK base class parses the GPU command stream (PM4 packets) on its own
// thread and calls the Issue* methods for draws, resolves and frame swaps.
//
// Milestone 2: shaders are translated to HLSL and compiled in the background
// as the game loads them. Nothing is drawn yet: draws and resolves are counted, and every swap
// presents a cleared frame.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/pipeline/shader/shader.h>

#include "shader_cache.h"
#include "shader_catalog.h"

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
  void LogStats();
  void EnsureCatalog();

  PlumeGraphicsSystem* plume_graphics_system_;
  ShaderCatalog catalog_;
  std::unique_ptr<ShaderCache> shader_cache_;
  std::unordered_map<uint64_t, std::unique_ptr<rex::graphics::Shader>> shaders_;

  // Statistics, logged every few seconds.
  uint64_t frames_ = 0;
  uint64_t draws_ = 0;
  uint64_t copies_ = 0;
  uint64_t presented_ = 0;
  std::chrono::steady_clock::time_point stats_start_;
};

}  // namespace hajinmon::renderer
