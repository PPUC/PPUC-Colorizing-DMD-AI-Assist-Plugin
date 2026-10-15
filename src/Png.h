#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aiassist {

// An RGB picture, 8 bits per channel, row-major.
struct Picture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb;

    Picture() = default;
    Picture(int w, int h) : width(w), height(h), rgb(static_cast<std::size_t>(w) * h * 3, 0) {}
    void set(int x, int y, uint8_t r, uint8_t g, uint8_t b)
    {
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
        rgb[i] = r;
        rgb[i + 1] = g;
        rgb[i + 2] = b;
    }
};

// The picture as a PNG file (uncompressed deflate: small pictures, no zlib needed).
[[nodiscard]] std::string encodePng(const Picture& picture);
[[nodiscard]] std::string base64(const std::string& bytes);

}  // namespace aiassist
