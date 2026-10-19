#include "Retheme.h"

#include <algorithm>
#include <stdexcept>

namespace aiassist {

namespace {

bool inAny(const std::map<uint32_t, std::vector<Area>>& areas, uint32_t frame, int x, int y)
{
    const auto found = areas.find(frame);
    if (found == areas.end()) {
        return false;
    }
    return std::any_of(found->second.begin(), found->second.end(),
                       [x, y](const Area& area) { return area.contains(x, y); });
}

uint32_t number(const nlohmann::json& value, const std::string& what)
{
    if (!value.is_number_integer() || value.get<long long>() < 0) {
        throw std::invalid_argument("'" + what + "' must be a whole number from 0.");
    }
    return value.get<uint32_t>();
}

// The frames an entry names: its frames and those of its section.
std::vector<uint32_t> entryFrames(const nlohmann::json& entry, const Snapshot& snapshot)
{
    std::vector<uint32_t> frames;
    const uint32_t frameCount = snapshot.project.frameCount;
    for (const nlohmann::json& value : entry.value("frames", nlohmann::json::array())) {
        const uint32_t frame = number(value, "frames");
        if (frame >= frameCount) {
            throw std::invalid_argument("Frame " + std::to_string(frame) + " does not exist.");
        }
        frames.push_back(frame);
    }
    if (entry.contains("section")) {
        const std::string name = entry["section"].get<std::string>();
        const auto section = std::find_if(snapshot.sections.begin(), snapshot.sections.end(),
                                          [&name](const Section& s) { return s.name == name; });
        if (section == snapshot.sections.end()) {
            throw std::invalid_argument("There is no section '" + name + "'.");
        }
        const uint32_t index = static_cast<uint32_t>(section - snapshot.sections.begin());
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            if (snapshot.frames[frame].section == index) {
                frames.push_back(frame);
            }
        }
    }
    if (frames.empty()) {
        throw std::invalid_argument("An area needs 'frames' or a 'section'.");
    }
    return frames;
}

void parseAreas(const nlohmann::json& arguments,
                const char* key,
                const Snapshot& snapshot,
                std::map<uint32_t, std::vector<Area>>& into)
{
    if (!arguments.contains(key)) {
        return;
    }
    if (!arguments[key].is_array()) {
        throw std::invalid_argument(std::string("'") + key + "' must be a list.");
    }
    for (const nlohmann::json& entry : arguments[key]) {
        Area area{static_cast<int>(number(entry.value("x", nlohmann::json()), "x")),
                  static_cast<int>(number(entry.value("y", nlohmann::json()), "y")),
                  static_cast<int>(number(entry.value("width", nlohmann::json()), "width")),
                  static_cast<int>(number(entry.value("height", nlohmann::json()), "height"))};
        for (uint32_t frame : entryFrames(entry, snapshot)) {
            into[frame].push_back(area);
        }
    }
}

}  // namespace

FrameStrip stripFrame(const RethemeFrame& frame, uint32_t frameIndex, const RethemeChoices& choices)
{
    const Plane<uint8_t>& original = *frame.original;
    FrameStrip strip;
    strip.keep.assign(original.pixels.size(), 0);
    for (uint32_t y = 0; y < original.height; ++y) {
        for (uint32_t x = 0; x < original.width; ++x) {
            if (inAny(choices.keepAreas, frameIndex, static_cast<int>(x), static_cast<int>(y))) {
                strip.keep[y * original.width + x] = 1;
            }
        }
    }
    if (!frame.zones || frame.zones->empty() || original.empty()) {
        return strip;
    }
    const Plane<uint8_t>& zones = *frame.zones;
    const Plane<uint8_t>* mask = frame.comparisonMask;
    const bool maskFits =
        mask && mask->width == original.width && mask->height == original.height;
    Plane<uint8_t> after = zones;
    bool changed = false;
    for (uint32_t zy = 0; zy < zones.height; ++zy) {
        for (uint32_t zx = 0; zx < zones.width; ++zx) {
            if (zones.at(zx, zy) == 255) {
                continue;
            }
            ++strip.dynamicPixels;
            const uint32_t ox = zx * original.width / zones.width;
            const uint32_t oy = zy * original.height / zones.height;
            const int px = static_cast<int>(ox);
            const int py = static_cast<int>(oy);
            const bool varies = maskFits && mask->at(ox, oy) != 0;
            const bool kept = (varies || inAny(choices.keepDynamic, frameIndex, px, py)) &&
                              !inAny(choices.dropDynamic, frameIndex, px, py);
            if (kept) {
                ++strip.keptDynamicPixels;
                strip.keep[oy * original.width + ox] = 1;
            } else {
                after.pixels[zy * zones.width + zx] = 255;
                changed = true;
            }
        }
    }
    if (changed) {
        strip.zones = std::move(after);
    }
    return strip;
}

RethemeFrame rethemeFrame(const Snapshot& snapshot, uint32_t frame)
{
    RethemeFrame result;
    result.original = &snapshot.originals[frame];
    const uint32_t mask = snapshot.frames[frame].comparisonMask;
    if (mask != CDMDPI_NONE && mask < snapshot.comparisonMasks.size()) {
        result.comparisonMask = &snapshot.comparisonMasks[mask];
    }
    if (frame < snapshot.dynamicMasks.size() && !snapshot.dynamicMasks[frame].empty()) {
        result.zones = &snapshot.dynamicMasks[frame];
    }
    return result;
}

std::vector<SectionSurvey> survey(const Snapshot& snapshot, const RethemeChoices& choices)
{
    std::vector<SectionSurvey> sections(snapshot.sections.size() + 1);
    sections[0].name = "(no section)";
    for (std::size_t index = 0; index < snapshot.sections.size(); ++index) {
        sections[index + 1].name = snapshot.sections[index].name;
        sections[index + 1].firstFrame = snapshot.sections[index].firstFrame;
    }
    for (uint32_t frame = 0; frame < snapshot.project.frameCount; ++frame) {
        const ColorizingDMDFrameInfo& info = snapshot.frames[frame];
        SectionSurvey& section =
            sections[info.section < snapshot.sections.size() ? info.section + 1 : 0];
        ++section.frames;
        if (info.background != CDMDPI_NONE) {
            ++section.framesWithBackground;
        }
        if (!info.hasDynamicPixels) {
            continue;
        }
        ++section.framesWithZones;
        if (choices.wholeFrames.count(frame)) {
            continue;
        }
        const RethemeFrame data = rethemeFrame(snapshot, frame);
        const FrameStrip strip = stripFrame(data, frame, choices);
        section.dynamicPixels += strip.dynamicPixels;
        section.keptDynamicPixels += strip.keptDynamicPixels;
        const bool corrected = choices.keepDynamic.count(frame) || choices.dropDynamic.count(frame);
        if (!corrected && strip.dynamicPixels > 0 &&
            (!data.comparisonMask || strip.keptDynamicPixels * 2 < strip.dynamicPixels)) {
            section.doubtful.push_back(frame);
        }
    }
    sections.erase(std::remove_if(sections.begin(), sections.end(),
                                  [](const SectionSurvey& s) { return s.frames == 0; }),
                   sections.end());
    return sections;
}

RethemeChoices parseChoices(const nlohmann::json& arguments, const Snapshot& snapshot)
{
    RethemeChoices choices;
    for (const nlohmann::json& value : arguments.value("keep_frames", nlohmann::json::array())) {
        const uint32_t frame = number(value, "keep_frames");
        if (frame >= snapshot.project.frameCount) {
            throw std::invalid_argument("Frame " + std::to_string(frame) + " does not exist.");
        }
        choices.wholeFrames.insert(frame);
    }
    parseAreas(arguments, "keep_areas", snapshot, choices.keepAreas);
    parseAreas(arguments, "keep_dynamic", snapshot, choices.keepDynamic);
    parseAreas(arguments, "drop_dynamic", snapshot, choices.dropDynamic);
    return choices;
}

}  // namespace aiassist
