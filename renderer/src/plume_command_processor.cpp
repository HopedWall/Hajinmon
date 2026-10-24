// Hajinmon native renderer - command processor.

#include "plume_command_processor.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging/macros.h>
#include <rex/string/buffer.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

#include "plume_graphics_system.h"

REXCVAR_DEFINE_BOOL(plume_async_shaders, false, "GPU/Plume",
                    "Skip draws whose shaders are still compiling instead of waiting for them");

namespace hajinmon::renderer {

using rex::graphics::PrimitiveProcessor;
using rex::graphics::Shader;
namespace draw_util = rex::graphics::draw_util;
namespace reg = rex::graphics::reg;
namespace xenos = rex::graphics::xenos;

namespace {

// Read by xe_load_system_constants in the translated shaders.
struct SystemConstants {
  uint32_t flags;
  int32_t vertex_index_offset;
  uint32_t ps_param_gen;
  uint32_t alpha_test;
  float ndc_scale[4];
  float ndc_offset[4];
  float alpha_ref;
  uint32_t vertex_index_endian;
  // Host pixels per guest pixel (plume_resolution_scale).
  float resolution_scale;
  uint32_t padding;
  // Texture index | sampler index << 20, and swizzle, per texture slot.
  uint32_t vertex_textures[HlslShaderTranslator::kMaxTextureSlots][2];
  uint32_t pixel_textures[HlslShaderTranslator::kMaxTextureSlots][2];
  // RB_BLENDCONTROL of each color target, for min/max blending.
  uint32_t blend_control[4];
};
static_assert(sizeof(SystemConstants) == 336);

}  // namespace

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
  shader_cache_ = std::make_unique<ShaderCache>(renderer.shader_target());
  shared_memory_ = std::make_unique<PlumeSharedMemory>(*memory_, renderer);
  if (!shared_memory_->Initialize()) {
    return false;
  }
  primitive_processor_ =
      std::make_unique<PlumePrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, renderer);
  if (!primitive_processor_->Initialize()) {
    REXLOG_ERROR("Plume: could not initialize the primitive processor");
    return false;
  }
  render_target_cache_ = std::make_unique<RenderTargetCache>(renderer);
  if (!render_target_cache_->Initialize()) {
    REXLOG_ERROR("Plume: could not create the depth resolve pipeline");
    return false;
  }
  texture_cache_ =
      std::make_unique<TextureCache>(renderer, *shared_memory_, *memory_, *render_target_cache_);
  if (!texture_cache_->Initialize()) {
    REXLOG_ERROR("Plume: could not initialize the texture cache");
    return false;
  }
  pipeline_cache_ = std::make_unique<PipelineCache>(renderer);
  if (!pipeline_cache_->Initialize()) {
    REXLOG_ERROR("Plume: could not create the pipeline layout");
    return false;
  }
  for (uint32_t i = 0; i < PlumeRenderer::kFramesInFlight; ++i) {
    frame_sets_[i] = renderer.device()->createDescriptorSet(PipelineCache::GetFrameSetDesc());
    frame_sets_[i]->setBuffer(0, shared_memory_->buffer(), PlumeSharedMemory::kBufferSize);
    frame_sets_[i]->setBuffer(1, renderer.constant_buffer(i), PlumeRenderer::kConstantMemorySize);
  }
  return true;
}

void PlumeCommandProcessor::ShutdownContext() {
  shader_cache_.reset();
  for (auto& set : frame_sets_) {
    set.reset();
  }
  pipeline_cache_.reset();
  texture_cache_.reset();
  render_target_cache_.reset();
  primitive_processor_.reset();
  shared_memory_.reset();
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
  // Shaders loaded in previous sessions first (the most likely to be needed),
  // then the executable's.
  std::vector<ShaderCache::PrewarmShader> prewarm = shader_cache_->LoadRecordedShaders();
  REXLOG_INFO("Plume: {} shaders recorded in previous sessions", prewarm.size());
  auto* memory = plume_graphics_system_->memory();
  for (const CatalogShader& entry : catalog_.shaders()) {
    const auto* ucode = memory->TranslateVirtual<const uint32_t*>(entry.ucode_address);
    prewarm.push_back({std::vector<uint32_t>(ucode, ucode + entry.ucode_dword_count),
                       entry.is_vertex_shader, true});
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
  // Draws need what the analysis finds (vertex buffers, outputs...).
  rex::string::StringBuffer disassembly;
  shader->AnalyzeUcode(disassembly);
  if (shader_cache_) {
    shader_cache_->RequestBuild(hash, shader_type, host_address, dword_count);
  }
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

uint32_t PlumeCommandProcessor::WriteBlock(Block block, const void* data, uint32_t size) {
  BlockState& state = blocks_[block];
  if (state.offset != UINT32_MAX && state.data.size() == size &&
      !std::memcmp(state.data.data(), data, size)) {
    return state.offset;
  }
  PlumeRenderer::Allocation allocation = plume_graphics_system_->renderer().AllocateConstants(size);
  if (!allocation.data) {
    return UINT32_MAX;
  }
  std::memcpy(allocation.data, data, size);
  state.data.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
  state.offset = uint32_t(allocation.offset);
  return state.offset;
}

bool PlumeCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  (void)prim_type;
  (void)index_count;
  (void)index_buffer_info;
  (void)major_mode_explicit;
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++draws_;
  if (Draw()) {
    ++draws_done_;
  }
  // Draws that can't be done yet are skipped, not treated as errors.
  return true;
}

bool PlumeCommandProcessor::Draw() {
  const rex::graphics::RegisterFile& regs = *register_file_;
  PlumeRenderer& renderer = plume_graphics_system_->renderer();
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  Shader* vertex_shader = active_vertex_shader();
  if (!vertex_shader) {
    return Skip(kSkipNoVertexShader);
  }
  bool polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, polygonal)) {
    // Memory export only, not supported yet.
    return Skip(kSkipNoRasterization);
  }
  Shader* pixel_shader = nullptr;
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    pixel_shader = active_pixel_shader();
    if (pixel_shader && !draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
      pixel_shader = nullptr;
    }
  }
  // New shaders are compiled in the background; by default the draw waits for
  // them (a short stall the first time a shader is seen) rather than being
  // skipped (missing objects for a moment).
  auto get_compiled = [&](const Shader* shader) {
    return REXCVAR_GET(plume_async_shaders)
               ? shader_cache_->Find(shader->ucode_data_hash())
               : shader_cache_->Wait(shader->ucode_data_hash(), std::chrono::seconds(5));
  };
  const CompiledShader* vertex_compiled = get_compiled(vertex_shader);
  const CompiledShader* pixel_compiled = pixel_shader ? get_compiled(pixel_shader) : nullptr;
  if (!vertex_compiled || !vertex_compiled->is_valid ||
      (pixel_shader && (!pixel_compiled || !pixel_compiled->is_valid))) {
    // Still compiling (or failed).
    return Skip(kSkipShaderNotReady);
  }

  renderer.command_list();
  PrimitiveProcessor::ProcessingResult primitive;
  if (!primitive_processor_->Process(primitive) || !primitive.host_draw_vertex_count ||
      primitive.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex) {
    return Skip(kSkipPrimitive);
  }

  // Render targets.
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  uint32_t color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets()) : 0;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  RenderTargetCache::Binding targets;
  if (!render_target_cache_->Update(regs, color_mask,
                                    depth_control.z_enable || depth_control.stencil_enable,
                                    scissor.offset[1] + scissor.extent[1], targets)) {
    return Skip(kSkipRenderTargets);
  }
  color_mask &= (1u << (targets.color_count * 4)) - 1;

  // Vertex buffers.
  for (const Shader::VertexBinding& binding : vertex_shader->vertex_bindings()) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) {
      continue;
    }
    shared_memory_->RequestRange(fetch.address << 2, fetch.size << 2);
  }

  // System constants and textures.
  SystemConstants system = {};
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  system.flags = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 0u : 4u);
  system.vertex_index_offset = int32_t(regs.Get<reg::VGT_INDX_OFFSET>().indx_offset << 8) >> 8;
  if (pixel_shader) {
    uint32_t param_gen = UINT32_MAX;
    pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                           regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen);
    system.ps_param_gen = param_gen == UINT32_MAX ? 0 : param_gen + 1;
  }
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  system.alpha_test = color_control.alpha_test_enable ? uint32_t(color_control.alpha_func) : 7;
  std::memcpy(&system.alpha_ref, &regs.values[rex::graphics::XE_GPU_REG_RB_ALPHA_REF], sizeof(float));
  system.vertex_index_endian = uint32_t(primitive.host_shader_index_endian);
  system.resolution_scale = float(render_target_cache_->scale());
  for (uint32_t i = 0; i < 4; ++i) {
    system.blend_control[i] = regs.values[reg::RB_BLENDCONTROL::rt_register_indices[i]];
  }
  draw_util::ViewportInfo viewport;
  uint32_t scale = render_target_cache_->scale();
  draw_util::GetHostViewportInfo(regs, scale, scale, true, 16384, 16384, false, depth_control, false, true,
                                 pixel_shader && pixel_shader->writes_depth(), viewport);
  for (uint32_t i = 0; i < 3; ++i) {
    system.ndc_scale[i] = viewport.ndc_scale[i];
    system.ndc_offset[i] = viewport.ndc_offset[i];
  }
  auto bind_textures = [&](const CompiledShader& compiled, uint32_t (*out)[2]) {
    for (size_t i = 0; i < compiled.texture_slots.size(); ++i) {
      const auto& slot = compiled.texture_slots[i];
      texture_cache_->Bind(regs.GetTextureFetch(slot.fetch_constant), slot.dimension, out[i][0],
                           out[i][1]);
    }
  };
  bind_textures(*vertex_compiled, system.vertex_textures);
  if (pixel_compiled) {
    bind_textures(*pixel_compiled, system.pixel_textures);
  }

  // Constant blocks.
  auto float_block = [&](uint32_t base, std::vector<uint8_t>& data) {
    data.assign(256 * 16, 0);
    base = std::min(base, 511u);
    uint32_t count = std::min(256u, 512u - base);
    std::memcpy(data.data(), &regs.values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X + base * 4], count * 16);
  };
  std::vector<uint8_t> vertex_floats, pixel_floats;
  float_block(regs.Get<reg::SQ_VS_CONST>().base, vertex_floats);
  float_block(regs.Get<reg::SQ_PS_CONST>().base, pixel_floats);
  uint32_t bool_loop[40];
  std::memcpy(bool_loop, &regs.values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 8 * 4);
  std::memcpy(bool_loop + 8, &regs.values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 32 * 4);
  uint32_t push[8] = {
      WriteBlock(kBlockSystem, &system, sizeof(system)),
      WriteBlock(kBlockVertexFloat, vertex_floats.data(), uint32_t(vertex_floats.size())),
      pixel_compiled
          ? WriteBlock(kBlockPixelFloat, pixel_floats.data(), uint32_t(pixel_floats.size()))
          : 0,
      WriteBlock(kBlockBoolLoop, bool_loop, sizeof(bool_loop)),
      WriteBlock(kBlockFetch, &regs.values[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 192 * 4),
  };
  for (uint32_t i = 0; i < 5; ++i) {
    if (push[i] == UINT32_MAX) {
      return Skip(kSkipConstantsFull);
    }
  }

  // Pipeline.
  PipelineCache::Description description = {};
  description.vertex_shader = vertex_shader->ucode_data_hash();
  description.pixel_shader = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  description.primitive_type = primitive.host_primitive_type;
  description.color_count = targets.color_count;
  for (uint32_t i = 0; i < targets.color_count; ++i) {
    description.color_formats[i] = targets.color[i]->format;
    description.blend_control[i] = regs.values[reg::RB_BLENDCONTROL::rt_register_indices[i]];
  }
  description.has_depth = targets.depth != nullptr;
  description.sample_count = targets.samples;
  // Alpha to coverage (alpha to mask), with MSAA only.
  description.alpha_to_coverage = targets.samples > 1 && color_control.alpha_to_mask_enable;
  description.color_mask = color_mask;
  description.depth_control = depth_control.value;
  description.stencil_ref_mask = regs.values[rex::graphics::XE_GPU_REG_RB_STENCILREFMASK];
  description.stencil_ref_mask_bf = regs.values[rex::graphics::XE_GPU_REG_RB_STENCILREFMASK_BF];
  if (polygonal && primitive.host_primitive_type != xenos::PrimitiveType::kRectangleList) {
    auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (mode.cull_front && mode.cull_back) {
      return Skip(kSkipCulled);
    }
    description.cull_mode = mode.cull_front | (mode.cull_back << 1) | (mode.face << 2);
  }
  // Polygon offset (coplanar geometry such as decals and layered scenery), as
  // in the SDK: the face that is drawn, in Direct3D integer units for the
  // guest depth format, and the slope for host pixels.
  if (targets.depth) {
    float polygon_offset_scale, polygon_offset;
    draw_util::GetPreferredFacePolygonOffset(regs, polygonal, polygon_offset_scale, polygon_offset);
    description.depth_bias = draw_util::GetD3D10IntegerPolygonOffset(
        regs.Get<reg::RB_DEPTH_INFO>().depth_format, polygon_offset);
    description.depth_bias_slope_scaled = polygon_offset_scale *
                                          xenos::kPolygonOffsetScaleSubpixelUnit *
                                          float(render_target_cache_->scale());
  }
  plume::RenderPipeline* pipeline =
      pipeline_cache_->GetPipeline(description, *vertex_compiled, pixel_compiled);
  if (!pipeline) {
    return Skip(kSkipPipeline);
  }

  // Draw.
  plume::RenderCommandList* list = renderer.command_list();
  list->setFramebuffer(targets.framebuffer);
  list->setGraphicsPipelineLayout(pipeline_cache_->layout());
  list->setPipeline(pipeline);
  list->setGraphicsDescriptorSet(frame_sets_[renderer.frame_slot()].get(), 0);
  for (uint32_t i = 0; i < TextureCache::kSetCount; ++i) {
    list->setGraphicsDescriptorSet(texture_cache_->descriptor_set(TextureCache::DescriptorSet(i)),
                                   1 + i);
  }
  list->setGraphicsPushConstants(0, push);
  list->setViewports(plume::RenderViewport(float(viewport.xy_offset[0]), float(viewport.xy_offset[1]),
                                           float(viewport.xy_extent[0]), float(viewport.xy_extent[1]),
                                           viewport.z_min, viewport.z_max));
  int32_t scissor_right =
      int32_t(std::min((scissor.offset[0] + scissor.extent[0]) * scale, targets.width));
  int32_t scissor_bottom =
      int32_t(std::min((scissor.offset[1] + scissor.extent[1]) * scale, targets.height));
  list->setScissors(plume::RenderRect(int32_t(scissor.offset[0] * scale),
                                      int32_t(scissor.offset[1] * scale), scissor_right,
                                      scissor_bottom));
  uint32_t count = primitive.host_draw_vertex_count;
  if (primitive.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    list->drawInstanced(count, 1, 0, 0);
    return true;
  }
  bool is_32bit = primitive.host_index_format == xenos::IndexFormat::kInt32;
  plume::RenderBufferReference index_buffer =
      primitive.index_buffer_type == PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA
          ? plume::RenderBufferReference(shared_memory_->buffer(), primitive.guest_index_base)
          : primitive_processor_->GetIndexBuffer(primitive);
  if (!index_buffer.ref) {
    return Skip(kSkipIndexBuffer);
  }
  plume::RenderIndexBufferView view(index_buffer, count * (is_32bit ? 4 : 2),
                                    is_32bit ? plume::RenderFormat::R32_UINT
                                             : plume::RenderFormat::R16_UINT);
  list->setIndexBuffer(&view);
  list->drawIndexedInstanced(count, 1, 0, 0, 0);
  return true;
}

bool PlumeCommandProcessor::IssueCopy() {
  ++copies_;
  draw_util::ResolveInfo info;
  if (draw_util::GetResolveInfo(*register_file_, *memory_, 1, 1, false, false, info)) {
    render_target_cache_->Resolve(*register_file_, info);
  }
  return true;
}

void PlumeCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  ++frames_;
  RenderTargetCache::Resolved* frontbuffer = render_target_cache_->FindResolved(frontbuffer_ptr);
  if (plume_graphics_system_->renderer().EndFrame(frontbuffer ? &frontbuffer->texture : nullptr,
                                                  frontbuffer_width * render_target_cache_->scale(),
                                                  frontbuffer_height * render_target_cache_->scale(),
                                                  // The display reads memory with red and
                                                  // blue swapped, as resolves write it.
                                                  false)) {
    ++presented_;
  }
  primitive_processor_->EndFrame();
  texture_cache_->EndFrame();
  for (BlockState& block : blocks_) {
    block.offset = UINT32_MAX;
  }
  LogStats();
}

void PlumeCommandProcessor::LogStats() {
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - stats_start_).count();
  if (seconds < 5.0) {
    return;
  }
  double frames = frames_ ? double(frames_) : 1.0;
  REXLOG_INFO(
      "Plume ({}): {:.1f} guest FPS, {:.0f}/{:.0f} draws done and {:.1f} resolves per frame, {} "
      "presented; {} shaders ({} compiled, {} from disk, {} failed, {} pending), {} pipelines",
      plume_graphics_system_->renderer().backend_name(), double(frames_) / seconds,
      double(draws_done_) / frames, double(draws_) / frames, double(copies_) / frames, presented_,
      shaders_.size(), shader_cache_->stats().compiled.load(), shader_cache_->stats().loaded.load(),
      shader_cache_->stats().failed.load(), shader_cache_->pending_builds(),
      pipeline_cache_->pipeline_count());
  static const char* const kSkipNames[kSkipCount] = {
      "no vertex shader", "no rasterization", "shader not ready", "primitive", "render targets",
      "constants full", "culled", "pipeline", "index buffer"};
  std::string skipped;
  for (uint32_t i = 0; i < kSkipCount; ++i) {
    if (skipped_[i]) {
      skipped += fmt::format(" {} {},", kSkipNames[i], skipped_[i]);
    }
    skipped_[i] = 0;
  }
  if (!skipped.empty()) {
    skipped.pop_back();
    REXLOG_INFO("Plume: skipped draws:{}", skipped);
  }
  std::string placeholders = texture_cache_->TakePlaceholderStats();
  if (!placeholders.empty()) {
    REXLOG_INFO("Plume: placeholder textures:{}", placeholders);
  }
  frames_ = draws_ = draws_done_ = copies_ = presented_ = 0;
  stats_start_ = now;
}

}  // namespace hajinmon::renderer
