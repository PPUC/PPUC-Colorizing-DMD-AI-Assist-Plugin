// Identification as the runtime does it, on a small made-up project: a frame whose mask ignores
// all of a score but its last digit catches every score frame ending in that digit.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "Identify.h"

namespace {

using namespace aiassist;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: identify: " << message << '\n';
        std::exit(1);
    }
}

constexpr uint32_t kWidth = 16;
constexpr uint32_t kHeight = 4;

Plane<uint8_t> plane(uint8_t value)
{
    Plane<uint8_t> result;
    result.width = kWidth;
    result.height = kHeight;
    result.pixels.assign(kWidth * kHeight, value);
    return result;
}

// Digits as one lit column each: the digit's value as the height from the bottom, 0 as a full
// column of shade 3.
void digits(Plane<uint8_t>& p, uint32_t x0, const std::string& text)
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        const int value = text[i] - '0';
        const uint32_t lit = value == 0 ? kHeight : static_cast<uint32_t>(value) % kHeight;
        for (uint32_t y = kHeight - lit; y < kHeight; ++y) {
            p.pixels[y * kWidth + x0 + i] = 3;
        }
    }
}

ColorizingDMDFrameInfo frameInfo(uint32_t mask, bool shape = false)
{
    ColorizingDMDFrameInfo info{};
    info.comparisonMask = mask;
    info.shapeMode = shape ? 1 : 0;
    info.background = CDMDPI_NONE;
    info.triggerId = CDMDPI_NONE;
    info.section = CDMDPI_NONE;
    return info;
}

// Frame 0: the bonus frame "5000000" at x 4, mask 0 ignores x 4..9 (all digits but the last).
// Frame 1: a score "1230" at x 7 (its last digit where the bonus has its last zero), mask 1
// ignores x 7..9 (the score but the last digit, which is always 0). Frame 2: a word frame
// without a mask. Frame 3: frame 2 again (a duplicate).
Snapshot makeSnapshot()
{
    Snapshot snapshot;
    snapshot.project.hasProject = 1;
    snapshot.project.frameCount = 4;
    snapshot.project.colorCount = 4;
    snapshot.project.originalWidth = kWidth;
    snapshot.project.originalHeight = kHeight;

    Plane<uint8_t> bonus = plane(0);
    digits(bonus, 4, "5000000");
    Plane<uint8_t> score = plane(0);
    digits(score, 7, "1230");
    Plane<uint8_t> word = plane(0);
    for (uint32_t x = 0; x < 3; ++x) {
        word.pixels[x] = 2;
    }

    Plane<uint8_t> bonusMask = plane(0);
    Plane<uint8_t> scoreMask = plane(0);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 4; x < 10; ++x) {
            bonusMask.pixels[y * kWidth + x] = 1;
        }
        for (uint32_t x = 7; x < 10; ++x) {
            scoreMask.pixels[y * kWidth + x] = 1;
        }
    }
    snapshot.originals = {bonus, score, word, word};
    snapshot.comparisonMasks = {bonusMask, scoreMask};
    snapshot.frames = {frameInfo(0), frameInfo(1), frameInfo(CDMDPI_NONE),
                       frameInfo(CDMDPI_NONE)};
    return snapshot;
}

void testMaskUses()
{
    const Snapshot snapshot = makeSnapshot();
    const std::vector<MaskUse> uses = maskUses(snapshot);
    expect(uses.size() == 2, "a use per mask");
    expect(uses[0].compared == kWidth * kHeight - 6 * kHeight, "mask 0 compares all but 6 columns");
    expect(uses[0].x0 == 0 && uses[0].x1 == static_cast<int>(kWidth), "mask 0's box");
    // The bonus frame shows the comparison only its last zero: one full column.
    expect(uses[0].fewestLitCompared == kHeight, "mask 0 compares one lit column");
    expect(uses[0].frames == std::vector<uint32_t>{0}, "mask 0 is frame 0's");
}

void testMatching()
{
    const Snapshot snapshot = makeSnapshot();
    expect(matchesFrame(snapshot, 0, snapshot.originals[1]), "the bonus frame matches the score");
    expect(!matchesFrame(snapshot, 1, snapshot.originals[0]),
           "the score frame does not match the bonus (its 5 lies outside the score's mask)");

    // Any score ending in 0 at the bonus's last digit, which the project does not have.
    Plane<uint8_t> other = plane(0);
    digits(other, 6, "99990");
    expect(matchingFrames(snapshot, other) == std::vector<uint32_t>{0},
           "an unknown score is caught by the bonus frame only");
    Plane<uint8_t> endsInOne = plane(0);
    digits(endsInOne, 7, "1231");
    expect(matchingFrames(snapshot, endsInOne).empty(), "a score ending in 1 matches nothing");
}

void testCatches()
{
    const Snapshot snapshot = makeSnapshot();
    const std::vector<Catch> catches = findCatches(snapshot);
    expect(catches.size() == 1, "one frame catches others (duplicates are not catches)");
    expect(catches[0].frame == 0, "the bonus frame catches");
    expect(catches[0].otherComparison == std::vector<uint32_t>{1}, "it catches the score frame");
    expect(catches[0].sameComparison.empty(), "nothing under its own comparison");
}

void testShape()
{
    Snapshot snapshot = makeSnapshot();
    snapshot.frames[2].shapeMode = 1;
    Plane<uint8_t> brighter = snapshot.originals[2];
    for (uint32_t x = 0; x < 3; ++x) {
        brighter.pixels[x] = 3;
    }
    expect(matchesFrame(snapshot, 2, brighter), "shape mode compares lit or not");
    expect(!matchesFrame(snapshot, 3, brighter), "without shape mode the shade counts");
}

void testText()
{
    const Snapshot snapshot = makeSnapshot();
    const std::string text = originalText(snapshot.originals[0], true);
    expect(text.substr(0, kWidth + 1) == ".....333333.....\n", "the first row as text");
    std::vector<std::string> rows;
    for (uint32_t y = 0; y < kHeight; ++y) {
        rows.push_back(text.substr(y * (kWidth + 1), kWidth));
    }
    std::string error;
    const Plane<uint8_t> parsed = parseOriginalText(rows, kWidth, kHeight, 4, error);
    expect(parsed.pixels == snapshot.originals[0].pixels, "text reads back: " + error);
    rows[1][0] = '7';
    expect(parseOriginalText(rows, kWidth, kHeight, 4, error).empty(), "a shade above 3");
    rows.pop_back();
    expect(parseOriginalText(rows, kWidth, kHeight, 4, error).empty(), "a row missing");
}

}  // namespace

void runIdentifyTests()
{
    testMaskUses();
    testMatching();
    testCatches();
    testShape();
    testText();
    std::cout << "identify tests passed\n";
}
