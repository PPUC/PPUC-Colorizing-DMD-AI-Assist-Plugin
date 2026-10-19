#pragma once

// The retheming template: the project stripped down to what a new theme keeps of an existing
// colorization (see docs/plan.md, "Retheming template"). What varies in a frame (scores, bonus
// counts, ball, player, credits) is where its dynamic zones lie under its comparison mask; that
// stays with its colors, everything else gets the plain look of a frame fresh from a dump. The
// assistant corrects the rule where it is wrong and names frames to keep whole.
//
// Pure functions over the project's data, without the editor, so they can be tested.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor.h"

namespace aiassist {

// A rectangle in the original frame's pixels.
struct Area {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool contains(int px, int py) const
    {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

// What the assistant (and the author) decided, by frame.
struct RethemeChoices {
    // Kept as they are: colors, dynamic zones, background, rotations.
    std::set<uint32_t> wholeFrames;
    // Static content kept in its colors (BONUS of BONUS 2X).
    std::map<uint32_t, std::vector<Area>> keepAreas;
    // Dynamic zones kept also where no comparison mask lies under them.
    std::map<uint32_t, std::vector<Area>> keepDynamic;
    // Dynamic zones dropped also where a comparison mask lies under them.
    std::map<uint32_t, std::vector<Area>> dropDynamic;
};

// What a frame needs for the rule.
struct RethemeFrame {
    const Plane<uint8_t>* original = nullptr;
    // The frame's comparison mask (1: ignored by the comparison); nullptr without one.
    const Plane<uint8_t>* comparisonMask = nullptr;
    // The dynamic zones on the dynamic plane; nullptr or empty without any.
    const Plane<uint8_t>* zones = nullptr;
};

struct FrameStrip {
    // Original size: 1 where the colorized picture stays.
    std::vector<uint8_t> keep;
    // The dynamic zones after the strip, when they change.
    std::optional<Plane<uint8_t>> zones;
    uint32_t dynamicPixels = 0;
    uint32_t keptDynamicPixels = 0;
};

// The strip of one frame that is not kept whole.
[[nodiscard]] FrameStrip stripFrame(const RethemeFrame& frame,
                                    uint32_t frameIndex,
                                    const RethemeChoices& choices);

struct SectionSurvey {
    std::string name;
    uint32_t firstFrame = 0;
    uint32_t frames = 0;
    uint32_t framesWithZones = 0;
    uint32_t framesWithBackground = 0;
    uint64_t dynamicPixels = 0;
    // Of those, the ones the rule keeps.
    uint64_t keptDynamicPixels = 0;
    // Frames where the rule may be wrong: dynamic zones without a comparison mask, or less than
    // half of them under it.
    std::vector<uint32_t> doubtful;
};

// Per section (frames before the first section under "(no section)"), the rule with the
// choices applied.
[[nodiscard]] std::vector<SectionSurvey> survey(const Snapshot& snapshot,
                                                const RethemeChoices& choices);

// The frame's data in the snapshot, for stripFrame().
[[nodiscard]] RethemeFrame rethemeFrame(const Snapshot& snapshot, uint32_t frame);

// The choices from a tool's arguments: keep_frames (frame indexes), keep_areas, keep_dynamic and
// drop_dynamic (lists of {frames: [...] and/or section: "<name>", x, y, width, height}). Throws
// std::invalid_argument with a message for the assistant.
[[nodiscard]] RethemeChoices parseChoices(const nlohmann::json& arguments, const Snapshot& snapshot);

}  // namespace aiassist
