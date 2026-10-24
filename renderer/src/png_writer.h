// Hajinmon native renderer - minimal PNG writer, for frame captures.

#pragma once

#include <cstdint>
#include <filesystem>

namespace hajinmon::renderer {

// Writes 8-bit RGBA pixels (tightly packed rows) as an uncompressed PNG.
bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height,
              const uint8_t* rgba);

}  // namespace hajinmon::renderer
