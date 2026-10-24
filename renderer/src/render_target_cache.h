// Hajinmon native renderer - render targets and resolves.
//
// The "hybrid" part of the renderer: instead of emulating the Xenos EDRAM (a
// 10 MB tiled framebuffer that is copied, "resolved", to textures in main
// memory), each EDRAM surface the game renders to becomes a host render target,
// keyed by its EDRAM base, format and pitch. A resolve copies the render target
// region to a host texture associated with the destination address; that
// texture is then used directly when the game samples or presents that
// address, without going through guest memory.
//
// Surfaces the game renders with MSAA are multisampled on the host too
// (plume_msaa samples, at least the game's), resolved by averaging.
//
// Not handled yet: EDRAM aliasing between surfaces of different formats or
// bases, resolve format conversion and resolves of a single sample.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>

#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

#include "plume_renderer.h"

namespace hajinmon::renderer {

class RenderTargetCache {
 public:
  explicit RenderTargetCache(PlumeRenderer& renderer)
      : renderer_(renderer), scale_(renderer.resolution_scale()) {}

  // Host pixels per guest pixel along each axis.
  uint32_t scale() const { return scale_; }

  // Creates the pipeline of depth resolves.
  bool Initialize();

  struct Binding {
    TrackedTexture* color[rex::graphics::xenos::kMaxColorRenderTargets] = {};
    uint32_t color_count = 0;
    TrackedTexture* depth = nullptr;
    plume::RenderFramebuffer* framebuffer = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t samples = 1;
  };
  // Finds or creates the render targets of the current draw: color targets
  // 0...color_count - 1 (as the pixel shader writes them), and depth/stencil if
  // used. Transitions them for rendering. `height` is the number of guest rows
  // the draw needs. Binding sizes are in host pixels (scaled).
  bool Update(const rex::graphics::RegisterFile& regs, uint32_t color_mask, bool use_depth,
              uint32_t height, Binding& binding_out);

  // Performs the copy and clears of a resolve.
  void Resolve(const rex::graphics::RegisterFile& regs,
               const rex::graphics::draw_util::ResolveInfo& info);

  // The texture last resolved to this guest physical address, if any, with
  // a counter that changes on every resolve to it.
  struct Resolved {
    TrackedTexture texture;
    uint64_t version = 0;
    // RB_COPY_DEST_INFO::copy_dest_swap: red and blue are swapped in memory.
    bool swap_red_blue = false;
    // Size in guest pixels (the texture is scaled by the resolution scale).
    uint32_t guest_width = 0;
    uint32_t guest_height = 0;
  };
  Resolved* FindResolved(uint32_t address);

  static plume::RenderFormat GetHostColorFormat(rex::graphics::xenos::ColorRenderTargetFormat format);

 private:
  TrackedTexture* GetRenderTarget(uint32_t base_tiles, uint32_t format, bool is_depth,
                                  uint32_t pitch, rex::graphics::xenos::MsaaSamples msaa,
                                  uint32_t min_height);
  // Host samples per pixel for a guest MSAA mode.
  uint32_t GetHostSampleCount(rex::graphics::xenos::MsaaSamples msaa, plume::RenderFormat format);
  plume::RenderFramebuffer* GetFramebuffer(TrackedTexture* const* colors, uint32_t color_count,
                                           TrackedTexture* depth);
  // Drops (after the frame) the framebuffers using a texture about to be replaced.
  void ForgetFramebuffers(TrackedTexture* texture);
  // Copies depth to a float color texture with a draw (depth can't be copied
  // to a color format).
  void CopyDepth(TrackedTexture& source, const plume::RenderRect& source_rect, TrackedTexture& dest,
                 int32_t dest_x, int32_t dest_y);

  static constexpr uint32_t kMaxDepthCopiesPerFrame = 32;

  PlumeRenderer& renderer_;
  uint32_t scale_;
  std::unordered_map<uint64_t, std::unique_ptr<TrackedTexture>> render_targets_;
  std::map<std::tuple<TrackedTexture*, TrackedTexture*, TrackedTexture*, TrackedTexture*,
                      TrackedTexture*>,
           std::unique_ptr<plume::RenderFramebuffer>>
      framebuffers_;
  std::unordered_map<uint32_t, std::unique_ptr<Resolved>> resolved_;
  uint64_t resolve_version_ = 0;

  std::unique_ptr<plume::RenderPipelineLayout> depth_copy_layout_;
  // Without and with MSAA on the source.
  std::unique_ptr<plume::RenderPipeline> depth_copy_pipelines_[2];
  std::unique_ptr<plume::RenderDescriptorSet>
      depth_copy_sets_[PlumeRenderer::kFramesInFlight][kMaxDepthCopiesPerFrame];
  uint64_t depth_copy_frame_ = UINT64_MAX;
  uint32_t depth_copy_count_ = 0;
  std::unordered_map<TrackedTexture*, std::unique_ptr<plume::RenderTextureView>> depth_views_;
};

}  // namespace hajinmon::renderer
