#include "Png.h"

#include <algorithm>
#include <array>

namespace aiassist {

namespace {

uint32_t crc32(const uint8_t* data, std::size_t size, uint32_t crc = 0)
{
    static const std::array<uint32_t, 256> table = []() {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

void put32(std::string& out, uint32_t value)
{
    out.push_back(static_cast<char>(value >> 24));
    out.push_back(static_cast<char>(value >> 16));
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void chunk(std::string& out, const char* type, const std::string& data)
{
    put32(out, static_cast<uint32_t>(data.size()));
    std::string body(type, 4);
    body += data;
    out += body;
    put32(out, crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
}

}  // namespace

std::string encodePng(const Picture& picture)
{
    std::string out("\x89PNG\r\n\x1a\n", 8);
    std::string header;
    put32(header, static_cast<uint32_t>(picture.width));
    put32(header, static_cast<uint32_t>(picture.height));
    header += std::string("\x08\x02\x00\x00\x00", 5);  // 8 bit, RGB, deflate, no filter, no interlace
    chunk(out, "IHDR", header);

    // Each row: filter type 0, then the pixels.
    std::string raw;
    const std::size_t rowBytes = static_cast<std::size_t>(picture.width) * 3;
    for (int y = 0; y < picture.height; ++y) {
        raw.push_back('\0');
        raw.append(reinterpret_cast<const char*>(picture.rgb.data()) + y * rowBytes, rowBytes);
    }
    // zlib stream of stored blocks.
    std::string zlib("\x78\x01", 2);
    std::size_t offset = 0;
    do {
        const std::size_t size = std::min<std::size_t>(65535, raw.size() - offset);
        const bool last = offset + size == raw.size();
        zlib.push_back(last ? '\x01' : '\x00');
        zlib.push_back(static_cast<char>(size & 0xff));
        zlib.push_back(static_cast<char>(size >> 8));
        zlib.push_back(static_cast<char>(~size & 0xff));
        zlib.push_back(static_cast<char>((~size >> 8) & 0xff));
        zlib.append(raw, offset, size);
        offset += size;
    } while (offset < raw.size());
    uint32_t a = 1;
    uint32_t b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32(zlib, (b << 16) | a);
    chunk(out, "IDAT", zlib);
    chunk(out, "IEND", std::string());
    return out;
}

std::string base64(const std::string& bytes)
{
    static const char* digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) |
                           uint8_t(bytes[i + 2]);
        out += digits[v >> 18];
        out += digits[(v >> 12) & 63];
        out += digits[(v >> 6) & 63];
        out += digits[v & 63];
    }
    if (i < bytes.size()) {
        uint32_t v = uint8_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) {
            v |= uint8_t(bytes[i + 1]) << 8;
        }
        out += digits[v >> 18];
        out += digits[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? digits[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace aiassist
