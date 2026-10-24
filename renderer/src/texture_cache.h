// Hajinmon native renderer - textures and samplers.
//
// Guest textures are loaded on the CPU from guest memory (untiled and
// endian-swapped) into host textures of a matching format, which go into
// bindless descriptor arrays the translated shaders index. Textures the game
// resolved from render targets are copied on the GPU from the resolve result
// instead. A texture is reloaded when the CPU writes to its memory (the shared
// memory watches its pages).
//
// Not handled yet: stacked (array) textures (a black placeholder is bound),
// mixed signed and unsigned components, gamma formats, and texture exponent
// bias.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xenos.h>

#include "plume_renderer.h"

namespace rex::memory {
class Memory;
}

namespace hajinmon::renderer {

class PlumeSharedMemory;
class RenderTargetCache;

class TextureCache {
 public:
  static constexpr uint32_t kMaxTextures = 8192;
  // Plume's Direct3D 12 sampler heap holds 1024 samplers in total.
  static constexpr uint32_t kMaxSamplers = 512;
  // Descriptor sets of the bindless arrays (spaces 1-4 of the shaders).
  enum DescriptorSet {
    kSet2D,
    kSet3D,
    kSetCube,
    kSetSamplers,
    kSetCount,
  };

  TextureCache(PlumeRenderer& renderer, PlumeSharedMemory& shared_memory,
               rex::memory::Memory& memory, RenderTargetCache& render_target_cache);
  ~TextureCache();

  bool Initialize();
  void Shutdown();

  // The layout of each bindless descriptor set, for the pipeline layout.
  static plume::RenderDescriptorSetDesc GetDescriptorSetDesc(DescriptorSet set);
  plume::RenderDescriptorSet* descriptor_set(DescriptorSet set) const {
    return sets_[set].get();
  }

  // Makes the texture of the fetch constant ready for sampling, returning its
  // binding for the shaders: texture index | sampler index << 20, and the
  // swizzle to apply to the sampled value.
  void Bind(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch,
            rex::graphics::xenos::FetchOpDimension dimension, uint32_t& binding_out,
            uint32_t& swizzle_out);

  uint32_t textures_loaded_this_frame() const { return loads_; }
  // Placeholder bindings since the last call, by reason, for the statistics.
  std::string TakePlaceholderStats();
  void EndFrame() { loads_ = 0; }

 private:
  struct Texture {
    TrackedTexture texture;
    uint32_t index = 0;
    uint32_t swizzle = 0;
    bool loaded = false;
    std::atomic<bool> dirty{false};
    // Watches of the base level and of the mips memory.
    void* watches[2] = {};
    uint32_t levels = 0;
    uint32_t depth = 0;
    DescriptorSet set = kSet2D;
    // For cube maps.
    std::unique_ptr<plume::RenderTextureView> view;
    // Version of the resolve copied into it, if it comes from one.
    uint64_t resolve_version = 0;
  };

  static DescriptorSet SetFor(rex::graphics::xenos::DataDimension dimension) {
    return dimension == rex::graphics::xenos::DataDimension::k3D     ? kSet3D
           : dimension == rex::graphics::xenos::DataDimension::kCube ? kSetCube
                                                                      : kSet2D;
  }
  Texture* GetTexture(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
  bool Load(Texture& texture, const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
  uint32_t GetSampler(const rex::graphics::xenos::xe_gpu_texture_fetch_t& fetch);
  static void WatchCallback(const std::unique_lock<std::recursive_mutex>& lock, void* context,
                            void* data, uint64_t argument, bool invalidated_by_gpu);

  PlumeRenderer& renderer_;
  PlumeSharedMemory& shared_memory_;
  rex::memory::Memory& memory_;
  RenderTargetCache& render_target_cache_;

  std::unique_ptr<plume::RenderDescriptorSet> sets_[kSetCount];
  // Black textures at index 0 of each array.
  TrackedTexture placeholder_2d_, placeholder_3d_, placeholder_cube_;
  std::unique_ptr<plume::RenderTextureView> placeholder_cube_view_;

  std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures_;
  uint32_t next_texture_index_ = 1;
  std::unordered_map<uint32_t, uint32_t> sampler_indices_;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers_;
  uint32_t loads_ = 0;
  uint64_t unsupported_formats_logged_ = 0;
  // Invalid fetch constant, 3D, cube, load failed.
  uint64_t placeholders_[4] = {};
  uint64_t placeholder_formats_[64] = {};
};

}  // namespace hajinmon::renderer
