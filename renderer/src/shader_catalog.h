// Hajinmon native renderer - catalog of the game's compiled shaders.
//
// DOA4 keeps all of its shaders in the executable as Xbox 360 D3D shader
// containers (flags 0x102A0E00 for pixel shaders, 0x102A0E01 for vertex
// shaders, built with the 2005 SDK). A container is a "virtual" part (header,
// D3DX constant table, definitions, shader descriptor) followed by the
// "physical" part: the Xenos microcode the game loads on the GPU.
//
// Scanning the executable once gives the complete set of microcode the game can
// use, so every shader can be translated and compiled before it is needed.

#pragma once

#include <cstdint>
#include <vector>

namespace hajinmon::renderer {

struct CatalogShader {
  uint32_t container_address;  // Guest address of the container.
  uint32_t ucode_address;      // Guest address of the microcode.
  uint32_t ucode_dword_count;
  bool is_vertex_shader;
};

class ShaderCatalog {
 public:
  // Scans [image_base, image_base + image_size) of guest memory, given as a
  // host pointer to its first byte.
  void Build(const uint8_t* image, uint32_t image_base, uint32_t image_size);

  bool built() const { return built_; }
  const std::vector<CatalogShader>& shaders() const { return shaders_; }

 private:
  bool built_ = false;
  std::vector<CatalogShader> shaders_;
};

}  // namespace hajinmon::renderer
