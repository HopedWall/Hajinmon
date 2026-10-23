// Hajinmon native renderer - command processor.

#include "plume_command_processor.h"

#include <cmath>

#include <fmt/format.h>
#include <rex/logging/macros.h>
#include <rex/system/xmemory.h>
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
  PlumeRenderer& renderer = plume_graphics_system_->renderer();
  if (!renderer.Initialize()) {
    return false;
  }
  shader_cache_ = std::make_unique<ShaderCache>(renderer.is_vulkan() ? ShaderCompiler::Target::kSpirv
                                                                     : ShaderCompiler::Target::kDxil);
  return true;
}

void PlumeCommandProcessor::ShutdownContext() {
  shader_cache_.reset();
  shaders_.clear();
  plume_graphics_system_->renderer().Shutdown();
}

void PlumeCommandProcessor::EnsureCatalog() {
  if (catalog_.built()) {
    return;
  }
  // Called once the executable is loaded.
  constexpr uint32_t kImageBase = 0x82000000, kImageSize = 0x01600000;
  const auto* image = plume_graphics_system_->memory()->TranslateVirtual<const uint8_t*>(kImageBase);
  catalog_.Build(image, kImageBase, kImageSize);
  size_t vertex = 0;
  for (const CatalogShader& entry : catalog_.shaders()) {
    vertex += entry.is_vertex_shader;
  }
  REXLOG_INFO("Plume: {} shaders in the executable ({} vertex, {} pixel)", catalog_.shaders().size(),
              vertex, catalog_.shaders().size() - vertex);
}

void PlumeCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  (void)blocking;
  if (!shader_cache_) {
    return;
  }
  EnsureCatalog();
  std::filesystem::path directory = cache_root / "plume" / fmt::format("{:08X}", title_id) / "shaders";
  shader_cache_->SetStorageDirectory(directory);
  REXLOG_INFO("Plume: shader cache in {}", directory.string());
  std::vector<ShaderCache::PrewarmShader> prewarm;
  auto* memory = plume_graphics_system_->memory();
  for (const CatalogShader& entry : catalog_.shaders()) {
    prewarm.push_back({memory->TranslateVirtual<const uint32_t*>(entry.ucode_address),
                       entry.ucode_dword_count, entry.is_vertex_shader});
  }
  shader_cache_->StartPrewarm(std::move(prewarm));
}

Shader* PlumeCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  EnsureCatalog();
  uint64_t hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  REXLOG_DEBUG("Plume: new {} shader {:016X} at 0x{:08X} ({} dwords)",
               shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel", hash, guest_address,
               dword_count);
  auto shader = std::make_unique<Shader>(shader_type, hash, host_address, dword_count);
  if (shader_cache_) {
    shader_cache_->RequestBuild(hash, shader_type, host_address, dword_count);
  }
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
      "{} shaders ({} compiled, {} from disk, {} failed, {} pending)",
      plume_graphics_system_->renderer().backend_name(), double(frames_) / seconds,
      frames_ ? double(draws_) / double(frames_) : 0.0,
      frames_ ? double(copies_) / double(frames_) : 0.0, presented_, shaders_.size(),
      shader_cache_ ? shader_cache_->stats().compiled.load() : 0,
      shader_cache_ ? shader_cache_->stats().loaded.load() : 0,
      shader_cache_ ? shader_cache_->stats().failed.load() : 0,
      shader_cache_ ? shader_cache_->pending_builds() : 0);
  frames_ = draws_ = copies_ = presented_ = 0;
  stats_start_ = now;
}

}  // namespace hajinmon::renderer
