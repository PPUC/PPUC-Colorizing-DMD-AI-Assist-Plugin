#pragma once

// The retheming template: the project stripped down to what a new theme keeps of an existing
// colorization (see docs/plan.md, "Retheming template"). The original frames are read as text
// lines and graphics. A text line where any of it varies (lies under the frame's comparison
// mask: scores, bonus counts, ball, player, credits) stays whole with its colors and the
// dynamic zones that reach it, labels included (BALL 1, +0 BONUS X); everything else is blacked
// out, graphics always, so no artwork of the old theme is left. The assistant corrects the rule
// where it is wrong and finds the frames whose text is kept although nothing in them varies:
// text any game has (BONUS 12X, MULTIBALL, SHOOT AGAIN, PLAYER 1).
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

// A line of text in an original frame: glyphs of a DMD font side by side (letters, digits and
// their punctuation), told from graphics by their size and by their neighbours.
struct TextLine {
    // The bounding box, in original pixels (x1, y1 exclusive).
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    // The lit pixels of its glyphs, as indexes into the original.
    std::vector<uint32_t> pixels;
};

// The text lines of an original frame; lit pixels in none of them are graphics.
[[nodiscard]] std::vector<TextLine> findTextLines(const Plane<uint8_t>& original);

// What the assistant (and the author) decided, by frame.
struct RethemeChoices {
    // Keep all their text with its colors and dynamic zones (their graphics, backgrounds and
    // rotations go as everywhere).
    std::set<uint32_t> wholeFrames;
    // Static content kept in its colors (BONUS of BONUS 2X).
    std::map<uint32_t, std::vector<Area>> keepAreas;
    // Text lines and dynamic zones kept also where no comparison mask lies under them.
    std::map<uint32_t, std::vector<Area>> keepDynamic;
    // Text lines and dynamic zones dropped also where a comparison mask lies under them.
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
    uint32_t textLines = 0;
    uint32_t keptTextLines = 0;
    // Lit pixels of the original, and of them those of text.
    uint32_t litPixels = 0;
    uint32_t textPixels = 0;
    // Text lines in a dynamic zone that are not kept: the author colored them by their shades,
    // perhaps because they vary although no mask lies under them.
    uint32_t zonedTextLinesDropped = 0;
};

// The strip of one frame.
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
    // Frames where the rule may be wrong: text in a dynamic zone that is not kept.
    std::vector<uint32_t> doubtful;
    // A few frames that show what the section has, see representativeFrames().
    std::vector<uint32_t> representative;
    // Frames that are mostly text of which nothing is kept (MULTIBALL, PLAYER TWO): how many,
    // and those of them that differ, candidates for keep_frames.
    uint32_t textFrames = 0;
    std::vector<uint32_t> textCandidates;
};

// Per section (frames before the first section under "(no section)"), the rule with the
// choices applied.
[[nodiscard]] std::vector<SectionSurvey> survey(const Snapshot& snapshot,
                                                const RethemeChoices& choices);

// Frames of the list that look different: the first, then each one whose original differs from
// every one taken in at least a sixth of its pixels, at most `limit`. For an overview of a
// section without looking at all its frames.
[[nodiscard]] std::vector<uint32_t> representativeFrames(const Snapshot& snapshot,
                                                         const std::vector<uint32_t>& frames,
                                                         std::size_t limit);

// The frame's data in the snapshot, for stripFrame().
[[nodiscard]] RethemeFrame rethemeFrame(const Snapshot& snapshot, uint32_t frame);

// The choices from a tool's arguments: keep_frames (frame indexes), keep_areas, keep_dynamic and
// drop_dynamic (lists of {frames: [...] and/or section: "<name>", x, y, width, height}). Throws
// std::invalid_argument with a message for the assistant.
[[nodiscard]] RethemeChoices parseChoices(const nlohmann::json& arguments, const Snapshot& snapshot);

}  // namespace aiassist
