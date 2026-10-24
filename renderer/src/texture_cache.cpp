// Hajinmon native renderer - textures and samplers.

#include "texture_cache.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging/macros.h>
#include <rex/system/xmemory.h>

#include "plume_shared_memory.h"
#include "render_target_cache.h"

namespace hajinmon::renderer {

namespace xenos = rex::graphics::xenos;
namespace texture_util = rex::graphics::texture_util;

namespace {

enum class Conversion {
  kCopy,
  k565ToRGBA8,
  k1555ToRGBA8,
  k4444ToRGBA8,
  k2101010ToRGBA16,
};

struct HostFormat {
  plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
  Conversion conversion = Conversion::kCopy;
  // Bytes per block on the host.
  uint32_t bytes_per_block = 0;
  // Host component holding each guest component (X, Y, Z, W).
  uint8_t components[4] = {0, 1, 2, 3};
};

HostFormat GetHostFormat(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  using P = plume::RenderFormat;
  switch (format) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      return {P::R8_UNORM, Conversion::kCopy, 1, {0, 0, 0, 0}};
    case F::k_8_8:
      return {P::R8G8_UNORM, Conversion::kCopy, 2, {0, 1, 1, 1}};
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
      return {P::R8G8B8A8_UNORM, Conversion::kCopy, 4};
    case F::k_DXT1:
      return {P::BC1_UNORM, Conversion::kCopy, 8};
    case F::k_DXT2_3:
      return {P::BC2_UNORM, Conversion::kCopy, 16};
    case F::k_DXT4_5:
      return {P::BC3_UNORM, Conversion::kCopy, 16};
    case F::k_16:
      return {P::R16_UNORM, Conversion::kCopy, 2, {0, 0, 0, 0}};
    case F::k_16_16:
      return {P::R16G16_UNORM, Conversion::kCopy, 4, {0, 1, 1, 1}};
    case F::k_16_16_16_16:
      return {P::R16G16B16A16_UNORM, Conversion::kCopy, 8};
    case F::k_16_FLOAT:
      return {P::R16_FLOAT, Conversion::kCopy, 2, {0, 0, 0, 0}};
    case F::k_16_16_FLOAT:
      return {P::R16G16_FLOAT, Conversion::kCopy, 4, {0, 1, 1, 1}};
    case F::k_16_16_16_16_FLOAT:
      return {P::R16G16B16A16_FLOAT, Conversion::kCopy, 8};
    case F::k_32_FLOAT:
      return {P::R32_FLOAT, Conversion::kCopy, 4, {0, 0, 0, 0}};
    case F::k_32_32_FLOAT:
      return {P::R32G32_FLOAT, Conversion::kCopy, 8, {0, 1, 1, 1}};
    case F::k_32_32_32_32_FLOAT:
      return {P::R32G32B32A32_FLOAT, Conversion::kCopy, 16};
    case F::k_5_6_5:
      return {P::R8G8B8A8_UNORM, Conversion::k565ToRGBA8, 4};
    case F::k_1_5_5_5:
      return {P::R8G8B8A8_UNORM, Conversion::k1555ToRGBA8, 4};
    case F::k_4_4_4_4:
      return {P::R8G8B8A8_UNORM, Conversion::k4444ToRGBA8, 4};
    case F::k_2_10_10_10:
      return {P::R16G16B16A16_UNORM, Conversion::k2101010ToRGBA16, 8};
    default:
      return {};
  }
}

uint32_t ComposeSwizzle(uint32_t guest_swizzle, const HostFormat& host) {
  uint32_t result = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t source = (guest_swizzle >> (i * 3)) & 7;
    if (source < 4) {
      source = host.components[source];
    }
    result |= source << (i * 3);
  }
  return result;
}

void SwapDwords(uint8_t* data, size_t size, xenos::Endian endian) {
  if (endian == xenos::Endian::kNone) {
    return;
  }
  auto* dwords = reinterpret_cast<uint32_t*>(data);
  for (size_t i = 0; i < size / 4; ++i) {
    uint32_t v = dwords[i];
    if (endian == xenos::Endian::k8in16 || endian == xenos::Endian::k8in32) {
      v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    }
    if (endian == xenos::Endian::k8in32 || endian == xenos::Endian::k16in32) {
      v = (v << 16) | (v >> 16);
    }
    dwords[i] = v;
  }
}

uint8_t Expand(uint32_t value, uint32_t bits) {
  return uint8_t(value * 255 / ((1u << bits) - 1));
}

// Converts one guest block to the host format.
void ConvertBlock(Conversion conversion, const uint8_t* source, uint8_t* dest,
                  uint32_t guest_bytes_per_block, uint32_t host_bytes_per_block) {
  switch (conversion) {
    case Conversion::kCopy:
      std::memcpy(dest, source, std::min(guest_bytes_per_block, host_bytes_per_block));
      break;
    case Conversion::k565ToRGBA8: {
      uint32_t v = source[0] | (uint32_t(source[1]) << 8);
      dest[0] = Expand(v >> 11, 5);
      dest[1] = Expand((v >> 5) & 0x3F, 6);
      dest[2] = Expand(v & 0x1F, 5);
      dest[3] = 255;
    } break;
    case Conversion::k1555ToRGBA8: {
      uint32_t v = source[0] | (uint32_t(source[1]) << 8);
      dest[0] = Expand((v >> 10) & 0x1F, 5);
      dest[1] = Expand((v >> 5) & 0x1F, 5);
      dest[2] = Expand(v & 0x1F, 5);
      dest[3] = (v & 0x8000) ? 255 : 0;
    } break;
    case Conversion::k4444ToRGBA8: {
      uint32_t v = source[0] | (uint32_t(source[1]) << 8);
      dest[0] = Expand((v >> 8) & 0xF, 4);
      dest[1] = Expand((v >> 4) & 0xF, 4);
      dest[2] = Expand(v & 0xF, 4);
      dest[3] = Expand(v >> 12, 4);
    } break;
    case Conversion::k2101010ToRGBA16: {
      uint32_t v;
      std::memcpy(&v, source, 4);
      uint16_t c[4] = {uint16_t((v & 0x3FF) * 65535 / 1023), uint16_t(((v >> 10) & 0x3FF) * 65535 / 1023),
                       uint16_t(((v >> 20) & 0x3FF) * 65535 / 1023), uint16_t((v >> 30) * 65535 / 3)};
      std::memcpy(dest, c, sizeof(c));
    } break;
  }
}

}  // namespace

TextureCache::TextureCache(PlumeRenderer& renderer, PlumeSharedMemory& shared_memory,
                           rex::memory::Memory& memory, RenderTargetCache& render_target_cache)
    : renderer_(renderer),
      shared_memory_(shared_memory),
      memory_(memory),
      render_target_cache_(render_target_cache) {}

TextureCache::~TextureCache() { Shutdown(); }

plume::RenderDescriptorSetDesc TextureCache::GetDescriptorSetDesc(DescriptorSet set) {
  static const plume::RenderDescriptorRange kTextureRange(plume::RenderDescriptorRangeType::TEXTURE,
                                                         0, kMaxTextures);
  static const plume::RenderDescriptorRange kSamplerRange(plume::RenderDescriptorRangeType::SAMPLER,
                                                         0, kMaxSamplers);
  return set == kSetSamplers ? plume::RenderDescriptorSetDesc(&kSamplerRange, 1, true, kMaxSamplers)
                             : plume::RenderDescriptorSetDesc(&kTextureRange, 1, true, kMaxTextures);
}

bool TextureCache::Initialize() {
  plume::RenderDevice* device = renderer_.device();
  for (uint32_t i = 0; i < kSetCount; ++i) {
    sets_[i] = device->createDescriptorSet(GetDescriptorSetDesc(DescriptorSet(i)));
    if (!sets_[i]) {
      return false;
    }
  }
  // Black placeholders at index 0.
  auto make = [&](TrackedTexture& t, plume::RenderTextureDesc desc) {
    t.format = desc.format;
    t.width = t.height = 1;
    t.texture = device->createTexture(desc);
  };
  make(placeholder_2d_, plume::RenderTextureDesc::Texture2D(1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  make(placeholder_3d_, plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, plume::RenderFormat::R8G8B8A8_UNORM));
  make(placeholder_cube_,
       plume::RenderTextureDesc::Texture(plume::RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
                                         plume::RenderFormat::R8G8B8A8_UNORM,
                                         plume::RenderTextureFlag::CUBE));
  if (!placeholder_2d_.texture || !placeholder_3d_.texture || !placeholder_cube_.texture) {
    return false;
  }
  placeholder_cube_view_ = placeholder_cube_.texture->createTextureView(
      plume::RenderTextureViewDesc::TextureCube(plume::RenderFormat::R8G8B8A8_UNORM));
  plume::RenderCommandList* list = renderer_.command_list();
  PlumeRenderer::Allocation zero = renderer_.Upload(256 * 6, 256);
  std::memset(zero.data, 0, 256 * 6);
  for (TrackedTexture* t : {&placeholder_2d_, &placeholder_3d_, &placeholder_cube_}) {
    t->Transition(list, plume::RenderTextureLayout::COPY_DEST);
    uint32_t layers = t == &placeholder_cube_ ? 6 : 1;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      list->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(t->texture.get(), 0, layer),
          plume::RenderTextureCopyLocation::PlacedFootprint(zero.buffer, t->format, 1, 1, 1, 64,
                                                            zero.offset + layer * 256));
    }
    t->Transition(list, plume::RenderTextureLayout::SHADER_READ);
  }
  sets_[kSet2D]->setTexture(0, placeholder_2d_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
  sets_[kSet3D]->setTexture(0, placeholder_3d_.texture.get(), plume::RenderTextureLayout::SHADER_READ);
  sets_[kSetCube]->setTexture(0, placeholder_cube_.texture.get(),
                              plume::RenderTextureLayout::SHADER_READ, placeholder_cube_view_.get());
  // Sampler 0: linear, wrapping.
  samplers_.push_back(device->createSampler(plume::RenderSamplerDesc()));
  sets_[kSetSamplers]->setSampler(0, samplers_[0].get());
  return true;
}

void TextureCache::Shutdown() {
  for (auto& [key, texture] : textures_) {
    if (texture->watch) {
      shared_memory_.UnwatchMemoryRange(texture->watch);
      texture->watch = nullptr;
    }
  }
  textures_.clear();
  samplers_.clear();
  sampler_indices_.clear();
  for (auto& set : sets_) {
    set.reset();
  }
  placeholder_cube_view_.reset();
  placeholder_2d_ = TrackedTexture();
  placeholder_3d_ = TrackedTexture();
  placeholder_cube_ = TrackedTexture();
}

void TextureCache::WatchCallback(const std::unique_lock<std::recursive_mutex>& lock, void* context,
                                 void* data, uint64_t argument, bool invalidated_by_gpu) {
  (void)lock;
  (void)context;
  (void)argument;
  (void)invalidated_by_gpu;
  auto* texture = static_cast<Texture*>(data);
  // The watch is cancelled after the callback.
  texture->watch = nullptr;
  texture->dirty = true;
}

uint32_t TextureCache::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch) {
  uint32_t key = uint32_t(fetch.clamp_x) | (uint32_t(fetch.clamp_y) << 3) |
                 (uint32_t(fetch.clamp_z) << 6) | (uint32_t(fetch.mag_filter) << 9) |
                 (uint32_t(fetch.min_filter) << 11) | (uint32_t(fetch.mip_filter) << 13) |
                 (uint32_t(fetch.aniso_filter) << 15) | (uint32_t(fetch.border_color) << 18);
  auto it = sampler_indices_.find(key);
  if (it != sampler_indices_.end()) {
    return it->second;
  }
  if (samplers_.size() >= kMaxSamplers) {
    return 0;
  }
  auto address_mode = [](xenos::ClampMode mode) {
    switch (mode) {
      case xenos::ClampMode::kRepeat:
        return plume::RenderTextureAddressMode::WRAP;
      case xenos::ClampMode::kMirroredRepeat:
        return plume::RenderTextureAddressMode::MIRROR;
      case xenos::ClampMode::kMirrorClampToEdge:
      case xenos::ClampMode::kMirrorClampToHalfway:
        return plume::RenderTextureAddressMode::MIRROR_ONCE;
      case xenos::ClampMode::kClampToBorder:
      case xenos::ClampMode::kMirrorClampToBorder:
        return plume::RenderTextureAddressMode::BORDER;
      default:
        return plume::RenderTextureAddressMode::CLAMP;
    }
  };
  plume::RenderSamplerDesc desc;
  desc.addressU = address_mode(fetch.clamp_x);
  desc.addressV = address_mode(fetch.clamp_y);
  desc.addressW = address_mode(fetch.clamp_z);
  desc.magFilter = fetch.mag_filter == xenos::TextureFilter::kPoint ? plume::RenderFilter::NEAREST
                                                                    : plume::RenderFilter::LINEAR;
  desc.minFilter = fetch.min_filter == xenos::TextureFilter::kPoint ? plume::RenderFilter::NEAREST
                                                                    : plume::RenderFilter::LINEAR;
  desc.mipmapMode = fetch.mip_filter == xenos::TextureFilter::kLinear ? plume::RenderMipmapMode::LINEAR
                                                                      : plume::RenderMipmapMode::NEAREST;
  uint32_t aniso = uint32_t(fetch.aniso_filter);
  if (aniso > uint32_t(xenos::AnisoFilter::kMax_1_1) && aniso <= uint32_t(xenos::AnisoFilter::kMax_16_1)) {
    desc.anisotropyEnabled = true;
    desc.maxAnisotropy = 1u << (aniso - 1);
  }
  // Coordinates outside a texture with clamp-to-border read this: for shadow
  // maps, white means not in shadow.
  desc.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White
                         ? plume::RenderBorderColor::OPAQUE_WHITE
                         : plume::RenderBorderColor::TRANSPARENT_BLACK;
  uint32_t index = uint32_t(samplers_.size());
  samplers_.push_back(renderer_.device()->createSampler(desc));
  sets_[kSetSamplers]->setSampler(index, samplers_.back().get());
  sampler_indices_.emplace(key, index);
  return index;
}

TextureCache::Texture* TextureCache::GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch) {
  // Everything describing the data (not the sampler state).
  uint64_t key = (uint64_t(fetch.dword_1) << 32) ^ (uint64_t(fetch.dword_2) * 0x9E3779B97F4A7C15ull) ^
                 (uint64_t(fetch.dword_0 & 0xFFC00001u) << 1) ^
                 (uint64_t(fetch.dword_5) * 0xC2B2AE3D27D4EB4Full) ^
                 (uint64_t(fetch.swizzle) << 50);
  std::unique_ptr<Texture>& texture = textures_[key];
  if (!texture) {
    if (next_texture_index_ >= kMaxTextures) {
      return nullptr;
    }
    texture = std::make_unique<Texture>();
    texture->index = next_texture_index_++;
  }
  return texture.get();
}

bool TextureCache::Load(Texture& texture, const xenos::xe_gpu_texture_fetch_t& fetch) {
  auto format = xenos::TextureFormat(fetch.format);
  HostFormat host = GetHostFormat(format);
  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(format);
  if (host.format == plume::RenderFormat::UNKNOWN || !info) {
    if (!(unsupported_formats_logged_ & (uint64_t(1) << (uint32_t(format) & 63)))) {
      unsupported_formats_logged_ |= uint64_t(1) << (uint32_t(format) & 63);
      REXLOG_WARN("Plume: texture format {} is not supported yet", uint32_t(format));
    }
    return false;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (!base_page) {
    return false;
  }
  uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  auto dimension = xenos::DataDimension(fetch.dimension);
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      dimension, fetch.pitch, width, height, 1, fetch.tiled, format, fetch.packed_mips, true, 0);
  uint32_t base_address = base_page << 12;
  uint32_t extent = std::max(layout.base.level_data_extent_bytes, 4u);
  if (uint64_t(base_address) + extent > PlumeSharedMemory::kBufferSize) {
    return false;
  }

  // Watch for CPU writes to reload the texture.
  if (texture.watch) {
    shared_memory_.UnwatchMemoryRange(texture.watch);
    texture.watch = nullptr;
  }
  texture.dirty = false;
  shared_memory_.RequestRange(base_address, extent);
  texture.watch =
      shared_memory_.WatchMemoryRange(base_address, extent, WatchCallback, this, &texture, 0);

  // Copy and endian-swap the memory, in memory order.
  std::vector<uint8_t> guest(rex::align(extent, 4u));
  std::memcpy(guest.data(), memory_.TranslatePhysical<const uint8_t*>(base_address), extent);
  SwapDwords(guest.data(), guest.size(), xenos::Endian(fetch.endianness));

  uint32_t block_width = info->block_width, block_height = info->block_height;
  uint32_t guest_bpb = info->bytes_per_block();
  uint32_t bpb_log2 = guest_bpb >= 16 ? 4 : guest_bpb >= 8 ? 3 : guest_bpb >= 4 ? 2 : guest_bpb >= 2 ? 1 : 0;
  uint32_t width_blocks = (width + block_width - 1) / block_width;
  uint32_t height_blocks = (height + block_height - 1) / block_height;
  uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
  if (layout.packed_level == 0) {
    texture_util::GetPackedMipOffset(width, height, 1, format, 0, offset_x, offset_y, offset_z);
  }
  uint32_t row_pitch = layout.base.row_pitch_bytes;
  uint32_t pitch_blocks = row_pitch >> bpb_log2;

  // Host rows are aligned to 256 bytes for Direct3D 12.
  uint32_t host_row = (width_blocks * host.bytes_per_block + 255) & ~255u;
  PlumeRenderer::Allocation upload = renderer_.Upload(uint64_t(host_row) * height_blocks, 512);
  for (uint32_t y = 0; y < height_blocks; ++y) {
    uint8_t* dest_row = upload.data + uint64_t(host_row) * y;
    for (uint32_t x = 0; x < width_blocks; ++x) {
      int64_t source_offset =
          fetch.tiled ? texture_util::GetTiledOffset2D(int32_t(x + offset_x), int32_t(y + offset_y),
                                                       pitch_blocks, bpb_log2)
                      : int64_t(y + offset_y) * row_pitch + int64_t(x + offset_x) * guest_bpb;
      uint8_t* dest = dest_row + x * host.bytes_per_block;
      if (source_offset < 0 || uint64_t(source_offset) + guest_bpb > guest.size()) {
        std::memset(dest, 0, host.bytes_per_block);
        continue;
      }
      ConvertBlock(host.conversion, guest.data() + source_offset, dest, guest_bpb,
                   host.bytes_per_block);
    }
  }

  // Block-compressed textures need host sizes in whole blocks.
  uint32_t host_width = width_blocks * block_width, host_height = height_blocks * block_height;
  if (!texture.texture.texture || texture.texture.format != host.format ||
      texture.texture.width != host_width || texture.texture.height != host_height) {
    renderer_.Retire(std::move(texture.texture.texture));
    texture.texture = TrackedTexture();
    texture.texture.format = host.format;
    texture.texture.width = host_width;
    texture.texture.height = host_height;
    texture.texture.texture = renderer_.device()->createTexture(
        plume::RenderTextureDesc::Texture2D(host_width, host_height, 1, host.format));
    if (!texture.texture.texture) {
      return false;
    }
    texture.texture.texture->setName(fmt::format("Texture {:08X}", base_address));
  }
  plume::RenderCommandList* list = renderer_.command_list();
  texture.texture.Transition(list, plume::RenderTextureLayout::COPY_DEST);
  list->copyTextureRegion(
      plume::RenderTextureCopyLocation::Subresource(texture.texture.texture.get()),
      plume::RenderTextureCopyLocation::PlacedFootprint(
          upload.buffer, host.format, host_width, host_height, 1,
          host_row / host.bytes_per_block * block_width, upload.offset));
  texture.texture.Transition(list, plume::RenderTextureLayout::SHADER_READ);
  sets_[kSet2D]->setTexture(texture.index, texture.texture.texture.get(),
                            plume::RenderTextureLayout::SHADER_READ);
  texture.swizzle = ComposeSwizzle(fetch.swizzle, host);
  texture.loaded = true;
  texture.resolve_version = 0;
  ++loads_;
  return true;
}

void TextureCache::Bind(const xenos::xe_gpu_texture_fetch_t& fetch,
                        xenos::FetchOpDimension dimension, uint32_t& binding_out,
                        uint32_t& swizzle_out) {
  uint32_t sampler = GetSampler(fetch);
  // The black placeholder by default: (0, 0, 0, 1).
  binding_out = sampler << 20;
  swizzle_out = 4 | (4 << 3) | (4 << 6) | (5 << 9);
  if (fetch.type != xenos::FetchConstantType::kTexture ||
      dimension == xenos::FetchOpDimension::k3DOrStacked ||
      dimension == xenos::FetchOpDimension::kCube ||
      xenos::DataDimension(fetch.dimension) == xenos::DataDimension::k3D ||
      xenos::DataDimension(fetch.dimension) == xenos::DataDimension::kCube) {
    ++placeholders_[fetch.type != xenos::FetchConstantType::kTexture ? 0
                    : (dimension == xenos::FetchOpDimension::kCube ||
                       xenos::DataDimension(fetch.dimension) == xenos::DataDimension::kCube)
                        ? 2
                        : 1];
    return;
  }
  Texture* texture = GetTexture(fetch);
  if (!texture) {
    return;
  }
  plume::RenderCommandList* list = renderer_.command_list();

  // Rendered by the game: copy from the resolve result.
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  RenderTargetCache::Resolved* resolved = render_target_cache_.FindResolved(base_page << 12);
  uint32_t width = width_minus_1 + 1, height = height_minus_1 + 1;
  // Resolved textures are at the resolution scale.
  uint32_t scale = render_target_cache_.scale();
  if (resolved && resolved->guest_width >= width && resolved->guest_height >= height) {
    if (texture->resolve_version != resolved->version) {
      if (!texture->texture.texture || texture->texture.format != resolved->texture.format ||
          texture->texture.width != width * scale || texture->texture.height != height * scale) {
        renderer_.Retire(std::move(texture->texture.texture));
        texture->texture = TrackedTexture();
        texture->texture.format = resolved->texture.format;
        texture->texture.width = width * scale;
        texture->texture.height = height * scale;
        texture->texture.texture = renderer_.device()->createTexture(
            plume::RenderTextureDesc::Texture2D(width * scale, height * scale, 1, resolved->texture.format));
      }
      resolved->texture.Transition(list, plume::RenderTextureLayout::COPY_SOURCE);
      texture->texture.Transition(list, plume::RenderTextureLayout::COPY_DEST);
      plume::RenderBox box(0, 0, int32_t(width * scale), int32_t(height * scale));
      list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(texture->texture.texture.get()),
                              plume::RenderTextureCopyLocation::Subresource(resolved->texture.texture.get()),
                              0, 0, 0, &box);
      texture->texture.Transition(list, plume::RenderTextureLayout::SHADER_READ);
      sets_[kSet2D]->setTexture(texture->index, texture->texture.texture.get(),
                                plume::RenderTextureLayout::SHADER_READ);
      texture->resolve_version = resolved->version;
      HostFormat resolved_format;
      if (resolved->texture.format == plume::RenderFormat::R32_FLOAT) {
        // Resolved depth: every component is the depth.
        resolved_format = GetHostFormat(xenos::TextureFormat::k_32_FLOAT);
      }
      if (resolved->swap_red_blue) {
        resolved_format.components[0] = 2;
        resolved_format.components[2] = 0;
      }
      texture->swizzle = ComposeSwizzle(fetch.swizzle, resolved_format);
      texture->loaded = true;
      if (texture->watch) {
        shared_memory_.UnwatchMemoryRange(texture->watch);
        texture->watch = nullptr;
      }
    }
  } else if (!texture->loaded || texture->dirty || texture->resolve_version) {
    texture->loaded = false;
    if (!Load(*texture, fetch)) {
      ++placeholders_[3];
      ++placeholder_formats_[uint32_t(fetch.format) & 63];
      return;
    }
  }
  texture->texture.Transition(list, plume::RenderTextureLayout::SHADER_READ);
  binding_out = texture->index | (sampler << 20);
  swizzle_out = texture->swizzle;
}

std::string TextureCache::TakePlaceholderStats() {
  std::string stats;
  static const char* const kReasons[] = {"invalid", "3D", "cube", "not loaded"};
  for (uint32_t i = 0; i < 4; ++i) {
    if (placeholders_[i]) {
      stats += fmt::format(" {} {},", kReasons[i], placeholders_[i]);
    }
    placeholders_[i] = 0;
  }
  for (uint32_t i = 0; i < 64; ++i) {
    if (placeholder_formats_[i]) {
      stats += fmt::format(" format {}: {},", i, placeholder_formats_[i]);
    }
    placeholder_formats_[i] = 0;
  }
  if (!stats.empty()) {
    stats.pop_back();
  }
  return stats;
}

}  // namespace hajinmon::renderer
