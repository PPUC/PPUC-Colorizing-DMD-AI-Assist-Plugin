#include "Identify.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

namespace aiassist {

namespace {

// A frame's comparison: its mask (CDMDPI_NONE without one) and shape mode.
using Comparison = std::pair<uint32_t, bool>;

Comparison comparisonOf(const Snapshot& snapshot, uint32_t frame)
{
    const ColorizingDMDFrameInfo& info = snapshot.frames[frame];
    const uint32_t mask =
        info.comparisonMask < snapshot.comparisonMasks.size() ? info.comparisonMask : CDMDPI_NONE;
    return {mask, info.shapeMode != 0};
}

const Plane<uint8_t>* maskOf(const Snapshot& snapshot, const Comparison& comparison,
                             const Plane<uint8_t>& original)
{
    if (comparison.first == CDMDPI_NONE) {
        return nullptr;
    }
    const Plane<uint8_t>& mask = snapshot.comparisonMasks[comparison.first];
    return mask.pixels.size() == original.pixels.size() ? &mask : nullptr;
}

// The pixels an original shows a comparison, as a key: the ignored ones left out, in shape
// mode only lit or not.
std::string comparedKey(const Plane<uint8_t>& original, const Plane<uint8_t>* mask, bool shape)
{
    std::string key;
    key.reserve(original.pixels.size());
    for (std::size_t i = 0; i < original.pixels.size(); ++i) {
        if (mask && mask->pixels[i]) {
            continue;
        }
        const uint8_t value = original.pixels[i];
        key.push_back(static_cast<char>(shape ? (value ? 1 : 0) : value));
    }
    return key;
}

char shadeChar(uint8_t shade, bool dots)
{
    static const char* const kDigits =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/";
    if (shade == 0 && dots) {
        return '.';
    }
    return shade < 64 ? kDigits[shade] : '?';
}

int shadeValue(char c)
{
    if (c == '.') {
        return 0;
    }
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'Z') {
        return 36 + (c - 'A');
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::vector<MaskUse> maskUses(const Snapshot& snapshot)
{
    std::vector<MaskUse> uses(snapshot.comparisonMasks.size());
    for (uint32_t index = 0; index < uses.size(); ++index) {
        MaskUse& use = uses[index];
        use.mask = index;
        const Plane<uint8_t>& mask = snapshot.comparisonMasks[index];
        int x0 = static_cast<int>(mask.width);
        int y0 = static_cast<int>(mask.height);
        int x1 = 0;
        int y1 = 0;
        for (uint32_t y = 0; y < mask.height; ++y) {
            for (uint32_t x = 0; x < mask.width; ++x) {
                if (mask.at(x, y)) {
                    ++use.ignored;
                    continue;
                }
                ++use.compared;
                x0 = std::min(x0, static_cast<int>(x));
                y0 = std::min(y0, static_cast<int>(y));
                x1 = std::max(x1, static_cast<int>(x) + 1);
                y1 = std::max(y1, static_cast<int>(y) + 1);
            }
        }
        if (use.compared) {
            use.x0 = x0;
            use.y0 = y0;
            use.x1 = x1;
            use.y1 = y1;
        }
    }
    for (uint32_t frame = 0; frame < snapshot.frames.size(); ++frame) {
        const uint32_t mask = snapshot.frames[frame].comparisonMask;
        if (mask >= uses.size()) {
            continue;
        }
        MaskUse& use = uses[mask];
        const Plane<uint8_t>& original = snapshot.originals[frame];
        const Plane<uint8_t>& pixels = snapshot.comparisonMasks[mask];
        uint32_t lit = 0;
        if (pixels.pixels.size() == original.pixels.size()) {
            for (std::size_t i = 0; i < original.pixels.size(); ++i) {
                lit += !pixels.pixels[i] && original.pixels[i] ? 1 : 0;
            }
        }
        use.fewestLitCompared = use.frames.empty() ? lit : std::min(use.fewestLitCompared, lit);
        use.frames.push_back(frame);
    }
    return uses;
}

bool matchesFrame(const Snapshot& snapshot, uint32_t frame, const Plane<uint8_t>& original)
{
    const Plane<uint8_t>& own = snapshot.originals[frame];
    if (own.pixels.size() != original.pixels.size()) {
        return false;
    }
    const Comparison comparison = comparisonOf(snapshot, frame);
    const Plane<uint8_t>* mask = maskOf(snapshot, comparison, own);
    for (std::size_t i = 0; i < own.pixels.size(); ++i) {
        if (mask && mask->pixels[i]) {
            continue;
        }
        const bool equal = comparison.second ? (own.pixels[i] != 0) == (original.pixels[i] != 0)
                                             : own.pixels[i] == original.pixels[i];
        if (!equal) {
            return false;
        }
    }
    return true;
}

std::vector<uint32_t> matchingFrames(const Snapshot& snapshot, const Plane<uint8_t>& original)
{
    std::vector<uint32_t> frames;
    for (uint32_t frame = 0; frame < snapshot.frames.size(); ++frame) {
        if (matchesFrame(snapshot, frame, original)) {
            frames.push_back(frame);
        }
    }
    return frames;
}

std::vector<Catch> findCatches(const Snapshot& snapshot)
{
    const uint32_t frameCount = static_cast<uint32_t>(snapshot.frames.size());
    std::map<Comparison, std::vector<uint32_t>> byComparison;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        byComparison[comparisonOf(snapshot, frame)].push_back(frame);
    }
    std::vector<Catch> catches;
    for (const auto& [comparison, members] : byComparison) {
        // Every frame's original as this comparison sees it; the frames of the comparison
        // catch the frames with their key.
        std::unordered_map<std::string, std::vector<uint32_t>> byKey;
        std::vector<std::string> keys(frameCount);
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            const Plane<uint8_t>& original = snapshot.originals[frame];
            keys[frame] =
                comparedKey(original, maskOf(snapshot, comparison, original), comparison.second);
            byKey[keys[frame]].push_back(frame);
        }
        for (uint32_t frame : members) {
            Catch found;
            found.frame = frame;
            const Plane<uint8_t>& own = snapshot.originals[frame];
            for (uint32_t other : byKey[keys[frame]]) {
                if (other == frame || snapshot.originals[other].pixels == own.pixels) {
                    continue;
                }
                (comparisonOf(snapshot, other) == comparison ? found.sameComparison
                                                             : found.otherComparison)
                    .push_back(other);
            }
            if (!found.otherComparison.empty() || !found.sameComparison.empty()) {
                catches.push_back(std::move(found));
            }
        }
    }
    std::stable_sort(catches.begin(), catches.end(), [](const Catch& a, const Catch& b) {
        if (a.otherComparison.size() != b.otherComparison.size()) {
            return a.otherComparison.size() > b.otherComparison.size();
        }
        return a.frame < b.frame;
    });
    return catches;
}

std::string originalText(const Plane<uint8_t>& original, bool dots)
{
    std::string text;
    text.reserve((original.width + 1) * original.height);
    for (uint32_t y = 0; y < original.height; ++y) {
        for (uint32_t x = 0; x < original.width; ++x) {
            text.push_back(shadeChar(original.at(x, y), dots));
        }
        text.push_back('\n');
    }
    return text;
}

Plane<uint8_t> parseOriginalText(const std::vector<std::string>& rows, uint32_t width,
                                 uint32_t height, uint32_t colorCount, std::string& error)
{
    Plane<uint8_t> plane;
    if (rows.size() != height) {
        error = "The frame needs " + std::to_string(height) + " rows, not " +
                std::to_string(rows.size()) + ".";
        return plane;
    }
    plane.width = width;
    plane.height = height;
    plane.pixels.reserve(static_cast<std::size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        if (rows[y].size() != width) {
            error = "Row " + std::to_string(y) + " has " + std::to_string(rows[y].size()) +
                    " characters, not " + std::to_string(width) + ".";
            return Plane<uint8_t>{};
        }
        for (uint32_t x = 0; x < width; ++x) {
            const int value = shadeValue(rows[y][x]);
            if (value < 0 || static_cast<uint32_t>(value) >= colorCount) {
                error = "Row " + std::to_string(y) + ", column " + std::to_string(x) +
                        ": '" + std::string(1, rows[y][x]) + "' is no shade of " +
                        std::to_string(colorCount) + ".";
                return Plane<uint8_t>{};
            }
            plane.pixels.push_back(static_cast<uint8_t>(value));
        }
    }
    return plane;
}

}  // namespace aiassist
