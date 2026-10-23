// Hajinmon native renderer - catalog of the game's compiled shaders.

#include "shader_catalog.h"

namespace hajinmon::renderer {

namespace {

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

}  // namespace

void ShaderCatalog::Build(const uint8_t* image, uint32_t image_base, uint32_t image_size) {
  shaders_.clear();
  for (uint32_t offset = 0; offset + 24 <= image_size; offset += 4) {
    uint32_t flags = LoadBE32(image + offset);
    if ((flags & 0xFFFFFFFE) != 0x102A0E00) {
      continue;
    }
    // Header: flags, virtual size, physical size, ... The microcode follows
    // the virtual part directly.
    uint32_t virtual_size = LoadBE32(image + offset + 4);
    uint32_t physical_size = LoadBE32(image + offset + 8);
    if (virtual_size < 24 || virtual_size > 0x10000 || physical_size == 0 ||
        physical_size > 0x40000 || (virtual_size | physical_size) & 3 ||
        uint64_t(offset) + virtual_size + physical_size > image_size) {
      continue;
    }
    CatalogShader shader;
    shader.container_address = image_base + offset;
    shader.ucode_address = shader.container_address + virtual_size;
    shader.ucode_dword_count = physical_size / 4;
    shader.is_vertex_shader = (flags & 1) != 0;
    shaders_.push_back(shader);
  }
  built_ = true;
}

}  // namespace hajinmon::renderer
