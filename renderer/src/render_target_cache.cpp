// Hajinmon native renderer - render targets and resolves.

#include "render_target_cache.h"

#include <algorithm>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging/macros.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/math.h>

REXCVAR_DEFINE_INT32(plume_msaa, 4, "GPU/Plume",
                     "Samples per pixel of the surfaces the game renders with MSAA (2, 4 or 8; "
                     "at least what the game uses)")
    .range(1, 8);

namespace hajinmon::renderer {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;
using rex::graphics::draw_util::ResolveInfo;

namespace {

// Minimum rows of a new render target (in guest pixels); grown when a draw or
// resolve needs more.
constexpr uint32_t kMinRenderTargetHeight = 736;
constexpr uint32_t kMaxRenderTargetHeight = 4096;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
// Depth resolves: the depth buffer copied to a float color texture.
constexpr plume::RenderFormat kResolvedDepthFormat = plume::RenderFormat::R32_FLOAT;
const char kDepthCopyShaders[] = R"(
struct DepthCopyConstants {
  int2 source_offset;
};
[[vk::push_constant]] ConstantBuffer<DepthCopyConstants> depth_copy : register(b0, space1);
#ifdef MULTISAMPLED
Texture2DMS<float> depth_source : register(t0, space0);
#else
Texture2D<float> depth_source : register(t0, space0);
#endif

#ifdef VERTEX
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
  float2 uv = float2((vertex_id << 1u) & 2u, vertex_id & 2u);
  return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
#else
float main(float4 position : SV_Position) : SV_Target0 {
#ifdef MULTISAMPLED
  // The first sample, like a Xenos resolve of sample 0.
  return depth_source.Load(int2(position.xy) + depth_copy.source_offset, 0);
#else
  return depth_source.Load(int3(int2(position.xy) + depth_copy.source_offset, 0));
#endif
}
#endif
)";

// Finds the position of the 32x32 tile at this byte offset in a tiled 2D
// texture.
bool FindTile(uint32_t offset, uint32_t pitch, uint32_t height, uint32_t bpp_log2, uint32_t& x_out,
              uint32_t& y_out) {
  for (uint32_t y = 0; y < height; y += 32) {
    for (uint32_t x = 0; x < pitch; x += 32) {
      if (uint32_t(rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch,
                                                                 bpp_log2)) == offset) {
        x_out = x;
        y_out = y;
        return true;
      }
    }
  }
  return false;
}

bool IsColorFormat64bpp(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return true;
    default:
      return false;
  }
}

}  // namespace

plume::RenderFormat RenderTargetCache::GetHostColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return plume::RenderFormat::R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      // No 10:10:10:2 format in Plume.
      return plume::RenderFormat::R16G16B16A16_UNORM;
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return plume::RenderFormat::R16G16_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return plume::RenderFormat::R32_FLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return plume::RenderFormat::R32G32_FLOAT;
    default:
      return plume::RenderFormat::R16G16B16A16_FLOAT;
  }
}

uint32_t RenderTargetCache::GetHostSampleCount(xenos::MsaaSamples msaa, plume::RenderFormat format) {
  uint32_t samples = msaa >= xenos::MsaaSamples::k4X ? 4 : msaa >= xenos::MsaaSamples::k2X ? 2 : 1;
  // Surfaces the game renders with MSAA can use more samples.
  if (samples > 1) {
    samples = std::max(samples, uint32_t(std::clamp(REXCVAR_GET(plume_msaa), 1, 8)));
  }
  plume::RenderSampleCounts supported = renderer_.device()->getSampleCountsSupported(format);
  while (samples > 1 && !(supported & samples)) {
    samples >>= 1;
  }
  return samples;
}

TrackedTexture* RenderTargetCache::GetRenderTarget(uint32_t base_tiles, uint32_t format,
                                                   bool is_depth, uint32_t pitch,
                                                   xenos::MsaaSamples msaa, uint32_t min_height) {
  plume::RenderFormat host_format =
      is_depth ? kDepthFormat : GetHostColorFormat(xenos::ColorRenderTargetFormat(format));
  uint32_t samples = GetHostSampleCount(msaa, host_format);
  uint64_t key = uint64_t(base_tiles & 0xFFF) | (uint64_t(format & 0xF) << 12) |
                 (uint64_t(is_depth) << 16) | (uint64_t(pitch & 0x3FFF) << 17) |
                 (uint64_t(samples) << 31);
  min_height = std::min(min_height, kMaxRenderTargetHeight);
  std::unique_ptr<TrackedTexture>& target = render_targets_[key];
  if (target && target->height >= min_height * scale_) {
    return target.get();
  }
  uint32_t height = std::max(min_height, kMinRenderTargetHeight);
  if (target) {
    // Grown (the content is lost). The current frame may still use the old
    // texture and its framebuffers: they are destroyed once it completes.
    ForgetFramebuffers(target.get());
    renderer_.Retire(std::move(target->texture));
    height = std::max(height, target->height / scale_ * 2);
  }
  height = std::min((height + 15) & ~15u, kMaxRenderTargetHeight);
  target = std::make_unique<TrackedTexture>();
  target->width = std::clamp(pitch, 1u, 8192u) * scale_;
  target->height = height * scale_;
  target->format = host_format;
  target->samples = samples;
  plume::RenderMultisampling multisampling;
  multisampling.sampleCount = samples;
  target->texture = renderer_.device()->createTexture(
      is_depth ? plume::RenderTextureDesc::DepthTarget(target->width, target->height, host_format,
                                                       multisampling)
               : plume::RenderTextureDesc::ColorTarget(target->width, target->height, host_format,
                                                       multisampling));
  if (!target->texture) {
    target.reset();
    return nullptr;
  }
  target->texture->setName(fmt::format("EDRAM {} {} format {} pitch {}", is_depth ? "depth" : "color",
                                       base_tiles, format, pitch));
  REXLOG_DEBUG("Plume: new {} render target at EDRAM tile {}, format {}, pitch {}",
               is_depth ? "depth" : "color", base_tiles, format, pitch);
  return target.get();
}

bool RenderTargetCache::Initialize() {
  plume::RenderDevice* device = renderer_.device();
  plume::RenderDescriptorRange range(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
  plume::RenderDescriptorSetDesc set_desc(&range, 1);
  plume::RenderPushConstantRange push_constants(0, 1, 0, 8, plume::RenderShaderStageFlag::PIXEL);
  depth_copy_layout_ =
      device->createPipelineLayout(plume::RenderPipelineLayoutDesc(&push_constants, 1, &set_desc, 1));
  for (auto& sets : depth_copy_sets_) {
    for (auto& set : sets) {
      set = device->createDescriptorSet(set_desc);
    }
  }
  if (!depth_copy_layout_) {
    return false;
  }
  // Without and with MSAA on the source.
  for (uint32_t multisampled = 0; multisampled < 2; ++multisampled) {
    std::string source =
        std::string(multisampled ? "#define MULTISAMPLED 1\n" : "") + kDepthCopyShaders;
    auto vertex_shader =
        renderer_.CompileBuiltinShader(source.c_str(), ShaderCompiler::Stage::kVertex);
    auto pixel_shader = renderer_.CompileBuiltinShader(source.c_str(), ShaderCompiler::Stage::kPixel);
    if (!vertex_shader || !pixel_shader) {
      return false;
    }
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = depth_copy_layout_.get();
    desc.vertexShader = vertex_shader.get();
    desc.pixelShader = pixel_shader.get();
    desc.renderTargetFormat[0] = kResolvedDepthFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    desc.renderTargetCount = 1;
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    depth_copy_pipelines_[multisampled] = device->createGraphicsPipeline(desc);
    if (!depth_copy_pipelines_[multisampled]) {
      return false;
    }
  }
  return true;
}

void RenderTargetCache::ForgetFramebuffers(TrackedTexture* texture) {
  for (auto fb = framebuffers_.begin(); fb != framebuffers_.end();) {
    const auto& k = fb->first;
    bool uses = std::get<0>(k) == texture || std::get<1>(k) == texture ||
                std::get<2>(k) == texture || std::get<3>(k) == texture || std::get<4>(k) == texture;
    if (uses) {
      renderer_.Retire(std::move(fb->second));
      fb = framebuffers_.erase(fb);
    } else {
      ++fb;
    }
  }
  depth_views_.erase(texture);
}

void RenderTargetCache::CopyDepth(TrackedTexture& source, const plume::RenderRect& source_rect,
                                  TrackedTexture& dest, int32_t dest_x, int32_t dest_y) {
  // A descriptor set per copy within a frame, as earlier copies of the frame
  // still use theirs.
  if (renderer_.frame_number() != depth_copy_frame_) {
    depth_copy_frame_ = renderer_.frame_number();
    depth_copy_count_ = 0;
  }
  if (depth_copy_count_ >= kMaxDepthCopiesPerFrame) {
    return;
  }
  plume::RenderDescriptorSet* set =
      depth_copy_sets_[renderer_.frame_slot()][depth_copy_count_++].get();
  std::unique_ptr<plume::RenderTextureView>& view = depth_views_[&source];
  if (!view) {
    view = source.texture->createTextureView(plume::RenderTextureViewDesc::Texture2D(kDepthFormat));
  }
  plume::RenderCommandList* list = renderer_.command_list();
  source.Transition(list, plume::RenderTextureLayout::SHADER_READ);
  dest.Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
  set->setTexture(0, source.texture.get(), plume::RenderTextureLayout::SHADER_READ, view.get());
  TrackedTexture* dest_pointer = &dest;
  list->setFramebuffer(GetFramebuffer(&dest_pointer, 1, nullptr));
  int32_t width = source_rect.right - source_rect.left, height = source_rect.bottom - source_rect.top;
  list->setViewports(plume::RenderViewport(float(dest_x), float(dest_y), float(width), float(height)));
  list->setScissors(plume::RenderRect(dest_x, dest_y, dest_x + width, dest_y + height));
  list->setPipeline(depth_copy_pipelines_[source.samples > 1 ? 1 : 0].get());
  list->setGraphicsPipelineLayout(depth_copy_layout_.get());
  list->setGraphicsDescriptorSet(set, 0);
  int32_t offset[2] = {source_rect.left - dest_x, source_rect.top - dest_y};
  list->setGraphicsPushConstants(0, offset);
  list->drawInstanced(3, 1, 0, 0);
}

plume::RenderFramebuffer* RenderTargetCache::GetFramebuffer(TrackedTexture* const* colors,
                                                            uint32_t color_count,
                                                            TrackedTexture* depth) {
  TrackedTexture* c[4] = {};
  std::copy(colors, colors + color_count, c);
  auto key = std::make_tuple(c[0], c[1], c[2], c[3], depth);
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return it->second.get();
  }
  const plume::RenderTexture* attachments[4] = {};
  for (uint32_t i = 0; i < color_count; ++i) {
    attachments[i] = colors[i]->texture.get();
  }
  plume::RenderFramebufferDesc desc;
  desc.colorAttachments = attachments;
  desc.colorAttachmentsCount = color_count;
  desc.depthAttachment = depth ? depth->texture.get() : nullptr;
  auto framebuffer = renderer_.device()->createFramebuffer(desc);
  plume::RenderFramebuffer* result = framebuffer.get();
  framebuffers_.emplace(key, std::move(framebuffer));
  return result;
}

bool RenderTargetCache::Update(const rex::graphics::RegisterFile& regs, uint32_t color_mask,
                               bool use_depth, uint32_t height, Binding& binding_out) {
  binding_out = Binding();
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface_info.surface_pitch;
  if (!pitch) {
    return false;
  }
  plume::RenderCommandList* list = renderer_.command_list();
  // Color targets must be contiguous from 0 for now.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (!(color_mask & (0xFu << (i * 4)))) {
      break;
    }
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    TrackedTexture* target = GetRenderTarget(color_info.color_base, uint32_t(color_info.color_format),
                                             false, pitch, surface_info.msaa_samples, height);
    if (!target) {
      return false;
    }
    target->Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
    binding_out.color[binding_out.color_count++] = target;
  }
  if (use_depth) {
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    binding_out.depth = GetRenderTarget(depth_info.depth_base, uint32_t(depth_info.depth_format),
                                        true, pitch, surface_info.msaa_samples, height);
    if (!binding_out.depth) {
      return false;
    }
    binding_out.depth->Transition(list, plume::RenderTextureLayout::DEPTH_WRITE);
  }
  if (!binding_out.color_count && !binding_out.depth) {
    return false;
  }
  binding_out.framebuffer =
      GetFramebuffer(binding_out.color, binding_out.color_count, binding_out.depth);
  binding_out.samples =
      binding_out.color_count ? binding_out.color[0]->samples : binding_out.depth->samples;
  // The area all bound targets cover.
  binding_out.width = binding_out.height = UINT32_MAX;
  for (uint32_t i = 0; i < binding_out.color_count; ++i) {
    binding_out.width = std::min(binding_out.width, binding_out.color[i]->width);
    binding_out.height = std::min(binding_out.height, binding_out.color[i]->height);
  }
  if (binding_out.depth) {
    binding_out.width = std::min(binding_out.width, binding_out.depth->width);
    binding_out.height = std::min(binding_out.height, binding_out.depth->height);
  }
  return binding_out.framebuffer != nullptr;
}

RenderTargetCache::Resolved* RenderTargetCache::FindResolved(uint32_t address) {
  auto it = resolved_.find(address & 0x1FFFFFFF);
  return it != resolved_.end() ? it->second.get() : nullptr;
}

void RenderTargetCache::Resolve(const rex::graphics::RegisterFile& regs, const ResolveInfo& info) {
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface_info.surface_pitch;
  if (!pitch || !info.coordinate_info.width_div_8 || !info.height_div_8) {
    return;
  }
  plume::RenderCommandList* list = renderer_.command_list();
  uint32_t width = info.coordinate_info.width_div_8 * 8;
  uint32_t height = info.height_div_8 * 8;
  bool msaa_2x = surface_info.msaa_samples >= xenos::MsaaSamples::k2X;
  bool msaa_4x = surface_info.msaa_samples >= xenos::MsaaSamples::k4X;
  int32_t scale = int32_t(scale_);

  // Source rectangle, in guest pixels, in the render target whose base is the
  // original one.
  auto source_rect = [&](const rex::graphics::draw_util::ResolveEdramInfo& edram,
                         uint32_t original_base) {
    uint32_t tile_width = (xenos::kEdramTileWidthSamples >> uint32_t(msaa_4x)) >>
                          uint32_t(edram.format_is_64bpp);
    uint32_t tile_height = xenos::kEdramTileHeightSamples >> uint32_t(msaa_2x);
    uint32_t relative_tiles = (edram.base_tiles - original_base) & (xenos::kEdramTileCount - 1);
    uint32_t pitch_tiles = std::max(uint32_t(edram.pitch_tiles), 1u);
    int32_t x = int32_t((relative_tiles % pitch_tiles) * tile_width +
                        info.coordinate_info.edram_offset_x_div_8 * 8);
    int32_t y = int32_t((relative_tiles / pitch_tiles) * tile_height +
                        info.coordinate_info.edram_offset_y_div_8 * 8);
    return plume::RenderRect(x, y, x + int32_t(width), y + int32_t(height));
  };
  auto scaled = [&](plume::RenderRect rect) {
    return plume::RenderRect(rect.left * scale, rect.top * scale, rect.right * scale,
                             rect.bottom * scale);
  };

  if (info.copy_dest_extent_length) {
    bool is_depth = info.IsCopyingDepth();
    TrackedTexture* source;
    plume::RenderRect rect;
    if (is_depth) {
      rect = source_rect(info.depth_edram_info, info.depth_original_base);
      source = GetRenderTarget(info.depth_original_base,
                               uint32_t(regs.Get<reg::RB_DEPTH_INFO>().depth_format), true, pitch,
                               surface_info.msaa_samples, uint32_t(rect.bottom));
    } else {
      rect = source_rect(info.color_edram_info, info.color_original_base);
      source = GetRenderTarget(info.color_original_base, uint32_t(info.color_edram_info.format), false,
                               pitch, surface_info.msaa_samples, uint32_t(rect.bottom));
    }
    uint32_t dest_width = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
    uint32_t dest_height = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
    // The destination: the texture at RB_COPY_DEST_BASE, or, if that address is
    // inside a texture resolved before (games resolving tiles of a texture
    // separately), that texture. The SDK moves the part of the destination
    // position above the tiling granularity into copy_dest_base.
    uint32_t bpp_log2 = 2;
    if (const auto* format_info =
            rex::graphics::FormatInfo::Get(uint32_t(info.copy_dest_info.copy_dest_format))) {
      bpp_log2 = rex::log2_floor(std::max(format_info->bits_per_pixel >> 3, 1u));
    }
    uint32_t dest_base = regs.values[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
    int32_t dest_x = int32_t(info.copy_dest_coordinate_info.offset_x_div_8 * 8);
    int32_t dest_y = int32_t(info.copy_dest_coordinate_info.offset_y_div_8 * 8);
    uint32_t tile_x, tile_y;
    if (FindTile((info.copy_dest_base & 0x1FFFFFFF) - dest_base, dest_width, dest_height, bpp_log2,
                 tile_x, tile_y)) {
      dest_x += int32_t(tile_x);
      dest_y += int32_t(tile_y);
    }
    for (const auto& [address, entry] : resolved_) {
      uint32_t entry_size = (entry->guest_width * entry->guest_height) << bpp_log2;
      if (dest_base > address && dest_base < address + entry_size &&
          entry->guest_width == dest_width &&
          FindTile(dest_base - address, entry->guest_width, entry->guest_height, bpp_log2, tile_x,
                   tile_y)) {
        dest_base = address;
        dest_height = entry->guest_height;
        dest_x += int32_t(tile_x);
        dest_y += int32_t(tile_y);
        break;
      }
    }
    // Depth is resolved to the 0...1 depth as a float, as the game samples it
    // (k_24_8 textures).
    plume::RenderFormat dest_format = is_depth ? kResolvedDepthFormat
                                      : source             ? source->format
                                                           : plume::RenderFormat::UNKNOWN;
    if (source && dest_width && dest_height) {
      // Clip to both textures, in host pixels.
      plume::RenderRect host_rect = scaled(rect);
      int32_t host_dest_x = dest_x * scale, host_dest_y = dest_y * scale;
      host_rect.right = std::min({host_rect.right, int32_t(source->width),
                                  host_rect.left + int32_t(dest_width) * scale - host_dest_x});
      host_rect.bottom = std::min({host_rect.bottom, int32_t(source->height),
                                   host_rect.top + int32_t(dest_height) * scale - host_dest_y});
      if (host_rect.right > host_rect.left && host_rect.bottom > host_rect.top) {
        std::unique_ptr<Resolved>& resolved = resolved_[dest_base];
        if (!resolved || resolved->guest_width != dest_width ||
            resolved->guest_height != dest_height || resolved->texture.format != dest_format) {
          if (resolved) {
            ForgetFramebuffers(&resolved->texture);
            renderer_.Retire(std::move(resolved->texture.texture));
          }
          resolved = std::make_unique<Resolved>();
          resolved->guest_width = dest_width;
          resolved->guest_height = dest_height;
          resolved->texture.width = dest_width * scale_;
          resolved->texture.height = dest_height * scale_;
          resolved->texture.format = dest_format;
          resolved->texture.texture = renderer_.device()->createTexture(
              is_depth ? plume::RenderTextureDesc::ColorTarget(resolved->texture.width,
                                                               resolved->texture.height, dest_format)
                       : plume::RenderTextureDesc::Texture2D(resolved->texture.width,
                                                             resolved->texture.height, 1, dest_format));
          resolved->texture.texture->setName(fmt::format("Resolve {:08X}", dest_base));
        }
        resolved->version = ++resolve_version_;
        resolved->swap_red_blue = !is_depth && info.copy_dest_info.copy_dest_swap != 0;
        if (is_depth) {
          CopyDepth(*source, host_rect, resolved->texture, host_dest_x, host_dest_y);
        } else {
          if (source->samples > 1) {
            // Averages the samples, as the Xenos resolve does by default.
            source->Transition(list, plume::RenderTextureLayout::RESOLVE_SOURCE);
            resolved->texture.Transition(list, plume::RenderTextureLayout::RESOLVE_DEST);
            list->resolveTextureRegion(resolved->texture.texture.get(), uint32_t(host_dest_x),
                                       uint32_t(host_dest_y), source->texture.get(), &host_rect);
          } else {
            source->Transition(list, plume::RenderTextureLayout::COPY_SOURCE);
            resolved->texture.Transition(list, plume::RenderTextureLayout::COPY_DEST);
            plume::RenderBox box(host_rect.left, host_rect.top, host_rect.right, host_rect.bottom);
            list->copyTextureRegion(
                plume::RenderTextureCopyLocation::Subresource(resolved->texture.texture.get()),
                plume::RenderTextureCopyLocation::Subresource(source->texture.get()),
                uint32_t(host_dest_x), uint32_t(host_dest_y), 0, &box);
          }
        }
      }
    }
  }

  if (info.IsClearingColor()) {
    auto format = xenos::ColorRenderTargetFormat(info.color_edram_info.format);
    plume::RenderRect rect = source_rect(info.color_edram_info, info.color_original_base);
    TrackedTexture* target = GetRenderTarget(info.color_original_base, uint32_t(format), false, pitch,
                                             surface_info.msaa_samples, uint32_t(rect.bottom));
    if (target) {
      plume::RenderColor color;
      if (IsColorFormat64bpp(format)) {
        // Two 16-bit halves per register, red in the low half of the low one.
        auto half = [](uint32_t v) { return rex::xenos_half_to_float(uint16_t(v)); };
        color = plume::RenderColor(half(info.rb_color_clear_lo), half(info.rb_color_clear_lo >> 16),
                                   half(info.rb_color_clear), half(info.rb_color_clear >> 16));
      } else {
        uint32_t v = info.rb_color_clear;
        color = plume::RenderColor(float(v & 0xFF) / 255.0f, float((v >> 8) & 0xFF) / 255.0f,
                                   float((v >> 16) & 0xFF) / 255.0f, float(v >> 24) / 255.0f);
      }
      plume::RenderRect host_rect = scaled(rect);
      target->Transition(list, plume::RenderTextureLayout::COLOR_WRITE);
      list->setFramebuffer(GetFramebuffer(&target, 1, nullptr));
      list->clearColor(0, color, &host_rect, 1);
    }
  }
  if (info.IsClearingDepth()) {
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    plume::RenderRect rect = source_rect(info.depth_edram_info, info.depth_original_base);
    TrackedTexture* target =
        GetRenderTarget(info.depth_original_base, uint32_t(depth_info.depth_format), true, pitch,
                        surface_info.msaa_samples, uint32_t(rect.bottom));
    if (target) {
      plume::RenderRect host_rect = scaled(rect);
      target->Transition(list, plume::RenderTextureLayout::DEPTH_WRITE);
      list->setFramebuffer(GetFramebuffer(nullptr, 0, target));
      list->clearDepthStencil(true, true, float(info.rb_depth_clear >> 8) / float(0xFFFFFF),
                              info.rb_depth_clear & 0xFF, &host_rect, 1);
    }
  }
}

}  // namespace hajinmon::renderer
