// image.h — PNG, JPEG (baseline + progressive), GIF and BMP decoders. No libraries.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace image {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4, R,G,B,A
};

constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;

// Detects the format from the bytes. Returns false and sets err on failure.
bool decode(const uint8_t *data, size_t size, Image &out, std::string &err);

}  // namespace image
