// The retheming template's rule, survey and choices, on a small made-up project.

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

#include "Retheme.h"

namespace {

using namespace aiassist;

void expect(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAIL: retheme: " << message << '\n';
        std::exit(1);
    }
}

Plane<uint8_t> plane(uint32_t width, uint32_t height, uint8_t value)
{
    Plane<uint8_t> result;
    result.width = width;
    result.height = height;
    result.pixels.assign(width * height, value);
    return result;
}

void fill(Plane<uint8_t>& p, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint8_t value)
{
    for (uint32_t y = y0; y < y0 + h; ++y) {
        for (uint32_t x = x0; x < x0 + w; ++x) {
            p.pixels[y * p.width + x] = value;
        }
    }
}

// A glyph of a 3x5 font: a ring, like a zero.
void glyph(Plane<uint8_t>& p, uint32_t x0, uint32_t y0)
{
    for (uint32_t y = 0; y < 5; ++y) {
        for (uint32_t x = 0; x < 3; ++x) {
            if (x != 1 || y == 0 || y == 4) {
                p.pixels[(y0 + y) * p.width + x0 + x] = 3;
            }
        }
    }
}

// 64x16 frames. Frame 0: a score of four glyphs at x 40..54, y 1, under the mask (x 36..63),
// in dynamic zone 3 that is wider than the score (x 36..63, y 0..6); a label of three glyphs
// right below it (y 8); a word at x 2..13, y 1 in zone 1 without the mask; a solid block of
// graphics at x 20..33, y 9..14 under its own part of the mask and in zone 2. Frame 1: like
// frame 0 without the mask. Frame 2: only a word, nothing else.
Snapshot makeSnapshot()
{
    Snapshot snapshot;
    snapshot.project.hasProject = 1;
    snapshot.project.frameCount = 3;
    snapshot.project.colorCount = 4;
    snapshot.sections = {Section{"Score", 0}, Section{"Logo", 2}};
    snapshot.frames.resize(3);
    for (uint32_t frame = 0; frame < 3; ++frame) {
        ColorizingDMDFrameInfo& info = snapshot.frames[frame];
        info.comparisonMask = CDMDPI_NONE;
        info.background = CDMDPI_NONE;
        info.section = frame < 2 ? 0 : 1;
        Plane<uint8_t> original = plane(64, 16, 0);
        if (frame < 2) {
            for (uint32_t x : {40u, 44u, 48u, 52u}) {
                glyph(original, x, 1);
            }
            for (uint32_t x : {42u, 46u, 50u}) {
                glyph(original, x, 8);
            }
            for (uint32_t x : {2u, 6u, 10u}) {
                glyph(original, x, 1);
            }
            fill(original, 20, 9, 14, 6, 2);
        } else {
            for (uint32_t x : {20u, 24u, 28u}) {
                glyph(original, x, 5);
            }
        }
        snapshot.originals.push_back(original);
    }
    snapshot.comparisonMasks.push_back(plane(64, 16, 0));
    fill(snapshot.comparisonMasks[0], 36, 0, 28, 7, 1);
    fill(snapshot.comparisonMasks[0], 20, 9, 14, 6, 1);
    snapshot.frames[0].comparisonMask = 0;
    snapshot.dynamicMasks.resize(3);
    for (uint32_t frame : {0u, 1u}) {
        snapshot.frames[frame].hasDynamicPixels = 1;
        snapshot.dynamicMasks[frame] = plane(64, 16, 255);
        fill(snapshot.dynamicMasks[frame], 36, 0, 28, 7, 3);
        fill(snapshot.dynamicMasks[frame], 0, 0, 16, 7, 1);
        fill(snapshot.dynamicMasks[frame], 18, 8, 18, 8, 2);
    }
    snapshot.frames[2].background = 4;
    return snapshot;
}

void testText()
{
    const Snapshot snapshot = makeSnapshot();
    const std::vector<TextLine> lines = findTextLines(snapshot.originals[0]);
    expect(lines.size() == 3, "three text lines; the block is graphics");
    const auto line = std::find_if(lines.begin(), lines.end(),
                                   [](const TextLine& l) { return l.x0 == 40 && l.y0 == 1; });
    expect(line != lines.end() && line->x1 == 55 && line->y1 == 6 && line->pixels.size() == 4 * 12,
           "the score is one line of four glyphs");
}

void testRule()
{
    const Snapshot snapshot = makeSnapshot();
    const RethemeChoices none;
    const FrameStrip strip = stripFrame(rethemeFrame(snapshot, 0), 0, none);
    const auto kept = [&strip](uint32_t x, uint32_t y) { return strip.keep[y * 64 + x] != 0; };
    expect(strip.textLines == 3 && strip.keptTextLines == 2,
           "the varying score and its label below are kept, the word apart is not");
    expect(kept(40, 1) && kept(42, 8) && !kept(2, 1) && !kept(25, 10),
           "kept text keeps its colors, the rest and graphics do not");
    expect(strip.zones && strip.zones->at(60, 3) == 3 && strip.zones->at(37, 0) == 3,
           "the score's zone stays whole, wider than the score");
    expect(strip.zones->at(5, 3) == 255 && strip.zones->at(25, 10) == 255,
           "the zone of the word apart and the zone of graphics go, mask or not");
    expect(strip.zonedTextLinesDropped == 1, "the word in a zone is reported");

    RethemeChoices choices;
    choices.keepDynamic[0].push_back(Area{0, 0, 16, 7});
    choices.dropDynamic[0].push_back(Area{40, 8, 20, 6});
    const FrameStrip corrected = stripFrame(rethemeFrame(snapshot, 0), 0, choices);
    expect(corrected.keep[1 * 64 + 2] && corrected.zones && corrected.zones->at(5, 3) == 1,
           "keep_dynamic keeps the word and its zone");
    expect(!corrected.keep[8 * 64 + 42], "drop_dynamic drops the label");
    choices.keepAreas[0].push_back(Area{20, 9, 2, 1});
    expect(stripFrame(rethemeFrame(snapshot, 0), 0, choices).keep[9 * 64 + 21] &&
               !stripFrame(rethemeFrame(snapshot, 0), 0, choices).keep[9 * 64 + 22],
           "keep_areas keep static pixels");

    const FrameStrip unmasked = stripFrame(rethemeFrame(snapshot, 1), 1, none);
    expect(unmasked.keptTextLines == 0 && unmasked.zones &&
               std::all_of(unmasked.zones->pixels.begin(), unmasked.zones->pixels.end(),
                           [](uint8_t set) { return set == 255; }),
           "without a mask nothing varies");

    RethemeChoices whole;
    whole.wholeFrames.insert(1);
    const FrameStrip all = stripFrame(rethemeFrame(snapshot, 1), 1, whole);
    expect(all.keptTextLines == 3 && all.keep[1 * 64 + 2] && !all.keep[10 * 64 + 25],
           "a frame in keep_frames keeps all its text, not its graphics");
}

void testSurvey()
{
    const Snapshot snapshot = makeSnapshot();
    const std::vector<SectionSurvey> sections = survey(snapshot, RethemeChoices{});
    expect(sections.size() == 2 && sections[0].name == "Score" && sections[1].name == "Logo",
           "the sections with frames, in order");
    expect(sections[0].frames == 2 && sections[0].framesWithZones == 2,
           "frames and frames with zones");
    expect(sections[0].doubtful == std::vector<uint32_t>{0, 1},
           "frames with text in a zone that is not kept are doubtful");
    expect(sections[1].framesWithBackground == 1, "backgrounds are counted");
    expect(sections[1].textFrames == 1 && sections[1].textCandidates == std::vector<uint32_t>{2},
           "a frame of only text nothing is kept of is a candidate");

    RethemeChoices choices;
    choices.wholeFrames.insert(1);
    choices.wholeFrames.insert(2);
    expect(survey(snapshot, choices)[0].doubtful == std::vector<uint32_t>{0} &&
               survey(snapshot, choices)[1].textCandidates.empty(),
           "frames kept are neither doubtful nor candidates");
}

void testRepresentatives()
{
    Snapshot snapshot = makeSnapshot();
    // Frame 1 differs from frame 0 in 2 pixels, frame 2 in more than a sixth.
    snapshot.originals[1].pixels[0] = 1;
    snapshot.originals[1].pixels[1] = 1;
    expect(representativeFrames(snapshot, {0, 1, 2}, 6) == std::vector<uint32_t>{0, 2},
           "frames that look alike are shown once");
    expect(representativeFrames(snapshot, {0, 1, 2}, 1) == std::vector<uint32_t>{0},
           "at most the limit");
}

void testChoices()
{
    const Snapshot snapshot = makeSnapshot();
    const nlohmann::json arguments = {
        {"keep_frames", {2}},
        {"keep_areas", {{{"section", "Score"}, {"x", 0}, {"y", 0}, {"width", 5}, {"height", 2}}}},
        {"drop_dynamic", {{{"frames", {1}}, {"x", 1}, {"y", 1}, {"width", 1}, {"height", 1}}}}};
    const RethemeChoices choices = parseChoices(arguments, snapshot);
    expect(choices.wholeFrames.count(2) && choices.keepAreas.size() == 2 &&
               choices.keepAreas.at(0).front().width == 5 && choices.dropDynamic.count(1),
           "frames, sections and areas are read");
    const auto refused = [&snapshot](const nlohmann::json& bad) {
        try {
            (void)parseChoices(bad, snapshot);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    expect(refused({{"keep_frames", {9}}}), "a frame that does not exist");
    expect(refused({{"keep_areas", {{{"section", "Nope"}, {"x", 0}, {"y", 0}, {"width", 1},
                                     {"height", 1}}}}}),
           "a section that does not exist");
    expect(refused({{"keep_areas", {{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}),
           "an area without frames");
    expect(refused({{"keep_areas", {{{"frames", {0}}, {"x", -1}, {"y", 0}, {"width", 1},
                                     {"height", 1}}}}}),
           "a negative coordinate");
}

}  // namespace

int main()
{
    testText();
    testRule();
    testSurvey();
    testRepresentatives();
    testChoices();
    std::cout << "retheme tests passed\n";
    return 0;
}
