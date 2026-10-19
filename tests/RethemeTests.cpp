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

// 16x8 frames. Frame 0: a score in zone 3 at x 8..15 under the mask, and a label in zone 1 at
// x 0..3 that is not. Frame 1: zones and no mask. Frame 2: nothing dynamic, a background.
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
        snapshot.originals.push_back(plane(16, 8, 2));
    }
    snapshot.comparisonMasks.push_back(plane(16, 8, 0));
    fill(snapshot.comparisonMasks[0], 8, 0, 8, 8, 1);
    snapshot.frames[0].comparisonMask = 0;
    snapshot.frames[0].hasDynamicPixels = 1;
    snapshot.frames[1].hasDynamicPixels = 1;
    snapshot.frames[2].background = 4;
    snapshot.dynamicMasks.resize(3);
    snapshot.dynamicMasks[0] = plane(16, 8, 255);
    fill(snapshot.dynamicMasks[0], 8, 2, 8, 4, 3);
    fill(snapshot.dynamicMasks[0], 0, 2, 4, 4, 1);
    snapshot.dynamicMasks[1] = plane(16, 8, 255);
    fill(snapshot.dynamicMasks[1], 0, 0, 4, 2, 0);
    return snapshot;
}

void testRule()
{
    const Snapshot snapshot = makeSnapshot();
    const RethemeChoices none;
    const FrameStrip strip = stripFrame(rethemeFrame(snapshot, 0), 0, none);
    expect(strip.dynamicPixels == 48 && strip.keptDynamicPixels == 32,
           "the zone under the mask is kept, the other is not");
    expect(strip.keep[3 * 16 + 10] == 1 && strip.keep[3 * 16 + 1] == 0 && strip.keep[0] == 0,
           "the kept zone's pixels keep their colors, the rest does not");
    expect(strip.zones && strip.zones->at(1, 3) == 255 && strip.zones->at(10, 3) == 3,
           "the zone that is not kept goes");

    RethemeChoices choices;
    choices.keepDynamic[0].push_back(Area{0, 0, 4, 8});
    choices.dropDynamic[0].push_back(Area{14, 0, 2, 8});
    choices.keepAreas[0].push_back(Area{0, 7, 2, 1});
    const FrameStrip corrected = stripFrame(rethemeFrame(snapshot, 0), 0, choices);
    expect(corrected.keptDynamicPixels == 16 + 24 && corrected.zones &&
               corrected.zones->at(1, 3) == 1 && corrected.zones->at(15, 3) == 255,
           "keep_dynamic and drop_dynamic correct the rule");
    expect(corrected.keep[7 * 16 + 1] == 1 && corrected.keep[7 * 16 + 2] == 0,
           "keep_areas keep static pixels");

    const FrameStrip plain = stripFrame(rethemeFrame(snapshot, 2), 2, none);
    expect(!plain.zones && plain.dynamicPixels == 0, "a frame without zones");

    RethemeChoices whole;
    whole.wholeFrames.insert(0);
    const FrameStrip kept = stripFrame(rethemeFrame(snapshot, 0), 0, whole);
    expect(!kept.zones && kept.keptDynamicPixels == 48 &&
               std::all_of(kept.keep.begin(), kept.keep.end(), [](uint8_t k) { return k == 1; }),
           "a frame in keep_frames keeps its colors and its dynamic zones");
}

void testRepresentatives()
{
    Snapshot snapshot = makeSnapshot();
    // Frame 1 differs from frame 0 in 2 of 128 pixels, frame 2 in a quarter.
    fill(snapshot.originals[1], 0, 0, 2, 1, 3);
    fill(snapshot.originals[2], 0, 0, 16, 2, 0);
    expect(representativeFrames(snapshot, {0, 1, 2}, 6) == std::vector<uint32_t>{0, 2},
           "frames that look alike are shown once");
    expect(representativeFrames(snapshot, {0, 1, 2}, 1) == std::vector<uint32_t>{0},
           "at most the limit");
    expect(survey(snapshot, RethemeChoices{})[0].representative == std::vector<uint32_t>{0},
           "the survey names a section's representative frames");
}

void testSurvey()
{
    const Snapshot snapshot = makeSnapshot();
    const std::vector<SectionSurvey> sections = survey(snapshot, RethemeChoices{});
    expect(sections.size() == 2 && sections[0].name == "Score" && sections[1].name == "Logo",
           "the sections with frames, in order");
    expect(sections[0].frames == 2 && sections[0].framesWithZones == 2 &&
               sections[0].dynamicPixels == 48 + 8 && sections[0].keptDynamicPixels == 32,
           "what the rule keeps of a section");
    expect(sections[0].doubtful == std::vector<uint32_t>{1}, "zones without a mask are doubtful");
    expect(sections[1].framesWithBackground == 1, "backgrounds are counted");

    RethemeChoices choices;
    choices.wholeFrames.insert(1);
    expect(survey(snapshot, choices)[0].doubtful.empty(), "a frame kept whole is not doubtful");
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
    testRule();
    testSurvey();
    testRepresentatives();
    testChoices();
    std::cout << "retheme tests passed\n";
    return 0;
}
