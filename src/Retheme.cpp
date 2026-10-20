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
    if (original.empty()) {
        return strip;
    }
    const uint32_t width = original.width;
    const bool whole = choices.wholeFrames.count(frameIndex) != 0;
    const Plane<uint8_t>* mask = frame.comparisonMask;
    const bool maskFits =
        mask && mask->width == original.width && mask->height == original.height;
    const Plane<uint8_t>* zones = frame.zones && !frame.zones->empty() ? frame.zones : nullptr;
    const auto at = [width](uint32_t index) {
        return std::pair<int, int>{static_cast<int>(index % width), static_cast<int>(index / width)};
    };
    // The original pixel under a pixel of the dynamic plane.
    const auto originalIndex = [&](uint32_t zx, uint32_t zy) {
        return (zy * original.height / zones->height) * width + zx * original.width / zones->width;
    };
    std::vector<uint8_t> inZone(original.pixels.size(), 0);
    if (zones) {
        for (uint32_t zy = 0; zy < zones->height; ++zy) {
            for (uint32_t zx = 0; zx < zones->width; ++zx) {
                if (zones->at(zx, zy) != 255) {
                    inZone[originalIndex(zx, zy)] = 1;
                }
            }
        }
    }

    // Text lines are kept whole where any of them varies (under the comparison mask), with the
    // corrections; graphics never.
    const std::vector<TextLine> lines = findTextLines(original);
    std::vector<uint8_t> isText(original.pixels.size(), 0);
    std::vector<uint8_t> nearKeptText(original.pixels.size(), 0);
    std::vector<uint8_t> keepLine(lines.size(), 0);
    std::vector<uint8_t> dropLine(lines.size(), 0);
    std::vector<uint8_t> zonedLine(lines.size(), 0);
    for (std::size_t index = 0; index < lines.size(); ++index) {
        bool varies = false;
        bool corrected = false;
        for (uint32_t pixel : lines[index].pixels) {
            isText[pixel] = 1;
            const auto [x, y] = at(pixel);
            varies = varies || (maskFits && mask->pixels[pixel] != 0);
            corrected = corrected || inAny(choices.keepDynamic, frameIndex, x, y);
            dropLine[index] = dropLine[index] || inAny(choices.dropDynamic, frameIndex, x, y);
            zonedLine[index] = zonedLine[index] || inZone[pixel] != 0;
        }
        keepLine[index] = (whole || varies || corrected) && !dropLine[index];
    }
    // The lines right above and below a kept line are its labels (HITS FOR SUPER JETS between
    // two numbers), and theirs in turn.
    for (bool grew = true; grew;) {
        grew = false;
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (keepLine[index] || dropLine[index]) {
                continue;
            }
            const TextLine& line = lines[index];
            for (std::size_t other = 0; other < lines.size(); ++other) {
                const TextLine& kept = lines[other];
                if (!keepLine[other]) {
                    continue;
                }
                const int gap = std::max(line.y0 - kept.y1, kept.y0 - line.y1);
                const int shared = std::min(line.x1, kept.x1) - std::max(line.x0, kept.x0);
                const int narrower = std::min(line.x1 - line.x0, kept.x1 - kept.x0);
                if (gap >= 0 && gap <= 4 && shared * 3 >= narrower) {
                    keepLine[index] = 1;
                    grew = true;
                    break;
                }
            }
        }
    }
    for (uint8_t shade : original.pixels) {
        strip.litPixels += shade != 0 ? 1 : 0;
    }
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const TextLine& line = lines[index];
        const bool zoned = zonedLine[index] != 0;
        strip.textPixels += static_cast<uint32_t>(line.pixels.size());
        ++strip.textLines;
        if (keepLine[index]) {
            ++strip.keptTextLines;
            for (uint32_t index : line.pixels) {
                strip.keep[index] = 1;
            }
            for (int y = std::max(0, line.y0 - 1); y < std::min<int>(original.height, line.y1 + 1); ++y) {
                for (int x = std::max(0, line.x0 - 1); x < std::min<int>(width, line.x1 + 1); ++x) {
                    nearKeptText[static_cast<uint32_t>(y) * width + static_cast<uint32_t>(x)] = 1;
                }
            }
        } else if (zoned) {
            ++strip.zonedTextLinesDropped;
        }
    }
    for (uint32_t y = 0; y < original.height; ++y) {
        for (uint32_t x = 0; x < original.width; ++x) {
            if (inAny(choices.keepAreas, frameIndex, static_cast<int>(x), static_cast<int>(y))) {
                strip.keep[y * width + x] = 1;
            }
        }
    }
    if (!zones) {
        return strip;
    }

    // A dynamic zone (pixels of one set that touch) stays whole when it reaches kept text and
    // holds no graphics, so a zone sized for a score that grows stays as it is; with graphics in
    // it, only its part around the kept text stays.
    Plane<uint8_t> after = *zones;
    bool changed = false;
    std::vector<uint8_t> seen(zones->pixels.size(), 0);
    for (uint32_t start = 0; start < zones->pixels.size(); ++start) {
        const uint8_t set = zones->pixels[start];
        if (set == 255 || seen[start]) {
            continue;
        }
        std::vector<uint32_t> region;
        std::vector<uint32_t> stack{start};
        seen[start] = 1;
        bool reachesText = false;
        bool holdsGraphics = false;
        while (!stack.empty()) {
            const uint32_t index = stack.back();
            stack.pop_back();
            region.push_back(index);
            const uint32_t zx = index % zones->width;
            const uint32_t zy = index / zones->width;
            const uint32_t o = originalIndex(zx, zy);
            reachesText = reachesText || (strip.keep[o] != 0 && isText[o] != 0);
            holdsGraphics = holdsGraphics || (original.pixels[o] != 0 && !isText[o]);
            const uint32_t neighbours[4][2] = {{zx - 1, zy}, {zx + 1, zy}, {zx, zy - 1}, {zx, zy + 1}};
            for (const auto& n : neighbours) {
                if (n[0] >= zones->width || n[1] >= zones->height) {
                    continue;
                }
                const uint32_t next = n[1] * zones->width + n[0];
                if (!seen[next] && zones->pixels[next] == set) {
                    seen[next] = 1;
                    stack.push_back(next);
                }
            }
        }
        for (uint32_t index : region) {
            ++strip.dynamicPixels;
            const uint32_t o = originalIndex(index % zones->width, index / zones->width);
            const auto [x, y] = at(o);
            bool kept = reachesText && (!holdsGraphics || nearKeptText[o]);
            kept = (kept || inAny(choices.keepDynamic, frameIndex, x, y)) &&
                   !inAny(choices.dropDynamic, frameIndex, x, y);
            if (kept) {
                ++strip.keptDynamicPixels;
            } else {
                after.pixels[index] = 255;
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

std::vector<uint32_t> representativeFrames(const Snapshot& snapshot,
                                           const std::vector<uint32_t>& frames,
                                           std::size_t limit)
{
    std::vector<uint32_t> chosen;
    for (uint32_t frame : frames) {
        if (chosen.size() >= limit) {
            break;
        }
        const Plane<uint8_t>& original = snapshot.originals[frame];
        const bool differs = std::all_of(chosen.begin(), chosen.end(), [&](uint32_t other) {
            const Plane<uint8_t>& taken = snapshot.originals[other];
            if (taken.pixels.size() != original.pixels.size()) {
                return true;
            }
            std::size_t different = 0;
            for (std::size_t index = 0; index < original.pixels.size(); ++index) {
                different += original.pixels[index] != taken.pixels[index] ? 1 : 0;
            }
            return different * 6 >= original.pixels.size();
        });
        if (differs) {
            chosen.push_back(frame);
        }
    }
    return chosen;
}

std::vector<SectionSurvey> survey(const Snapshot& snapshot, const RethemeChoices& choices)
{
    std::vector<SectionSurvey> sections(snapshot.sections.size() + 1);
    sections[0].name = "(no section)";
    for (std::size_t index = 0; index < snapshot.sections.size(); ++index) {
        sections[index + 1].name = snapshot.sections[index].name;
        sections[index + 1].firstFrame = snapshot.sections[index].firstFrame;
    }
    std::vector<std::vector<uint32_t>> members(sections.size());
    std::vector<std::vector<uint32_t>> textMembers(sections.size());
    for (uint32_t frame = 0; frame < snapshot.project.frameCount; ++frame) {
        const ColorizingDMDFrameInfo& info = snapshot.frames[frame];
        const std::size_t sectionIndex =
            info.section < snapshot.sections.size() ? info.section + 1 : 0;
        SectionSurvey& section = sections[sectionIndex];
        members[sectionIndex].push_back(frame);
        ++section.frames;
        if (info.background != CDMDPI_NONE) {
            ++section.framesWithBackground;
        }
        const RethemeFrame data = rethemeFrame(snapshot, frame);
        const FrameStrip strip = stripFrame(data, frame, choices);
        if (!choices.wholeFrames.count(frame) && strip.keptTextLines == 0 && strip.textLines > 0 &&
            strip.textPixels * 10 >= strip.litPixels * 6) {
            ++section.textFrames;
            textMembers[sectionIndex].push_back(frame);
        }
        if (!info.hasDynamicPixels) {
            continue;
        }
        ++section.framesWithZones;
        if (choices.wholeFrames.count(frame)) {
            continue;
        }
        section.dynamicPixels += strip.dynamicPixels;
        section.keptDynamicPixels += strip.keptDynamicPixels;
        const bool corrected = choices.keepDynamic.count(frame) || choices.dropDynamic.count(frame);
        if (!corrected && strip.zonedTextLinesDropped > 0) {
            section.doubtful.push_back(frame);
        }
    }
    for (std::size_t index = 0; index < sections.size(); ++index) {
        sections[index].representative = representativeFrames(snapshot, members[index], 6);
        sections[index].textCandidates = representativeFrames(snapshot, textMembers[index], 4);
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

namespace aiassist {

namespace {

// A connected group of lit pixels (8-neighbours).
struct Blob {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    std::vector<uint32_t> pixels;

    [[nodiscard]] int width() const { return x1 - x0; }
    [[nodiscard]] int height() const { return y1 - y0; }
};

std::vector<Blob> findBlobs(const Plane<uint8_t>& original)
{
    const int width = static_cast<int>(original.width);
    const int height = static_cast<int>(original.height);
    std::vector<uint8_t> seen(original.pixels.size(), 0);
    std::vector<Blob> blobs;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint32_t start = static_cast<uint32_t>(y * width + x);
            if (seen[start] || original.pixels[start] == 0) {
                continue;
            }
            Blob blob{x, y, x + 1, y + 1, {}};
            std::vector<uint32_t> stack{start};
            seen[start] = 1;
            while (!stack.empty()) {
                const uint32_t index = stack.back();
                stack.pop_back();
                blob.pixels.push_back(index);
                const int px = static_cast<int>(index) % width;
                const int py = static_cast<int>(index) / width;
                blob.x0 = std::min(blob.x0, px);
                blob.y0 = std::min(blob.y0, py);
                blob.x1 = std::max(blob.x1, px + 1);
                blob.y1 = std::max(blob.y1, py + 1);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int qx = px + dx;
                        const int qy = py + dy;
                        if (qx < 0 || qy < 0 || qx >= width || qy >= height) {
                            continue;
                        }
                        const uint32_t next = static_cast<uint32_t>(qy * width + qx);
                        if (!seen[next] && original.pixels[next] != 0) {
                            seen[next] = 1;
                            stack.push_back(next);
                        }
                    }
                }
            }
            blobs.push_back(std::move(blob));
        }
    }
    return blobs;
}

// A letter or digit of a DMD font: at least 4 and at most 20 rows, not much wider than high, and
// not a solid block.
bool isGlyph(const Blob& blob)
{
    const int w = blob.width();
    const int h = blob.height();
    if (h < 4 || h > 20 || w > 16 || w > 2 * h + 2) {
        return false;
    }
    return blob.pixels.size() * 10 <= static_cast<std::size_t>(w * h) * 9 || w <= 2;
}

// Punctuation and small marks of a font (comma, dot, dash, the dots of a colon).
bool isMark(const Blob& blob)
{
    return blob.height() <= 3 && blob.width() <= 4;
}

int overlap(int a0, int a1, int b0, int b1)
{
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

}  // namespace

// Inverse text: a lit bar of a line's height with letters cut out of it (the unlit holes, which
// do not touch the bar's edge, are glyphs).
bool isInverseText(const Plane<uint8_t>& original, const Blob& bar)
{
    if (bar.height() < 5 || bar.height() > 22 || bar.width() < 12) {
        return false;
    }
    Plane<uint8_t> holes;
    holes.width = static_cast<uint32_t>(bar.width());
    holes.height = static_cast<uint32_t>(bar.height());
    holes.pixels.assign(holes.width * holes.height, 0);
    std::vector<uint8_t> inBar(original.pixels.size(), 0);
    for (uint32_t index : bar.pixels) {
        inBar[index] = 1;
    }
    for (int y = bar.y0; y < bar.y1; ++y) {
        for (int x = bar.x0; x < bar.x1; ++x) {
            const uint32_t index = static_cast<uint32_t>(y) * original.width + static_cast<uint32_t>(x);
            holes.pixels[static_cast<uint32_t>(y - bar.y0) * holes.width +
                         static_cast<uint32_t>(x - bar.x0)] = inBar[index] ? 0 : 1;
        }
    }
    int glyphs = 0;
    for (const Blob& hole : findBlobs(holes)) {
        const bool inside = hole.x0 > 0 && hole.y0 > 0 &&
                            hole.x1 < static_cast<int>(holes.width) &&
                            hole.y1 < static_cast<int>(holes.height);
        if (inside && hole.height() >= 3 && hole.width() <= 16 && hole.pixels.size() >= 3) {
            ++glyphs;
        }
    }
    return glyphs >= 2;
}

std::vector<TextLine> findTextLines(const Plane<uint8_t>& original)
{
    std::vector<Blob> blobs = findBlobs(original);
    std::vector<std::size_t> glyphs;
    for (std::size_t index = 0; index < blobs.size(); ++index) {
        if (isGlyph(blobs[index])) {
            glyphs.push_back(index);
        }
    }
    std::sort(glyphs.begin(), glyphs.end(),
              [&blobs](std::size_t a, std::size_t b) { return blobs[a].x0 < blobs[b].x0; });
    // Glyphs of one height side by side, with at most a word space between them, are one line.
    std::vector<int> line(blobs.size(), -1);
    std::vector<std::vector<std::size_t>> lines;
    for (std::size_t glyph : glyphs) {
        const Blob& blob = blobs[glyph];
        int joined = -1;
        for (std::size_t other = 0; other < lines.size() && joined < 0; ++other) {
            const Blob& last = blobs[lines[other].back()];
            const int h = std::max(blob.height(), last.height());
            const bool sameHeight = std::min(blob.height(), last.height()) * 3 >= h * 2;
            const bool sameRows = overlap(blob.y0, blob.y1, last.y0, last.y1) * 3 >= h * 2;
            const int gap = blob.x0 - last.x1;
            if (sameHeight && sameRows && gap <= std::max(4, h * 2 / 3)) {
                joined = static_cast<int>(other);
            }
        }
        if (joined < 0) {
            joined = static_cast<int>(lines.size());
            lines.emplace_back();
        }
        lines[static_cast<std::size_t>(joined)].push_back(glyph);
        line[glyph] = joined;
    }
    std::vector<TextLine> result;
    for (const std::vector<std::size_t>& members : lines) {
        // A lone glyph is text only when it is tall enough to be a digit of a score.
        if (members.size() < 2 && blobs[members.front()].height() < 5) {
            continue;
        }
        TextLine text{blobs[members.front()].x0, blobs[members.front()].y0,
                      blobs[members.front()].x1, blobs[members.front()].y1, {}};
        for (std::size_t member : members) {
            const Blob& blob = blobs[member];
            text.x0 = std::min(text.x0, blob.x0);
            text.y0 = std::min(text.y0, blob.y0);
            text.x1 = std::max(text.x1, blob.x1);
            text.y1 = std::max(text.y1, blob.y1);
            text.pixels.insert(text.pixels.end(), blob.pixels.begin(), blob.pixels.end());
        }
        result.push_back(std::move(text));
    }
    // Marks in a line's rows: inside it (commas between digits), and a few beside it (dots after
    // a word, an equals sign between two lines). A field of dots beside a line is a dither of
    // graphics, not punctuation: then none of them.
    std::vector<uint8_t> taken(blobs.size(), 0);
    for (TextLine& text : result) {
        const int reach = std::max(4, text.y1 - text.y0);
        const int glyphX0 = text.x0;
        const int glyphX1 = text.x1;
        std::vector<std::size_t> inside;
        std::vector<std::size_t> beside;
        for (std::size_t index = 0; index < blobs.size(); ++index) {
            const Blob& blob = blobs[index];
            if (taken[index] || !isMark(blob) || blob.y0 < text.y0 || blob.y1 > text.y1 + 2 ||
                blob.x1 < glyphX0 - 3 * reach || blob.x0 > glyphX1 + 3 * reach) {
                continue;
            }
            (blob.x0 >= glyphX0 && blob.x1 <= glyphX1 ? inside : beside).push_back(index);
        }
        if (beside.size() > 6) {
            beside.clear();
        }
        for (std::size_t index : inside) {
            beside.push_back(index);
        }
        for (std::size_t index : beside) {
            const Blob& blob = blobs[index];
            taken[index] = 1;
            text.pixels.insert(text.pixels.end(), blob.pixels.begin(), blob.pixels.end());
            text.x0 = std::min(text.x0, blob.x0);
            text.x1 = std::max(text.x1, blob.x1);
            text.y1 = std::max(text.y1, blob.y1);
        }
    }
    for (const Blob& blob : blobs) {
        if (!isGlyph(blob) && !isMark(blob) && isInverseText(original, blob)) {
            result.push_back(TextLine{blob.x0, blob.y0, blob.x1, blob.y1, blob.pixels});
        }
    }
    return result;
}

}  // namespace aiassist
