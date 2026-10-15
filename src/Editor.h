#pragma once

// The editor API (ColorizingDMDPlugin.h) with C++ types, for the server's threads: every call
// runs on the editor's main thread through the runner (the API is main-thread only) and waits
// for it. Each call costs a round trip to the main thread (up to the editor's 20 ms bus tick),
// so tools that need many frames take a Snapshot, all in one go.

#include <cstdint>
#include <functional>
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

// The project at one moment: what the analyses need of every frame.
struct Snapshot {
    ColorizingDMDProjectInfo project{};
    std::vector<ColorizingDMDFrameInfo> frames;
    std::vector<Plane<uint8_t>> originals;
    std::vector<Plane<uint8_t>> comparisonMasks;
    std::vector<Section> sections;
};

class Editor {
public:
    using Runner = std::function<void(const std::function<void()>&)>;

    Editor(const ColorizingDMDEditorAPI* api, Runner runOnMainThread)
        : m_api(api)
        , m_run(std::move(runOnMainThread))
    {
    }

    // Everything of the project the analyses use, read on the main thread in one go; nullopt
    // without a project.
    [[nodiscard]] std::optional<Snapshot> snapshot() const
    {
        std::optional<Snapshot> result;
        m_run([&]() {
            ColorizingDMDProjectInfo info{};
            if (!m_api->GetProjectInfo(&info) || !info.hasProject) {
                return;
            }
            Snapshot snapshot;
            snapshot.project = info;
            snapshot.frames.resize(info.frameCount);
            snapshot.originals.resize(info.frameCount);
            for (uint32_t frame = 0; frame < info.frameCount; ++frame) {
                m_api->GetFrameInfo(frame, &snapshot.frames[frame]);
                snapshot.originals[frame] = readOriginal(frame);
            }
            for (uint32_t mask = 0; mask < info.comparisonMaskCount; ++mask) {
                snapshot.comparisonMasks.push_back(readComparisonMask(mask));
            }
            snapshot.sections = readSections();
            result = std::move(snapshot);
        });
        return result;
    }

    // The colorized SD pictures of the frames, in one go.
    [[nodiscard]] std::vector<Plane<uint16_t>> colorizedFrames(const std::vector<uint32_t>& frames) const
    {
        std::vector<Plane<uint16_t>> result;
        m_run([&]() {
            for (uint32_t frame : frames) {
                result.push_back(readColorized(frame, false));
            }
        });
        return result;
    }

    [[nodiscard]] std::optional<ColorizingDMDProjectInfo> project() const
    {
        std::optional<ColorizingDMDProjectInfo> result;
        m_run([&]() {
            ColorizingDMDProjectInfo info{};
            if (m_api->GetProjectInfo(&info) && info.hasProject) {
                result = info;
            }
        });
        return result;
    }

    [[nodiscard]] std::optional<ColorizingDMDFrameInfo> frame(uint32_t index) const
    {
        std::optional<ColorizingDMDFrameInfo> result;
        m_run([&]() {
            ColorizingDMDFrameInfo info{};
            if (m_api->GetFrameInfo(index, &info)) {
                result = info;
            }
        });
        return result;
    }

    // ---- The API itself: on the main thread only (inside m_run) ---------------------------
    [[nodiscard]] Plane<uint8_t> readOriginal(uint32_t frame) const
    {
        Plane<uint8_t> plane;
        const uint32_t size = m_api->GetOriginalFrame(frame, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetOriginalFrame(frame, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] Plane<uint16_t> readColorized(uint32_t frame, bool hd) const
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

    [[nodiscard]] Plane<uint8_t> readComparisonMask(uint32_t mask) const
    {
        Plane<uint8_t> plane;
        const uint32_t size = m_api->GetComparisonMask(mask, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetComparisonMask(mask, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] Plane<uint8_t> readDynamicMask(uint32_t frame) const
    {
        Plane<uint8_t> plane;
        const uint32_t size = m_api->GetDynamicMask(frame, nullptr, 0, &plane.width, &plane.height);
        plane.pixels.resize(size);
        if (size) {
            m_api->GetDynamicMask(frame, plane.pixels.data(), size, nullptr, nullptr);
        }
        return plane;
    }

    [[nodiscard]] std::vector<Section> readSections() const
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
        uint32_t frame = CDMDPI_NONE;
        m_run([&]() { frame = m_api->GetCurrentFrame(); });
        return frame == CDMDPI_NONE ? std::nullopt : std::optional<uint32_t>(frame);
    }

    [[nodiscard]] std::vector<uint32_t> selectedFrames() const
    {
        std::vector<uint32_t> frames;
        m_run([&]() {
            frames.resize(m_api->GetSelectedFrames(nullptr, 0));
            if (!frames.empty()) {
                m_api->GetSelectedFrames(frames.data(), static_cast<uint32_t>(frames.size()));
            }
        });
        return frames;
    }

    bool showFrames(uint32_t current, const std::vector<uint32_t>& frames) const
    {
        bool shown = false;
        m_run([&]() {
            shown = m_api->ShowFrames(current, frames.data(),
                                      static_cast<uint32_t>(frames.size())) != 0;
        });
        return shown;
    }

    // ---- From any thread ------------------------------------------------------------------
    [[nodiscard]] Plane<uint8_t> original(uint32_t frame) const
    {
        Plane<uint8_t> plane;
        m_run([&]() { plane = readOriginal(frame); });
        return plane;
    }
    [[nodiscard]] Plane<uint16_t> colorized(uint32_t frame, bool hd) const
    {
        Plane<uint16_t> plane;
        m_run([&]() { plane = readColorized(frame, hd); });
        return plane;
    }
    [[nodiscard]] Plane<uint8_t> comparisonMask(uint32_t mask) const
    {
        Plane<uint8_t> plane;
        m_run([&]() { plane = readComparisonMask(mask); });
        return plane;
    }
    [[nodiscard]] Plane<uint8_t> dynamicMask(uint32_t frame) const
    {
        Plane<uint8_t> plane;
        m_run([&]() { plane = readDynamicMask(frame); });
        return plane;
    }
    [[nodiscard]] std::vector<Section> sections() const
    {
        std::vector<Section> result;
        m_run([&]() { result = readSections(); });
        return result;
    }

private:
    const ColorizingDMDEditorAPI* m_api;
    Runner m_run;
};

}  // namespace aiassist
