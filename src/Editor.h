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
    // The dynamic zones of the frames that have any (empty for the others); only when the
    // snapshot was asked for them.
    std::vector<Plane<uint8_t>> dynamicMasks;
};

// The editing part of the API (version 2) inside one edit batch, see Editor::edit(). Each call
// returns whether the editor made the change.
class EditSession {
public:
    EditSession(const ColorizingDMDEditorAPI* api, uint32_t endpointId)
        : m_api(api)
        , m_endpoint(endpointId)
    {
    }

    // fill: RGB565, or CDMDPI_NONE for the original's plain look.
    bool resetFramePixels(uint32_t frame, const std::vector<uint8_t>& keep, uint32_t fill) const
    {
        return m_api->ResetFramePixels(m_endpoint, frame, keep.data(),
                                       static_cast<uint32_t>(keep.size()), fill) != 0;
    }
    bool setDynamicZones(uint32_t frame, const std::vector<uint8_t>& sets) const
    {
        return m_api->SetDynamicZones(m_endpoint, frame, sets.data(),
                                      static_cast<uint32_t>(sets.size())) != 0;
    }
    bool setFrameBackground(uint32_t frame, uint32_t background) const
    {
        return m_api->SetFrameBackground(m_endpoint, frame, background) != 0;
    }
    bool deleteBackground(uint32_t background) const
    {
        return m_api->DeleteBackground(m_endpoint, background) != 0;
    }
    bool clearColorRotations(uint32_t frame) const
    {
        return m_api->ClearColorRotations(m_endpoint, frame) != 0;
    }

private:
    const ColorizingDMDEditorAPI* m_api;
    uint32_t m_endpoint;
};

class Editor {
public:
    using Runner = std::function<void(const std::function<void()>&)>;

    Editor(const ColorizingDMDEditorAPI* api, uint32_t endpointId, Runner runOnMainThread)
        : m_api(api)
        , m_endpoint(endpointId)
        , m_run(std::move(runOnMainThread))
    {
    }

    [[nodiscard]] int apiVersion() const { return m_api->version; }
    [[nodiscard]] bool canEdit() const { return m_api->version >= 2; }

    // Runs the changes as one edit batch under the name, in one call on the main thread, so
    // the author cannot edit in between: recorded as one undo step when changes returns true,
    // taken back otherwise. Returns false when no batch could begin (no project, or the editor's
    // API has no editing part).
    bool edit(const std::string& name, const std::function<bool(const EditSession&)>& changes) const
    {
        if (!canEdit()) {
            return false;
        }
        bool begun = false;
        m_run([&]() {
            if (!m_api->BeginEdit(m_endpoint, name.c_str())) {
                return;
            }
            begun = true;
            bool apply = false;
            try {
                apply = changes(EditSession(m_api, m_endpoint));
            } catch (...) {
                m_api->EndEdit(m_endpoint, 0);
                throw;
            }
            m_api->EndEdit(m_endpoint, apply ? 1 : 0);
        });
        return begun;
    }

    // The plain color of each original shade (RGB565); empty before API version 2.
    [[nodiscard]] std::vector<uint16_t> plainColors() const
    {
        std::vector<uint16_t> colors;
        if (!canEdit()) {
            return colors;
        }
        m_run([&]() {
            colors.resize(m_api->GetPlainColors(nullptr, 0));
            if (!colors.empty()) {
                m_api->GetPlainColors(colors.data(), static_cast<uint32_t>(colors.size()));
            }
        });
        return colors;
    }

    // A frame's dynamic sets: colors[set * shades + shade], RGB565.
    [[nodiscard]] std::vector<uint16_t> dynamicColors(uint32_t frame) const
    {
        std::vector<uint16_t> colors;
        m_run([&]() {
            colors.resize(m_api->GetDynamicColors(frame, nullptr, 0));
            if (!colors.empty()) {
                m_api->GetDynamicColors(frame, colors.data(), static_cast<uint32_t>(colors.size()));
            }
        });
        return colors;
    }

    // Everything of the project the analyses use, read on the main thread in one go; nullopt
    // without a project.
    [[nodiscard]] std::optional<Snapshot> snapshot(bool withDynamicMasks = false) const
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
            if (withDynamicMasks) {
                snapshot.dynamicMasks.resize(info.frameCount);
                for (uint32_t frame = 0; frame < info.frameCount; ++frame) {
                    if (snapshot.frames[frame].hasDynamicPixels) {
                        snapshot.dynamicMasks[frame] = readDynamicMask(frame);
                    }
                }
            }
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
    uint32_t m_endpoint;
    Runner m_run;
};

}  // namespace aiassist
