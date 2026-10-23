// Hajinmon native renderer - command processor.

#include "plume_command_processor.h"

#include <cmath>

#include <rex/logging/macros.h>
#include <xxhash.h>

#include "plume_graphics_system.h"

namespace hajinmon::renderer {

using rex::graphics::Shader;
namespace xenos = rex::graphics::xenos;

PlumeCommandProcessor::PlumeCommandProcessor(PlumeGraphicsSystem* graphics_system,
                                             rex::system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), plume_graphics_system_(graphics_system) {}

PlumeCommandProcessor::~PlumeCommandProcessor() = default;

bool PlumeCommandProcessor::SetupContext() {
  stats_start_ = std::chrono::steady_clock::now();
  return plume_graphics_system_->renderer().Initialize();
}

void PlumeCommandProcessor::ShutdownContext() {
  plume_graphics_system_->renderer().Shutdown();
  shaders_.clear();
}

Shader* PlumeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  // Milestone 2 maps the guest address to the shader container in the
  // executable, whose precompiled HLSL replaces the microcode.
  REXLOG_DEBUG("Plume: new {} shader {:016X} at 0x{:08X} ({} dwords)",
               shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, guest_address,
               dword_count);
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

bool PlumeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  ++draws_;
  return true;
}

bool PlumeCommandProcessor::IssueCopy() {
  ++copies_;
  return true;
}

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  (void)frontbuffer_ptr;
  (void)frontbuffer_width;
  (void)frontbuffer_height;
  ++frames_;
  // A slowly pulsing dark blue shows that frames flow through the renderer.
  float pulse = 0.5f + 0.5f * std::sin(float(frames_) * 0.05f);
  if (plume_graphics_system_->renderer().PresentClear(0.05f, 0.08f + 0.06f * pulse, 0.2f + 0.2f * pulse)) {
    ++presented_;
  }
  LogStats();
}

void PlumeCommandProcessor::LogStats() {
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - stats_start_).count();
  if (seconds < 5.0) {
    return;
  }
  REXLOG_INFO(
      "Plume ({}): {:.1f} guest FPS, {:.0f} draws and {:.1f} resolves per frame, {} presented, "
      "{} shaders",
      plume_graphics_system_->renderer().backend_name(), double(frames_) / seconds,
      frames_ ? double(draws_) / double(frames_) : 0.0,
      frames_ ? double(copies_) / double(frames_) : 0.0, presented_, shaders_.size());
  frames_ = draws_ = copies_ = presented_ = 0;
  stats_start_ = now;
}

}  // namespace hajinmon::renderer
