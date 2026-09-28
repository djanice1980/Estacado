#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

float HalfToFloat(uint16_t value) {
  const uint32_t sign = uint32_t(value & 0x8000u) << 16;
  int32_t exponent = int32_t((value >> 10) & 0x1Fu);
  uint32_t mantissa = value & 0x3FFu;
  uint32_t result;
  if (!exponent) {
    if (!mantissa) {
      result = sign;
    } else {
      exponent = 1;
      while (!(mantissa & 0x400u)) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FFu;
      result = sign | (uint32_t(exponent + 112) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    result = sign | 0x7F800000u | (mantissa << 13);
  } else {
    result = sign | (uint32_t(exponent + 112) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(result);
}

uint8_t EncodeSrgb(float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  const float encoded = value <= 0.0031308f
                            ? value * 12.92f
                            : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
  return uint8_t(std::clamp(std::lround(encoded * 255.0f), 0l, 255l));
}

enum class DecodeMode {
  kFloat16,
  kUnorm16,
  kSnorm16,
  kR10G10B10A2Unorm,
  kRgba8Unorm,
};

float DecodeChannel(uint16_t value, DecodeMode mode) {
  switch (mode) {
    case DecodeMode::kUnorm16:
      return float(value) / 65535.0f;
    case DecodeMode::kSnorm16:
      return std::max(float(std::bit_cast<int16_t>(value)) / 32767.0f,
                      -1.0f);
    default:
      return HalfToFloat(value);
  }
}

void WriteU16(std::ofstream& output, uint16_t value) {
  output.put(char(value));
  output.put(char(value >> 8));
}

void WriteU32(std::ofstream& output, uint32_t value) {
  output.put(char(value));
  output.put(char(value >> 8));
  output.put(char(value >> 16));
  output.put(char(value >> 24));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5 || argc > 7) {
    std::cerr << "usage: decode-fp16-bmp <input> <output> <width> <height> "
                 "[float16|unorm16|snorm16|r10g10b10a2|rgba8] "
                 "[linear-exposure]\n";
    return 2;
  }
  DecodeMode decode_mode = DecodeMode::kFloat16;
  const char* decode_mode_name = "float16";
  if (argc >= 6) {
    const std::string mode = argv[5];
    if (mode == "unorm16") {
      decode_mode = DecodeMode::kUnorm16;
      decode_mode_name = "unorm16";
    } else if (mode == "snorm16") {
      decode_mode = DecodeMode::kSnorm16;
      decode_mode_name = "snorm16";
    } else if (mode == "r10g10b10a2") {
      decode_mode = DecodeMode::kR10G10B10A2Unorm;
      decode_mode_name = "r10g10b10a2";
    } else if (mode == "rgba8") {
      decode_mode = DecodeMode::kRgba8Unorm;
      decode_mode_name = "rgba8";
    } else if (mode != "float16") {
      std::cerr << "unknown decode mode: " << mode << '\n';
      return 2;
    }
  }
  const float linear_exposure = argc == 7 ? std::stof(argv[6]) : 1.0f;
  if (!(linear_exposure > 0.0f) || !std::isfinite(linear_exposure)) {
    std::cerr << "linear exposure must be finite and positive\n";
    return 2;
  }
  const uint32_t width = uint32_t(std::stoul(argv[3]));
  const uint32_t height = uint32_t(std::stoul(argv[4]));
  const uint32_t input_pixel_bytes =
      decode_mode == DecodeMode::kR10G10B10A2Unorm ||
              decode_mode == DecodeMode::kRgba8Unorm
          ? 4
          : 8;
  const uint64_t bytes_required =
      uint64_t(width) * height * input_pixel_bytes;
  std::ifstream input(argv[1], std::ios::binary);
  if (!input) {
    std::cerr << "input open failed\n";
    return 1;
  }
  std::vector<uint8_t> rgba(static_cast<size_t>(bytes_required), uint8_t{});
  input.read(reinterpret_cast<char*>(rgba.data()),
             std::streamsize(bytes_required));
  if (uint64_t(input.gcount()) != bytes_required) {
    std::cerr << "input is shorter than requested image: read="
              << input.gcount() << " required=" << bytes_required << '\n';
    return 1;
  }

  std::ofstream output(argv[2], std::ios::binary);
  if (!output) {
    std::cerr << "output open failed\n";
    return 1;
  }
  const uint32_t pixel_bytes = width * height * 4;
  output.put('B');
  output.put('M');
  WriteU32(output, 54 + pixel_bytes);
  WriteU32(output, 0);
  WriteU32(output, 54);
  WriteU32(output, 40);
  WriteU32(output, width);
  WriteU32(output, height);
  WriteU16(output, 1);
  WriteU16(output, 32);
  WriteU32(output, 0);
  WriteU32(output, pixel_bytes);
  WriteU32(output, 2835);
  WriteU32(output, 2835);
  WriteU32(output, 0);
  WriteU32(output, 0);

  for (uint32_t y = height; y; --y) {
    const uint8_t* row =
        rgba.data() + size_t(y - 1) * width * input_pixel_bytes;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* pixel = row + size_t(x) * input_pixel_bytes;
      if (decode_mode == DecodeMode::kR10G10B10A2Unorm) {
        const uint32_t packed = uint32_t(pixel[0]) |
                                (uint32_t(pixel[1]) << 8) |
                                (uint32_t(pixel[2]) << 16) |
                                (uint32_t(pixel[3]) << 24);
        output.put(char(((packed >> 20) & 0x3FFu) * 255u / 1023u));
        output.put(char(((packed >> 10) & 0x3FFu) * 255u / 1023u));
        output.put(char((packed & 0x3FFu) * 255u / 1023u));
      } else if (decode_mode == DecodeMode::kRgba8Unorm) {
        output.put(char(pixel[2]));
        output.put(char(pixel[1]));
        output.put(char(pixel[0]));
      } else {
        const uint16_t* pixel16 =
            reinterpret_cast<const uint16_t*>(pixel);
        output.put(
            char(EncodeSrgb(DecodeChannel(pixel16[2], decode_mode) *
                            linear_exposure)));
        output.put(
            char(EncodeSrgb(DecodeChannel(pixel16[1], decode_mode) *
                            linear_exposure)));
        output.put(
            char(EncodeSrgb(DecodeChannel(pixel16[0], decode_mode) *
                            linear_exposure)));
      }
      output.put(char(255));
    }
  }
  if (!output) {
    std::cerr << "output write failed\n";
    return 1;
  }
  std::cout << "decoded " << width << 'x' << height << ' '
            << decode_mode_name << " exposure=" << linear_exposure
            << " to BMP\n";
  return 0;
}
