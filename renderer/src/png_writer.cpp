// Hajinmon native renderer - minimal PNG writer, for screenshots and frame
// captures. Rows use the "up" filter and are compressed with LZ77 and the
// fixed deflate Huffman codes: simple, and a few times smaller than raw pixels.

#include "png_writer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
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

class BitWriter {
 public:
  explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
  // Least significant bit first, as deflate stores everything but Huffman codes.
  void Bits(uint32_t value, uint32_t count) {
    buffer_ |= uint64_t(value) << count_;
    count_ += count;
    while (count_ >= 8) {
      out_.push_back(uint8_t(buffer_));
      buffer_ >>= 8;
      count_ -= 8;
    }
  }
  // Huffman codes are stored most significant bit first.
  void Code(uint32_t code, uint32_t length) {
    uint32_t reversed = 0;
    for (uint32_t i = 0; i < length; ++i) {
      reversed |= ((code >> i) & 1) << (length - 1 - i);
    }
    Bits(reversed, length);
  }
  void Flush() {
    if (count_) {
      out_.push_back(uint8_t(buffer_));
    }
    buffer_ = 0;
    count_ = 0;
  }

 private:
  std::vector<uint8_t>& out_;
  uint64_t buffer_ = 0;
  uint32_t count_ = 0;
};

// Fixed Huffman literal/length code.
void PutLiteralLength(BitWriter& bits, uint32_t symbol) {
  if (symbol < 144) {
    bits.Code(0x30 + symbol, 8);
  } else if (symbol < 256) {
    bits.Code(0x190 + (symbol - 144), 9);
  } else if (symbol < 280) {
    bits.Code(symbol - 256, 7);
  } else {
    bits.Code(0xC0 + (symbol - 280), 8);
  }
}

void Deflate(const std::vector<uint8_t>& data, std::vector<uint8_t>& out) {
  static const uint16_t kLengthBase[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t kDistanceBase[] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                           33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                           1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t kDistanceExtra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                           6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  constexpr size_t kWindow = 32768, kMaxMatch = 258, kHashSize = 1 << 16;

  BitWriter bits(out);
  bits.Bits(1, 1);  // Final block.
  bits.Bits(1, 2);  // Fixed Huffman codes.
  std::vector<int64_t> last(kHashSize, -1);
  size_t size = data.size();
  size_t i = 0;
  while (i < size) {
    size_t match_length = 0, match_distance = 0;
    if (i + 3 <= size) {
      uint32_t hash = ((uint32_t(data[i]) << 16) ^ (uint32_t(data[i + 1]) << 8) ^ data[i + 2]) *
                          2654435761u >>
                      16;
      int64_t candidate = last[hash];
      last[hash] = int64_t(i);
      if (candidate >= 0 && i - size_t(candidate) <= kWindow) {
        size_t limit = std::min(kMaxMatch, size - i);
        size_t length = 0;
        while (length < limit && data[size_t(candidate) + length] == data[i + length]) {
          ++length;
        }
        if (length >= 3) {
          match_length = length;
          match_distance = i - size_t(candidate);
        }
      }
    }
    if (!match_length) {
      PutLiteralLength(bits, data[i]);
      ++i;
      continue;
    }
    uint32_t length_code = 28;
    while (kLengthBase[length_code] > match_length) {
      --length_code;
    }
    PutLiteralLength(bits, 257 + length_code);
    bits.Bits(uint32_t(match_length - kLengthBase[length_code]), kLengthExtra[length_code]);
    uint32_t distance_code = 29;
    while (kDistanceBase[distance_code] > match_distance) {
      --distance_code;
    }
    bits.Code(distance_code, 5);
    bits.Bits(uint32_t(match_distance - kDistanceBase[distance_code]),
              kDistanceExtra[distance_code]);
    // Index the skipped positions too, for better matches later.
    for (size_t j = i + 1; j < i + match_length && j + 3 <= size; ++j) {
      uint32_t hash = ((uint32_t(data[j]) << 16) ^ (uint32_t(data[j + 1]) << 8) ^ data[j + 2]) *
                          2654435761u >>
                      16;
      last[hash] = int64_t(j);
    }
    i += match_length;
  }
  PutLiteralLength(bits, 256);  // End of block.
  bits.Flush();
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

  // Rows with the "up" filter (the first row unfiltered).
  size_t row_size = size_t(width) * 4;
  std::vector<uint8_t> raw;
  raw.reserve((row_size + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* row = rgba + y * row_size;
    if (!y) {
      raw.push_back(0);
      raw.insert(raw.end(), row, row + row_size);
      continue;
    }
    const uint8_t* above = row - row_size;
    raw.push_back(2);
    for (size_t x = 0; x < row_size; ++x) {
      raw.push_back(uint8_t(row[x] - above[x]));
    }
  }
  std::vector<uint8_t> zlib = {0x78, 0x01};
  Deflate(raw, zlib);
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
