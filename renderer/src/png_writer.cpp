// Hajinmon native renderer - minimal PNG writer, for frame captures.

#include "png_writer.h"

#include <array>
#include <fstream>
#include <string>
#include <vector>

namespace hajinmon::renderer {

namespace {

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void Chunk(std::ofstream& file, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> chunk;
  PutBE32(chunk, uint32_t(data.size()));
  chunk.insert(chunk.end(), type, type + 4);
  chunk.insert(chunk.end(), data.begin(), data.end());
  PutBE32(chunk, Crc32(chunk.data() + 4, chunk.size() - 4));
  file.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(chunk.size()));
}

}  // namespace

bool WritePng(const std::filesystem::path& path, uint32_t width, uint32_t height,
              const uint8_t* rgba) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  static const uint8_t kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  file.write(reinterpret_cast<const char*>(kSignature), sizeof(kSignature));

  std::vector<uint8_t> header;
  PutBE32(header, width);
  PutBE32(header, height);
  header.insert(header.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA.
  Chunk(file, "IHDR", header);

  // Rows prefixed with filter 0, in stored (uncompressed) deflate blocks.
  std::vector<uint8_t> raw;
  raw.reserve(size_t(width * 4 + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + size_t(y) * width * 4, rgba + size_t(y + 1) * width * 4);
  }
  std::vector<uint8_t> zlib = {0x78, 0x01};
  for (size_t offset = 0; offset < raw.size() || raw.empty();) {
    size_t length = std::min<size_t>(65535, raw.size() - offset);
    bool last = offset + length >= raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(uint8_t(length));
    zlib.push_back(uint8_t(length >> 8));
    zlib.push_back(uint8_t(~length));
    zlib.push_back(uint8_t(~length >> 8));
    zlib.insert(zlib.end(), raw.begin() + offset, raw.begin() + offset + length);
    offset += length;
    if (last) {
      break;
    }
  }
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  PutBE32(zlib, (b << 16) | a);
  Chunk(file, "IDAT", zlib);
  Chunk(file, "IEND", {});
  return bool(file);
}

}  // namespace hajinmon::renderer
