#pragma once

// Frame identification as the runtime (libserum) does it, over the project's data: an incoming
// ROM frame is the project frame whose original it equals on the pixels that frame's comparison
// mask does not ignore (in shape mode: lit or not, instead of the shade). The runtime tries the
// frames' mask and shape pairs in frame order, starting at the frame it found last and wrapping
// around, and takes the first pair that gives a match. So when an incoming frame matches more
// than one project frame under different masks, which one it becomes depends on what was shown
// before: a mask that ignores too much catches frames that should be identified as others.
//
// Pure functions over the project's data, without the editor, so they can be tested.

#include <cstdint>
#include <string>
#include <vector>

#include "Editor.h"

namespace aiassist {

// A comparison mask and how the project uses it.
struct MaskUse {
    uint32_t mask = 0;
    // Original pixels the comparison looks at and ignores.
    uint32_t compared = 0;
    uint32_t ignored = 0;
    // The box around the compared pixels (x1, y1 exclusive); all 0 when nothing is compared.
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    // Lit pixels of the frames' originals the comparison looks at, the fewest of any frame
    // using the mask: how much content identifies them.
    uint32_t fewestLitCompared = 0;
    std::vector<uint32_t> frames;
};

[[nodiscard]] std::vector<MaskUse> maskUses(const Snapshot& snapshot);

// Whether the original equals the frame's original on the pixels the frame's comparison
// looks at (its mask and shape mode).
[[nodiscard]] bool matchesFrame(const Snapshot& snapshot, uint32_t frame,
                                const Plane<uint8_t>& original);

// The project frames an original matches, ascending; frames that start a scene are left out
// (the runtime looks for them only while it plays a scene).
[[nodiscard]] std::vector<uint32_t> matchingFrames(const Snapshot& snapshot,
                                                   const Plane<uint8_t>& original);

// A frame whose comparison catches the originals of other frames: the runtime may identify
// those as this one.
struct Catch {
    uint32_t frame = 0;
    // The frames caught whose comparison differs (another mask or shape mode): which of the
    // two they become depends on the search order, they are at risk.
    std::vector<uint32_t> otherComparison;
    // The frames caught with the same comparison but another original: under that comparison
    // they are one frame, the runtime takes the first in its search order.
    std::vector<uint32_t> sameComparison;
};

// Every frame that catches others, the most other-comparison catches first. Frames with an
// equal original are duplicates and not counted.
[[nodiscard]] std::vector<Catch> findCatches(const Snapshot& snapshot);

// An original as text, one line per row, a character per pixel: the shade as a digit 0-9 or a
// letter a-z, A-Z for 10 to 61 and the next two for 62, 63 ('.' for 0 if dots is set).
[[nodiscard]] std::string originalText(const Plane<uint8_t>& original, bool dots);
// The other way round; returns an empty plane and sets error when the text is no frame of
// width x height with shades below colorCount.
[[nodiscard]] Plane<uint8_t> parseOriginalText(const std::vector<std::string>& rows,
                                               uint32_t width, uint32_t height,
                                               uint32_t colorCount, std::string& error);

}  // namespace aiassist
