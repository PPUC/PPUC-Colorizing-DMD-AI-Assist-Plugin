#pragma once

// The editor API (ColorizingDMDPlugin.h) with C++ types. Main thread only, as the API itself.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ColorizingDMDPlugin.h"

namespace aiassist {

template <typename T>
struct Plane {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<T> pixels;
    [[nodiscard]] bool empty() const { return pixels.empty(); }
    [[nodiscard]] T at(uint32_t x, uint32_t y) const { return pixels[y * width + x]; }
};

struct Section {
    std::string name;
    uint32_t firstFrame = 0;
};

class Editor {
public:
    explicit Editor(const ColorizingDMDEditorAPI* api) : m_api(api) {}

    [[nodiscard]] std::optional<ColorizingDMDProjectInfo> project() const
    {
        ColorizingDMDProjectInfo info{};
        if (!m_api->GetProjectInfo(&info) || !info.hasProject) {
            return std::nullopt;
        }
        return info;
    }

    [[nodiscard]] std::optional<ColorizingDMDFrameInfo> frame(uint32_t index) const
    {
        ColorizingDMDFrameInfo info{};
        if (!m_api->GetFrameInfo(index, &info)) {
            return std::nullopt;
        }
        return info;
    }

    [[nodiscard]] Plane<uint8_t> original(uint32_t frame) const
    {
        Plane<uint8_t> plane;
        const uint32_t size = m_api->GetOriginalFrame(frame, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetOriginalFrame(frame, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] Plane<uint16_t> colorized(uint32_t frame, bool hd) const
    {
        Plane<uint16_t> plane;
        const uint32_t size =
            m_api->GetColorizedFrame(frame, hd ? 1 : 0, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetColorizedFrame(frame, hd ? 1 : 0, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] Plane<uint8_t> comparisonMask(uint32_t mask) const
    {
        Plane<uint8_t> plane;
        const uint32_t size = m_api->GetComparisonMask(mask, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetComparisonMask(mask, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] Plane<uint8_t> dynamicMask(uint32_t frame) const
    {
        Plane<uint8_t> plane;
        const uint32_t size = m_api->GetDynamicMask(frame, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetDynamicMask(frame, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] std::vector<Section> sections() const
    {
        std::vector<Section> result;
        char name[64];
        uint32_t first = 0;
        for (uint32_t index = 0; m_api->GetSection(index, name, sizeof(name), &first); ++index) {
            result.push_back(Section{name, first});
        }
        return result;
    }

    [[nodiscard]] std::optional<uint32_t> currentFrame() const
    {
        const uint32_t frame = m_api->GetCurrentFrame();
        return frame == CDMDPI_NONE ? std::nullopt : std::optional<uint32_t>(frame);
    }

    [[nodiscard]] std::vector<uint32_t> selectedFrames() const
    {
        std::vector<uint32_t> frames(m_api->GetSelectedFrames(nullptr, 0));
        if (!frames.empty()) {
            m_api->GetSelectedFrames(frames.data(), static_cast<uint32_t>(frames.size()));
        }
        return frames;
    }

    bool showFrames(uint32_t current, const std::vector<uint32_t>& frames) const
    {
        return m_api->ShowFrames(current, frames.data(), static_cast<uint32_t>(frames.size())) != 0;
    }

private:
    const ColorizingDMDEditorAPI* m_api;
};

}  // namespace aiassist
