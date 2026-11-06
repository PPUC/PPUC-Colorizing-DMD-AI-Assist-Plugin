#include "Tools.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "Identify.h"
#include "Png.h"
#include "Retheme.h"

namespace aiassist {

using nlohmann::json;

namespace {

// A refusal the assistant can act on.
struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

json textContent(const std::string& text)
{
    return json{{"type", "text"}, {"text", text}};
}

json imageContent(const Picture& picture)
{
    return json{{"type", "image"}, {"data", base64(encodePng(picture))}, {"mimeType", "image/png"}};
}

ColorizingDMDProjectInfo requireProject(const Editor& editor)
{
    const std::optional<ColorizingDMDProjectInfo> project = editor.project();
    if (!project) {
        throw ToolError("No project is open in the editor. Ask the author to open one.");
    }
    return *project;
}

uint32_t requireFrame(const ColorizingDMDProjectInfo& project, const json& arguments)
{
    if (!arguments.contains("frame") || !arguments["frame"].is_number_integer()) {
        throw ToolError("'frame' (a frame index) is required.");
    }
    const long long frame = arguments["frame"].get<long long>();
    if (frame < 0 || frame >= static_cast<long long>(project.frameCount)) {
        throw ToolError("Frame " + std::to_string(frame) + " does not exist; the project has " +
                        std::to_string(project.frameCount) + " frames (0 .. " +
                        std::to_string(project.frameCount - 1) + ").");
    }
    return static_cast<uint32_t>(frame);
}

std::string idText(uint32_t value)
{
    return value == CDMDPI_NONE ? std::string("-") : std::to_string(value);
}

// ---- Pictures --------------------------------------------------------------------------

std::array<uint8_t, 3> fromRgb565(uint16_t color)
{
    const uint8_t r = static_cast<uint8_t>(((color >> 11) & 0x1f) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((color >> 5) & 0x3f) * 255 / 63);
    const uint8_t b = static_cast<uint8_t>((color & 0x1f) * 255 / 31);
    return {r, g, b};
}

uint8_t shadeGray(uint8_t shade, uint32_t colorCount)
{
    const uint32_t maxShade = std::max<uint32_t>(1, colorCount - 1);
    return static_cast<uint8_t>(std::min<uint32_t>(shade, maxShade) * 255 / maxShade);
}

// Distinct colors for the dynamic sets, as labels (not the sets' own colors).
std::array<uint8_t, 3> setColor(uint8_t set)
{
    static const std::array<std::array<uint8_t, 3>, 8> colors{{{255, 80, 80},
                                                               {80, 200, 255},
                                                               {255, 220, 0},
                                                               {120, 255, 120},
                                                               {255, 120, 255},
                                                               {255, 160, 60},
                                                               {160, 120, 255},
                                                               {60, 255, 220}}};
    return colors[set % colors.size()];
}

// Each pixel as a scale x scale block, with a dark line between pixels from scale 3 on, so
// that the model can count pixels.
Picture scaled(const Picture& picture, int scale)
{
    Picture out(picture.width * scale, picture.height * scale);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const std::size_t i =
                (static_cast<std::size_t>(y / scale) * picture.width + x / scale) * 3;
            const bool grid = scale >= 3 && (x % scale == scale - 1 || y % scale == scale - 1);
            const uint8_t dim = grid ? 2 : 1;
            out.set(x, y, picture.rgb[i] / dim, picture.rgb[i + 1] / dim, picture.rgb[i + 2] / dim);
        }
    }
    return out;
}

Picture originalPicture(const Plane<uint8_t>& original, uint32_t colorCount)
{
    Picture picture(static_cast<int>(original.width), static_cast<int>(original.height));
    for (uint32_t y = 0; y < original.height; ++y) {
        for (uint32_t x = 0; x < original.width; ++x) {
            const uint8_t gray = shadeGray(original.at(x, y), colorCount);
            picture.set(static_cast<int>(x), static_cast<int>(y), gray, gray, gray);
        }
    }
    return picture;
}

Picture colorizedPicture(const Plane<uint16_t>& colorized)
{
    Picture picture(static_cast<int>(colorized.width), static_cast<int>(colorized.height));
    for (uint32_t y = 0; y < colorized.height; ++y) {
        for (uint32_t x = 0; x < colorized.width; ++x) {
            const auto [r, g, b] = fromRgb565(colorized.at(x, y));
            picture.set(static_cast<int>(x), static_cast<int>(y), r, g, b);
        }
    }
    return picture;
}

// The original in gray, the pixels the comparison ignores tinted magenta (as the editor).
Picture maskPicture(const Plane<uint8_t>& original, const Plane<uint8_t>& mask, uint32_t colorCount)
{
    Picture picture = originalPicture(original, colorCount);
    if (mask.width != original.width || mask.height != original.height) {
        return picture;
    }
    for (uint32_t y = 0; y < mask.height; ++y) {
        for (uint32_t x = 0; x < mask.width; ++x) {
            if (mask.at(x, y)) {
                const uint8_t gray = shadeGray(original.at(x, y), colorCount);
                picture.set(static_cast<int>(x), static_cast<int>(y),
                            static_cast<uint8_t>((255 * 2 + gray) / 3),
                            static_cast<uint8_t>(gray / 3),
                            static_cast<uint8_t>((255 * 2 + gray) / 3));
            }
        }
    }
    return picture;
}

// The dynamic plane's zones over the original in gray: each set in its label color, mixed with
// the shade so the content stays visible.
Picture dynamicPicture(const Plane<uint8_t>& original, const Plane<uint8_t>& zones,
                       uint32_t colorCount)
{
    Picture picture(static_cast<int>(zones.width), static_cast<int>(zones.height));
    const uint32_t scaleX = original.width ? std::max<uint32_t>(1, zones.width / original.width) : 1;
    const uint32_t scaleY =
        original.height ? std::max<uint32_t>(1, zones.height / original.height) : 1;
    for (uint32_t y = 0; y < zones.height; ++y) {
        for (uint32_t x = 0; x < zones.width; ++x) {
            const uint32_t ox = std::min(x / scaleX, original.width - 1);
            const uint32_t oy = std::min(y / scaleY, original.height - 1);
            const uint8_t gray = original.empty() ? 0 : shadeGray(original.at(ox, oy), colorCount);
            const uint8_t set = zones.at(x, y);
            if (set == 255) {
                picture.set(static_cast<int>(x), static_cast<int>(y), gray / 2, gray / 2, gray / 2);
            } else {
                const auto [r, g, b] = setColor(set);
                const int light = 64 + gray * 3 / 4;
                picture.set(static_cast<int>(x), static_cast<int>(y),
                            static_cast<uint8_t>(r * light / 255),
                            static_cast<uint8_t>(g * light / 255),
                            static_cast<uint8_t>(b * light / 255));
            }
        }
    }
    return picture;
}

// ---- Tools -------------------------------------------------------------------------------

json projectSummary(const Editor& editor)
{
    const ColorizingDMDProjectInfo project = requireProject(editor);
    json sections = json::array();
    for (const Section& section : editor.sections()) {
        sections.push_back({{"name", section.name}, {"firstFrame", section.firstFrame}});
    }
    json summary{
        {"rom", project.romName},
        {"frames", project.frameCount},
        {"shades", project.colorCount},
        {"originalSize", std::to_string(project.originalWidth) + "x" +
                             std::to_string(project.originalHeight)},
        {"sdPlane", std::to_string(project.sdWidth) + "x" + std::to_string(project.sdHeight)},
        {"hdPlane", project.hdWidth ? std::to_string(project.hdWidth) + "x" +
                                          std::to_string(project.hdHeight)
                                    : std::string("none")},
        {"dynamicZonesOn", project.dynamicPlaneIsHd ? "hd" : "sd"},
        {"comparisonMasks", project.comparisonMaskCount},
        {"sprites", project.spriteCount},
        {"backgrounds", project.backgroundCount},
        {"palettes", project.paletteCount},
        {"sections", sections},
        {"revision", project.revision},
    };
    if (const std::optional<uint32_t> current = editor.currentFrame()) {
        summary["currentFrame"] = *current;
    }
    summary["selectedFrames"] = editor.selectedFrames();
    return json{{"content", json::array({textContent(summary.dump(2))})}};
}

json listFrames(const Editor& editor, const json& arguments)
{
    const ColorizingDMDProjectInfo project = requireProject(editor);
    const long long start = arguments.value("start", 0LL);
    const long long count = std::clamp(arguments.value("count", 100LL), 1LL, 1000LL);
    if (start < 0 || start >= static_cast<long long>(project.frameCount)) {
        throw ToolError("'start' must be 0 .. " + std::to_string(project.frameCount - 1) + ".");
    }
    const std::vector<Section> sections = editor.sections();
    std::ostringstream text;
    text << "frame | ms | mask | shape | background | sprites | dynamic | hd | trigger | section\n";
    const uint32_t end =
        static_cast<uint32_t>(std::min<long long>(start + count, project.frameCount));
    for (uint32_t index = static_cast<uint32_t>(start); index < end; ++index) {
        const std::optional<ColorizingDMDFrameInfo> frame = editor.frame(index);
        if (!frame) {
            continue;
        }
        const std::string section = frame->section < sections.size()
                                        ? sections[frame->section].name
                                        : std::string("-");
        text << index << " | " << frame->durationMs << " | " << idText(frame->comparisonMask)
             << " | " << (frame->shapeMode ? "yes" : "no") << " | " << idText(frame->background)
             << " | " << frame->spriteCount << " | " << (frame->hasDynamicPixels ? "yes" : "no")
             << " | " << (frame->hasHd ? "yes" : "no") << " | " << idText(frame->triggerId)
             << " | " << section << '\n';
    }
    if (end < project.frameCount) {
        text << "(" << project.frameCount - end << " more frames from " << end << ")\n";
    }
    return json{{"content", json::array({textContent(text.str())})}};
}

json frameImage(const Editor& editor, const json& arguments)
{
    const ColorizingDMDProjectInfo project = requireProject(editor);
    const uint32_t frame = requireFrame(project, arguments);
    const std::string view = arguments.value("view", std::string("original_and_colorized"));
    const bool hd = arguments.value("plane", std::string("sd")) == "hd";
    const int scale = static_cast<int>(std::clamp(arguments.value("scale", 4LL), 1LL, 8LL));
    if (hd && !project.hdWidth) {
        throw ToolError("The project has no HD plane.");
    }
    const ColorizingDMDFrameInfo info = *editor.frame(frame);
    const Plane<uint8_t> original = editor.original(frame);

    std::vector<std::pair<std::string, Picture>> pictures;
    const auto addColorized = [&]() {
        const Plane<uint16_t> colorized = editor.colorized(frame, hd);
        if (colorized.empty()) {
            throw ToolError("Frame " + std::to_string(frame) + " has no " + (hd ? "HD" : "SD") +
                            " picture.");
        }
        pictures.emplace_back(hd ? "colorized (HD)" : "colorized", colorizedPicture(colorized));
    };
    if (view == "original" || view == "original_and_colorized") {
        pictures.emplace_back("original", originalPicture(original, project.colorCount));
    }
    if (view == "colorized" || view == "original_and_colorized") {
        addColorized();
    }
    if (view == "comparison_mask") {
        if (info.comparisonMask == CDMDPI_NONE) {
            throw ToolError("Frame " + std::to_string(frame) + " has no comparison mask.");
        }
        pictures.emplace_back("comparison mask " + std::to_string(info.comparisonMask) +
                                  " (magenta: ignored) over the original",
                              maskPicture(original, editor.comparisonMask(info.comparisonMask),
                                          project.colorCount));
    }
    if (view == "dynamic") {
        pictures.emplace_back("dynamic zones (a color per set) over the original",
                              dynamicPicture(original, editor.dynamicMask(frame),
                                             project.colorCount));
    }
    if (pictures.empty()) {
        throw ToolError("Unknown view '" + view +
                        "'; use original, colorized, original_and_colorized, comparison_mask or "
                        "dynamic.");
    }
    json content = json::array();
    std::string caption = "Frame " + std::to_string(frame) + ":";
    for (const auto& [name, picture] : pictures) {
        caption += " " + name + " (" + std::to_string(picture.width) + "x" +
                   std::to_string(picture.height) + " pixels, shown " + std::to_string(scale) +
                   "x)";
        caption += &picture == &pictures.back().second ? "." : ";";
    }
    content.push_back(textContent(caption));
    for (const auto& [name, picture] : pictures) {
        content.push_back(imageContent(scaled(picture, scale)));
    }
    return json{{"content", content}};
}

json selection(const Editor& editor)
{
    requireProject(editor);
    json result{{"selectedFrames", editor.selectedFrames()}};
    if (const std::optional<uint32_t> current = editor.currentFrame()) {
        result["currentFrame"] = *current;
    }
    return json{{"content", json::array({textContent(result.dump())})}};
}

json showFrames(const Editor& editor, const json& arguments)
{
    const ColorizingDMDProjectInfo project = requireProject(editor);
    const uint32_t frame = requireFrame(project, arguments);
    std::vector<uint32_t> frames;
    for (const json& value : arguments.value("select", json::array())) {
        if (!value.is_number_integer() || value.get<long long>() < 0 ||
            value.get<long long>() >= static_cast<long long>(project.frameCount)) {
            throw ToolError("'select' must list existing frame indexes.");
        }
        frames.push_back(value.get<uint32_t>());
    }
    if (!editor.showFrames(frame, frames)) {
        throw ToolError("The editor did not show the frames.");
    }
    return json{{"content", json::array({textContent("Shown in the editor.")})}};
}

// ---- Retheming template ------------------------------------------------------------------

Snapshot requireSnapshot(const Editor& editor, bool withDynamicMasks)
{
    std::optional<Snapshot> snapshot = editor.snapshot(withDynamicMasks);
    if (!snapshot) {
        throw ToolError("No project is open in the editor. Ask the author to open one.");
    }
    return std::move(*snapshot);
}

RethemeChoices choicesFrom(const json& arguments, const Snapshot& snapshot)
{
    try {
        return parseChoices(arguments, snapshot);
    } catch (const std::invalid_argument& error) {
        throw ToolError(error.what());
    }
}

std::string percent(uint64_t part, uint64_t whole)
{
    return whole ? std::to_string((part * 100 + whole / 2) / whole) + "%" : std::string("-");
}

json rethemeSurvey(const Editor& editor, const json& arguments)
{
    const Snapshot snapshot = requireSnapshot(editor, true);
    const RethemeChoices choices = choicesFrom(arguments, snapshot);
    const std::string only = arguments.value("section", std::string());
    std::ostringstream text;
    text << "Rule: the originals are read as text lines and graphics. A text line where any of it "
            "lies under the frame's comparison mask (varying content) stays whole with its colors "
            "and the dynamic zones that reach it, and so do the lines right above and below it "
            "(its labels); everything else is blacked out, graphics always. Frames in "
            "keep_frames keep all their text. All backgrounds and color rotations go.\n\n"
            "section | first frame | frames | with dynamic zones | with background | dynamic "
            "pixels kept | doubtful frames | text frames nothing is kept of (candidates) | "
            "representative frames\n";
    uint64_t dynamic = 0;
    uint64_t kept = 0;
    std::size_t doubtful = 0;
    for (const SectionSurvey& section : survey(snapshot, choices)) {
        if (!only.empty() && section.name != only) {
            continue;
        }
        dynamic += section.dynamicPixels;
        kept += section.keptDynamicPixels;
        doubtful += section.doubtful.size();
        text << section.name << " | " << section.firstFrame << " | " << section.frames << " | "
             << section.framesWithZones << " | " << section.framesWithBackground << " | "
             << percent(section.keptDynamicPixels, section.dynamicPixels) << " | ";
        if (section.doubtful.empty()) {
            text << "-";
        } else {
            text << section.doubtful.size() << ":";
            for (std::size_t index = 0; index < section.doubtful.size() && index < 12; ++index) {
                text << ' ' << section.doubtful[index];
            }
            if (section.doubtful.size() > 12) {
                text << " ...";
            }
        }
        text << " | " << section.textFrames;
        if (!section.textCandidates.empty()) {
            text << " (";
            for (std::size_t index = 0; index < section.textCandidates.size(); ++index) {
                text << (index ? " " : "") << section.textCandidates[index];
            }
            text << ")";
        }
        text << " |";
        for (uint32_t frame : section.representative) {
            text << ' ' << frame;
        }
        text << '\n';
    }
    text << "\nAll: " << percent(kept, dynamic) << " of the dynamic pixels kept, " << doubtful
         << " doubtful frames (dynamic zones without a comparison mask, or less than half of "
            "them under it). Look at doubtful frames with get_frame_image (views dynamic and "
            "comparison_mask) and retheme_preview before you propose corrections. To find the "
            "frames to keep (text any game has: BONUS 12X, MULTIBALL, SHOOT AGAIN, PLAYER 1, "
            "EXTRA BALL, JACKPOT, REPLAY, MATCH), read the candidates of all sections with "
            "get_contact_sheet (candidates: true); a candidate stands for the frames of its "
            "section that look alike, so propose those too (get_contact_sheet with the section).";
    if (!editor.canEdit()) {
        text << "\nThis editor's API is version " << editor.apiVersion()
             << ": it cannot apply the template (version 2 needed).";
    }
    return json{{"content", json::array({textContent(text.str())})}};
}

// The SD picture as the runtime colors it: the colorized pixels, and the dynamic zones in their
// sets' colors for the shade (when the zones are on the SD plane).
Picture rethemePicture(const Plane<uint16_t>& colorized,
                       const Plane<uint8_t>& original,
                       const std::vector<uint8_t>* keep,
                       uint32_t fill,
                       const std::vector<uint16_t>& plain,
                       const Plane<uint8_t>* zones,
                       const std::vector<uint16_t>& dynamicColors,
                       uint32_t shades)
{
    Picture picture(static_cast<int>(colorized.width), static_cast<int>(colorized.height));
    for (uint32_t y = 0; y < colorized.height; ++y) {
        for (uint32_t x = 0; x < colorized.width; ++x) {
            const uint32_t ox = x * original.width / colorized.width;
            const uint32_t oy = y * original.height / colorized.height;
            const uint8_t shade = original.at(ox, oy);
            uint16_t color = colorized.at(x, y);
            if (keep && !(*keep)[oy * original.width + ox]) {
                color = fill != CDMDPI_NONE ? static_cast<uint16_t>(fill)
                                            : (shade < plain.size() ? plain[shade] : uint16_t{0});
            }
            if (zones && zones->width == colorized.width && zones->height == colorized.height) {
                const uint8_t set = zones->at(x, y);
                const std::size_t index = static_cast<std::size_t>(set) * shades + shade;
                if (set != 255 && index < dynamicColors.size()) {
                    color = dynamicColors[index];
                }
            }
            const auto [r, g, b] = fromRgb565(color);
            picture.set(static_cast<int>(x), static_cast<int>(y), r, g, b);
        }
    }
    return picture;
}

// What the frames not kept get: black (default) or "plain" (the look of a frame fresh from a
// dump).
uint32_t fillFrom(const json& arguments)
{
    const std::string fill = arguments.value("fill", std::string("black"));
    if (fill == "black") {
        return 0;
    }
    if (fill == "plain") {
        return CDMDPI_NONE;
    }
    throw ToolError("'fill' is black or plain.");
}

json rethemePreview(const Editor& editor, const json& arguments)
{
    if (!editor.canEdit()) {
        throw ToolError("The editor's API is version " + std::to_string(editor.apiVersion()) +
                        "; the retheming template needs version 2.");
    }
    Snapshot snapshot = requireSnapshot(editor, false);
    const uint32_t frame = requireFrame(snapshot.project, arguments);
    snapshot.dynamicMasks.resize(snapshot.project.frameCount);
    if (snapshot.frames[frame].hasDynamicPixels) {
        snapshot.dynamicMasks[frame] = editor.dynamicMask(frame);
    }
    const RethemeChoices choices = choicesFrom(arguments, snapshot);
    const int scale = static_cast<int>(std::clamp(arguments.value("scale", 4LL), 1LL, 8LL));
    const Plane<uint16_t> colorized = editor.colorized(frame, false);
    const std::vector<uint16_t> plain = editor.plainColors();
    const std::vector<uint16_t> dynamicColors = editor.dynamicColors(frame);
    const Plane<uint8_t>& original = snapshot.originals[frame];
    const RethemeFrame data = rethemeFrame(snapshot, frame);
    const uint32_t shades = snapshot.project.colorCount;

    const uint32_t fill = fillFrom(arguments);
    const Picture now = rethemePicture(colorized, original, nullptr, fill, plain, data.zones,
                                       dynamicColors, shades);
    std::string caption = "Frame " + std::to_string(frame) + ": now, then in the template";
    const FrameStrip strip = stripFrame(data, frame, choices);
    const Plane<uint8_t>* zones = strip.zones ? &*strip.zones : data.zones;
    const Picture after = rethemePicture(colorized, original, &strip.keep, fill, plain, zones,
                                         dynamicColors, shades);
    caption += choices.wholeFrames.count(frame) ? std::string(" (kept: its colors stay")
                                                : " (" + std::to_string(strip.keptDynamicPixels) +
                                                      " of " + std::to_string(strip.dynamicPixels) +
                                                      " dynamic pixels kept";
    if (snapshot.frames[frame].background != CDMDPI_NONE) {
        caption += "; its background goes";
    }
    caption += ")";
    caption += ". SD plane, " + std::to_string(colorized.width) + "x" +
               std::to_string(colorized.height) + " pixels shown " + std::to_string(scale) +
               "x; the background is not drawn.";
    return json{{"content", json::array({textContent(caption), imageContent(scaled(now, scale)),
                                         imageContent(scaled(after, scale))})}};
}

json rethemeApply(const Editor& editor, const json& arguments)
{
    if (!editor.canEdit()) {
        throw ToolError("The editor's API is version " + std::to_string(editor.apiVersion()) +
                        "; the retheming template needs version 2.");
    }
    const Snapshot snapshot = requireSnapshot(editor, true);
    const RethemeChoices choices = choicesFrom(arguments, snapshot);
    const std::string name = arguments.value("name", std::string("Retheming template"));

    const uint32_t fill = fillFrom(arguments);
    uint32_t stripped = 0;
    uint64_t dynamic = 0;
    uint64_t kept = 0;
    uint32_t deletedBackgrounds = 0;
    std::string failure;
    const bool begun = editor.edit(name, [&](const EditSession& session) {
        for (uint32_t frame = 0; frame < snapshot.project.frameCount; ++frame) {
            const FrameStrip strip = stripFrame(rethemeFrame(snapshot, frame), frame, choices);
            dynamic += strip.dynamicPixels;
            kept += strip.keptDynamicPixels;
            bool ok = session.resetFramePixels(frame, strip.keep, fill);
            if (ok && strip.zones) {
                ok = session.setDynamicZones(frame, strip.zones->pixels);
            }
            if (ok && snapshot.frames[frame].background != CDMDPI_NONE) {
                ok = session.setFrameBackground(frame, CDMDPI_NONE);
            }
            ok = ok && session.clearColorRotations(frame);
            if (!ok) {
                failure = "The editor refused a change of frame " + std::to_string(frame) + ".";
                return false;
            }
            stripped += choices.wholeFrames.count(frame) ? 0 : 1;
        }
        for (uint32_t background = snapshot.project.backgroundCount; background-- > 0;) {
            if (!session.deleteBackground(background)) {
                failure = "The editor refused to delete background " +
                          std::to_string(background) + ".";
                return false;
            }
            ++deletedBackgrounds;
        }
        return true;
    });
    if (!begun) {
        throw ToolError("The editor did not start an edit (no project, or another edit is open).");
    }
    if (!failure.empty()) {
        throw ToolError(failure + " Nothing was changed.");
    }
    std::ostringstream text;
    text << "Applied as one step '" << name << "': " << stripped << " frames stripped, "
         << choices.wholeFrames.size() << " kept with their colors, " << kept << " of " << dynamic
         << " dynamic pixels kept, " << deletedBackgrounds
         << " backgrounds deleted. One Undo in the editor takes it all back; the author saves "
            "the template under a new name (File -> Save As).";
    return json{{"content", json::array({textContent(text.str())})}};
}

// The picture of a frame as the runtime colors it (dynamic zones in their sets' colors).
Picture runtimePicture(const Editor& editor, const Snapshot& snapshot, uint32_t frame,
                       const Plane<uint16_t>& colorized)
{
    Plane<uint8_t> zones;
    std::vector<uint16_t> dynamicColors;
    if (snapshot.frames[frame].hasDynamicPixels && !snapshot.project.dynamicPlaneIsHd) {
        zones = editor.dynamicMask(frame);
        dynamicColors = editor.dynamicColors(frame);
    }
    return rethemePicture(colorized, snapshot.originals[frame], nullptr, CDMDPI_NONE, {},
                          zones.empty() ? nullptr : &zones, dynamicColors,
                          snapshot.project.colorCount);
}

// Many frames in one picture, row by row, to look over a project quickly.
json contactSheet(const Editor& editor, const json& arguments)
{
    const Snapshot snapshot = requireSnapshot(editor, false);
    const uint32_t frameCount = snapshot.project.frameCount;
    std::vector<uint32_t> frames;
    std::string what;
    if (arguments.contains("frames")) {
        for (const json& value : arguments["frames"]) {
            if (!value.is_number_integer() || value.get<long long>() < 0 ||
                value.get<long long>() >= static_cast<long long>(frameCount)) {
                throw ToolError("'frames' must list existing frame indexes.");
            }
            frames.push_back(value.get<uint32_t>());
        }
        what = "the frames asked for";
    } else if (arguments.contains("section")) {
        const std::string name = arguments["section"].get<std::string>();
        const auto section = std::find_if(snapshot.sections.begin(), snapshot.sections.end(),
                                          [&name](const Section& s) { return s.name == name; });
        if (section == snapshot.sections.end()) {
            throw ToolError("There is no section '" + name + "'.");
        }
        const uint32_t index = static_cast<uint32_t>(section - snapshot.sections.begin());
        std::vector<uint32_t> members;
        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            if (snapshot.frames[frame].section == index) {
                members.push_back(frame);
            }
        }
        frames = arguments.value("all", false) ? members
                                               : representativeFrames(snapshot, members, 48);
        what = "section '" + name + "'" +
               (arguments.value("all", false) ? std::string() : std::string(", frames that differ"));
    } else {
        const bool candidates = arguments.value("candidates", false);
        // Which text is kept does not depend on the dynamic zones.
        for (const SectionSurvey& section : survey(snapshot, RethemeChoices{})) {
            const std::vector<uint32_t>& chosen =
                candidates ? section.textCandidates : section.representative;
            frames.insert(frames.end(), chosen.begin(), chosen.end());
        }
        what = candidates ? "the text frames nothing is kept of, candidates for keep_frames"
                          : "the representative frames of every section";
        if (frames.empty()) {
            throw ToolError("There are no such frames.");
        }
    }
    const std::size_t start =
        static_cast<std::size_t>(std::max<long long>(0, arguments.value("start", 0LL)));
    const std::size_t total = frames.size();
    constexpr std::size_t kMaxTiles = 48;
    if (start >= total) {
        throw ToolError("Nothing from 'start' " + std::to_string(start) + " on; there are " +
                        std::to_string(total) + " frames.");
    }
    frames = std::vector<uint32_t>(frames.begin() + static_cast<std::ptrdiff_t>(start),
                                   frames.begin() + static_cast<std::ptrdiff_t>(
                                                        std::min(total, start + kMaxTiles)));
    const bool colorized = arguments.value("view", std::string("original")) == "colorized";
    const int columns = static_cast<int>(std::clamp(arguments.value("columns", 4LL), 1LL, 8LL));
    const int scale = static_cast<int>(std::clamp(arguments.value("scale", 2LL), 1LL, 4LL));
    std::vector<Plane<uint16_t>> pictures;
    if (colorized) {
        pictures = editor.colorizedFrames(frames);
    }
    const int tileWidth = static_cast<int>(colorized ? pictures.front().width
                                                     : snapshot.project.originalWidth);
    const int tileHeight = static_cast<int>(colorized ? pictures.front().height
                                                      : snapshot.project.originalHeight);
    constexpr int kGap = 3;
    const int rows = static_cast<int>((frames.size() + columns - 1) / columns);
    Picture sheet(columns * (tileWidth + kGap) + kGap, rows * (tileHeight + kGap) + kGap);
    for (int y = 0; y < sheet.height; ++y) {
        for (int x = 0; x < sheet.width; ++x) {
            sheet.set(x, y, 0, 40, 110);
        }
    }
    std::ostringstream caption;
    caption << "Contact sheet of " << what << " (" << (colorized ? "colorized" : "originals")
            << "), " << columns << " per row, frames " << start + 1 << " to "
            << start + frames.size() << " of " << total << ". Row by row:";
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const uint32_t frame = frames[index];
        const Picture tile = colorized ? runtimePicture(editor, snapshot, frame, pictures[index])
                                       : originalPicture(snapshot.originals[frame],
                                                         snapshot.project.colorCount);
        const int left = kGap + static_cast<int>(index % columns) * (tileWidth + kGap);
        const int top = kGap + static_cast<int>(index / columns) * (tileHeight + kGap);
        for (int y = 0; y < std::min(tile.height, tileHeight); ++y) {
            for (int x = 0; x < std::min(tile.width, tileWidth); ++x) {
                const std::size_t i = (static_cast<std::size_t>(y) * tile.width + x) * 3;
                sheet.set(left + x, top + y, tile.rgb[i], tile.rgb[i + 1], tile.rgb[i + 2]);
            }
        }
        if (index % columns == 0) {
            caption << (index ? "; row " : " row ") << index / columns + 1 << ":";
        }
        caption << ' ' << frame;
        const uint32_t section = snapshot.frames[frame].section;
        if (section < snapshot.sections.size()) {
            caption << " (" << snapshot.sections[section].name << ")";
        }
    }
    caption << '.';
    if (start + frames.size() < total) {
        caption << " More with start " << start + frames.size() << ".";
    }
    return json{{"content", json::array({textContent(caption.str()),
                                         imageContent(scaled(sheet, scale))})}};
}

// ---- Identification -------------------------------------------------------------------------

std::string sectionOf(const Snapshot& snapshot, uint32_t frame)
{
    const uint32_t section = snapshot.frames[frame].section;
    return section < snapshot.sections.size() ? snapshot.sections[section].name : std::string("-");
}

std::string comparisonText(const Snapshot& snapshot, uint32_t frame)
{
    const ColorizingDMDFrameInfo& info = snapshot.frames[frame];
    return std::string("mask ") + idText(info.comparisonMask) + (info.shapeMode ? ", shape" : "");
}

// Frames as "12 (Section), 13, 14 (Other)": the section only where it changes.
std::string frameListText(const Snapshot& snapshot, const std::vector<uint32_t>& frames,
                          std::size_t limit)
{
    std::ostringstream text;
    std::string lastSection;
    for (std::size_t index = 0; index < frames.size() && index < limit; ++index) {
        const std::string section = sectionOf(snapshot, frames[index]);
        text << (index ? ", " : "") << frames[index];
        if (section != lastSection) {
            text << " (" << section << ")";
            lastSection = section;
        }
    }
    if (frames.size() > limit) {
        text << " and " << frames.size() - limit << " more";
    }
    return text.str();
}

json comparisonMasks(const Editor& editor)
{
    const Snapshot snapshot = requireSnapshot(editor, false);
    const uint32_t total = snapshot.project.originalWidth * snapshot.project.originalHeight;
    std::ostringstream text;
    text << "Comparison masks of the " << total << " original pixels. compared: the pixels the "
         << "comparison looks at; box: where they are (x, y, width, height); fewest lit: the lit "
         << "pixels among them of the frame using the mask that has the fewest, i.e. how little "
         << "content may identify a frame. Few compared or few lit pixels: the mask may catch "
         << "frames it should not (find_identification_conflicts).\n"
         << "mask | compared | box | fewest lit | frames\n";
    for (const MaskUse& use : maskUses(snapshot)) {
        text << use.mask << " | " << use.compared << " (" << percent(use.compared, total)
             << ") | ";
        if (use.compared) {
            text << use.x0 << "," << use.y0 << " " << use.x1 - use.x0 << "x" << use.y1 - use.y0;
        } else {
            text << "-";
        }
        text << " | " << (use.frames.empty() ? std::string("-")
                                              : std::to_string(use.fewestLitCompared))
             << " | " << (use.frames.empty() ? std::string("unused")
                                              : frameListText(snapshot, use.frames, 12))
             << '\n';
    }
    return json{{"content", json::array({textContent(text.str())})}};
}

json identificationConflicts(const Editor& editor, const json& arguments)
{
    const Snapshot snapshot = requireSnapshot(editor, false);
    std::set<uint32_t> only;
    for (const json& value : arguments.value("frames", json::array())) {
        if (!value.is_number_integer()) {
            throw ToolError("'frames' must list frame indexes.");
        }
        only.insert(value.get<uint32_t>());
    }
    const bool withSame = arguments.value("same_comparison", false);
    const std::size_t limit =
        static_cast<std::size_t>(std::clamp(arguments.value("limit", 30LL), 1LL, 500LL));
    std::vector<Catch> catches = findCatches(snapshot);
    const auto involved = [&only](const Catch& c) {
        if (only.empty() || only.count(c.frame)) {
            return true;
        }
        const auto in = [&only](const std::vector<uint32_t>& frames) {
            return std::any_of(frames.begin(), frames.end(),
                               [&only](uint32_t f) { return only.count(f) > 0; });
        };
        return in(c.otherComparison) || in(c.sameComparison);
    };
    std::vector<const Catch*> shown;
    for (const Catch& c : catches) {
        if (involved(c) && (!c.otherComparison.empty() || withSame)) {
            shown.push_back(&c);
        }
    }
    std::ostringstream text;
    text << "Frames whose comparison (mask, shape mode) also matches the originals of other "
         << "frames. The runtime tries the comparisons in frame order from the frame it found "
         << "last, wrapping around, and takes the first match: a frame caught under another "
         << "comparison is shown as the catching frame whenever that comparison comes first "
         << "(it depends on what was shown before). Frames with an equal original are "
         << "duplicates and left out. ";
    if (!withSame) {
        text << "Catches under the same comparison (the same frame to the runtime, the first in "
             << "its order wins) are left out; same_comparison: true lists them. ";
    }
    text << shown.size() << " catching frames.\n";
    for (std::size_t index = 0; index < shown.size() && index < limit; ++index) {
        const Catch& c = *shown[index];
        text << "- frame " << c.frame << " (" << sectionOf(snapshot, c.frame) << ", "
             << comparisonText(snapshot, c.frame) << ") catches ";
        if (!c.otherComparison.empty()) {
            text << c.otherComparison.size() << " under other comparisons: "
                 << frameListText(snapshot, c.otherComparison, 40);
        }
        if (withSame && !c.sameComparison.empty()) {
            text << (c.otherComparison.empty() ? "" : "; ") << c.sameComparison.size()
                 << " under the same comparison: " << frameListText(snapshot, c.sameComparison, 20);
        }
        text << '\n';
    }
    if (shown.size() > limit) {
        text << "(" << shown.size() - limit << " more; raise limit)\n";
    }
    return json{{"content", json::array({textContent(text.str())})}};
}

json frameText(const Editor& editor, const json& arguments)
{
    const Snapshot snapshot = requireSnapshot(editor, false);
    const uint32_t frame = requireFrame(snapshot.project, arguments);
    const Plane<uint8_t>& original = snapshot.originals[frame];
    const ColorizingDMDFrameInfo& info = snapshot.frames[frame];
    std::ostringstream text;
    text << "Frame " << frame << " (" << sectionOf(snapshot, frame) << ", "
         << comparisonText(snapshot, frame) << "), original " << original.width << "x"
         << original.height << ", one character per pixel: the shade (0-9, then a-z, A-Z), '.' "
         << "for 0. With the mask, pixels the comparison ignores are '#' in a second block.\n";
    const std::string plain = originalText(original, true);
    text << plain;
    if (arguments.value("with_mask", true) &&
        info.comparisonMask < snapshot.comparisonMasks.size()) {
        const Plane<uint8_t>& mask = snapshot.comparisonMasks[info.comparisonMask];
        std::string masked = plain;
        for (uint32_t y = 0; y < mask.height && y < original.height; ++y) {
            for (uint32_t x = 0; x < mask.width && x < original.width; ++x) {
                if (mask.at(x, y)) {
                    masked[y * (original.width + 1) + x] = '#';
                }
            }
        }
        text << "\nmask " << info.comparisonMask << " ('#' ignored, the shade where compared):\n"
             << masked;
    }
    return json{{"content", json::array({textContent(text.str())})}};
}

json identifyOriginal(const Editor& editor, const json& arguments)
{
    const Snapshot snapshot = requireSnapshot(editor, false);
    Plane<uint8_t> original;
    std::string what;
    if (arguments.contains("rows")) {
        std::vector<std::string> rows;
        for (const json& row : arguments["rows"]) {
            rows.push_back(row.get<std::string>());
        }
        std::string error;
        original = parseOriginalText(rows, snapshot.project.originalWidth,
                                     snapshot.project.originalHeight, snapshot.project.colorCount,
                                     error);
        if (original.empty()) {
            throw ToolError(error);
        }
        what = "The frame given";
    } else {
        const uint32_t frame = requireFrame(snapshot.project, arguments);
        original = snapshot.originals[frame];
        what = "The original of frame " + std::to_string(frame);
    }
    const std::vector<uint32_t> frames = matchingFrames(snapshot, original);
    std::ostringstream text;
    if (frames.empty()) {
        text << what << " matches no frame: the runtime does not colorize it (it shows the "
             << "frame before or the original, as the project's settings say).";
    } else {
        text << what << " matches " << frames.size() << " frames. The runtime takes the first "
             << "whose comparison it tries, in frame order from the frame it found last; more "
             << "than one comparison listed means the result depends on what was shown before.\n";
        std::map<std::string, std::vector<uint32_t>> byComparison;
        for (uint32_t frame : frames) {
            byComparison[comparisonText(snapshot, frame)].push_back(frame);
        }
        for (const auto& [comparison, members] : byComparison) {
            text << "- " << comparison << ": " << frameListText(snapshot, members, 30) << '\n';
        }
    }
    return json{{"content", json::array({textContent(text.str())})}};
}

// ---- The runtime and identification changes (editor API version 5) ------------------------

void requireRuntime(const Editor& editor)
{
    if (!editor.canRunRuntime()) {
        throw ToolError("The editor's API is version " + std::to_string(editor.apiVersion()) +
                        "; running libserum and changing identification need version 5 (a "
                        "newer editor).");
    }
}

// An original given as rows in the get_frame_text format.
Plane<uint8_t> originalFromRows(const Snapshot& snapshot, const json& rows)
{
    std::vector<std::string> lines;
    for (const json& row : rows) {
        lines.push_back(row.get<std::string>());
    }
    std::string error;
    Plane<uint8_t> original =
        parseOriginalText(lines, snapshot.project.originalWidth, snapshot.project.originalHeight,
                          snapshot.project.colorCount, error);
    if (original.empty()) {
        throw ToolError(error);
    }
    return original;
}

// One input of a sequence: a project frame's original (an index or {"frame": n}), an original
// as rows ({"rows": [...]}), either with patches over it ({"x", "y", "rows"}: shades written,
// '?' keeps the pixel), repeated "repeat" times.
std::vector<std::pair<std::string, Plane<uint8_t>>> sequenceFrom(const Snapshot& snapshot,
                                                                 const json& sequence)
{
    if (!sequence.is_array() || sequence.empty()) {
        throw ToolError("'sequence' must list the ROM frames to feed, in order.");
    }
    std::vector<std::pair<std::string, Plane<uint8_t>>> inputs;
    for (const json& item : sequence) {
        std::string label;
        Plane<uint8_t> original;
        long long repeat = 1;
        const auto projectFrame = [&](const json& value) {
            if (!value.is_number_integer() || value.get<long long>() < 0 ||
                value.get<long long>() >= static_cast<long long>(snapshot.project.frameCount)) {
                throw ToolError("A sequence item names a frame that does not exist.");
            }
            const uint32_t frame = value.get<uint32_t>();
            label = "frame " + std::to_string(frame);
            original = snapshot.originals[frame];
        };
        if (item.is_number_integer()) {
            projectFrame(item);
        } else if (item.is_object() && item.contains("frame")) {
            projectFrame(item["frame"]);
        } else if (item.is_object() && item.contains("rows")) {
            original = originalFromRows(snapshot, item["rows"]);
            label = "given";
        } else {
            throw ToolError("A sequence item is a frame index, {\"frame\": n} or {\"rows\": "
                            "[...]}.");
        }
        if (item.is_object()) {
            label = item.value("label", label);
            repeat = std::clamp(item.value("repeat", 1LL), 1LL, 100LL);
            for (const json& patch : item.value("patches", json::array())) {
                const int x0 = patch.at("x").get<int>();
                const int y0 = patch.at("y").get<int>();
                const json& rows = patch.at("rows");
                for (std::size_t r = 0; r < rows.size(); ++r) {
                    const std::string row = rows[r].get<std::string>();
                    for (std::size_t c = 0; c < row.size(); ++c) {
                        const int x = x0 + static_cast<int>(c);
                        const int y = y0 + static_cast<int>(r);
                        if (row[c] == '?' || x < 0 || y < 0 ||
                            x >= static_cast<int>(original.width) ||
                            y >= static_cast<int>(original.height)) {
                            continue;
                        }
                        std::string error;
                        const Plane<uint8_t> one = parseOriginalText(
                            {std::string(1, row[c])}, 1, 1, snapshot.project.colorCount, error);
                        if (one.empty()) {
                            throw ToolError("A patch has '" + std::string(1, row[c]) +
                                            "', no shade of the project.");
                        }
                        original.pixels[static_cast<std::size_t>(y) * original.width + x] =
                            one.pixels[0];
                    }
                }
                label += " (patched)";
            }
        }
        for (long long index = 0; index < repeat; ++index) {
            inputs.emplace_back(label, original);
        }
    }
    if (inputs.size() > 500) {
        throw ToolError("At most 500 frames per run.");
    }
    return inputs;
}

std::string outcomeText(uint32_t outcome)
{
    switch (outcome) {
        case CDMDPI_RUNTIME_NEW:
            return "new";
        case CDMDPI_RUNTIME_SAME:
            return "same picture";
        case CDMDPI_RUNTIME_INACTIVE:
            return "inactive";
        default:
            return "unknown";
    }
}

// The run as a table, and the displays as a contact sheet when asked for.
json runContent(const Snapshot& snapshot,
                const std::vector<std::pair<std::string, Plane<uint8_t>>>& inputs,
                const RuntimeRun& run, bool sheet, std::size_t firstPicture,
                const std::string& heading)
{
    std::ostringstream text;
    text << heading
         << "#: input | outcome (new: identified and shown; same picture: the display stays, "
         << "the frame identified last or one with its picture; inactive: identified, renders "
         << "nothing; unknown: identified nothing, the display stays) | identified | on display "
         << "after it\n";
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const ColorizingDMDRuntimeResult& result = run.results[index];
        text << index << ": " << inputs[index].first << " | " << outcomeText(result.outcome)
             << " | " << idText(result.identified);
        if (result.identified != CDMDPI_NONE) {
            text << " (" << sectionOf(snapshot, result.identified) << ", "
                 << comparisonText(snapshot, result.identified) << ")";
        }
        text << " | " << idText(result.shown) << '\n';
    }
    json content = json::array({textContent(text.str())});
    if (sheet && !run.pictures.empty() && firstPicture < inputs.size()) {
        const int width = static_cast<int>(run.width);
        const int height = static_cast<int>(run.height);
        const std::size_t shown = std::min<std::size_t>(inputs.size() - firstPicture, 24);
        constexpr int kGap = 3;
        // Each row: the input (the original in gray) and the display after it.
        Picture page(2 * (width + kGap) + kGap, static_cast<int>(shown) * (height + kGap) + kGap);
        for (int y = 0; y < page.height; ++y) {
            for (int x = 0; x < page.width; ++x) {
                page.set(x, y, 0, 40, 110);
            }
        }
        for (std::size_t row = 0; row < shown; ++row) {
            const std::size_t index = firstPicture + row;
            const int top = kGap + static_cast<int>(row) * (height + kGap);
            const Picture input =
                originalPicture(inputs[index].second, snapshot.project.colorCount);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    const int ox = x * input.width / width;
                    const int oy = y * input.height / height;
                    const std::size_t i = (static_cast<std::size_t>(oy) * input.width + ox) * 3;
                    page.set(kGap + x, top + y, input.rgb[i], input.rgb[i + 1], input.rgb[i + 2]);
                    const auto [r, g, b] = fromRgb565(
                        run.pictures[index * run.width * run.height +
                                     static_cast<std::size_t>(y) * run.width + x]);
                    page.set(2 * kGap + width + x, top + y, r, g, b);
                }
            }
        }
        std::string caption = "Row by row, inputs " + std::to_string(firstPicture) + " to " +
                              std::to_string(firstPicture + shown - 1) +
                              ": the ROM frame fed (left) and the display after it (right).";
        if (firstPicture + shown < inputs.size()) {
            caption += " More with pictures_start " + std::to_string(firstPicture + shown) + ".";
        }
        content.push_back(textContent(caption));
        content.push_back(imageContent(scaled(page, 2)));
    }
    return content;
}

std::vector<uint8_t> concatenated(const std::vector<std::pair<std::string, Plane<uint8_t>>>& inputs)
{
    std::vector<uint8_t> originals;
    for (const auto& [label, original] : inputs) {
        originals.insert(originals.end(), original.pixels.begin(), original.pixels.end());
    }
    return originals;
}

std::size_t pictureStart(const json& arguments)
{
    return static_cast<std::size_t>(std::max(0LL, arguments.value("pictures_start", 0LL)));
}

json runRuntime(const Editor& editor, const json& arguments)
{
    requireRuntime(editor);
    const Snapshot snapshot = requireSnapshot(editor, false);
    const auto inputs = sequenceFrom(snapshot, arguments.value("sequence", json()));
    const bool sheet = arguments.value("pictures", true);
    const std::optional<RuntimeRun> run = editor.runOriginals(
        concatenated(inputs), static_cast<uint32_t>(inputs.size()), sheet);
    if (!run) {
        throw ToolError("The editor could not run libserum on the project.");
    }
    return json{{"content",
                 runContent(snapshot, inputs, *run, sheet, pictureStart(arguments),
                            "libserum with the whole project, fed the frames in order from a "
                            "fresh start.\n")}};
}

// A rectangle list as a plane of the original's size: 1 inside any.
std::vector<uint8_t> areasPlane(const Snapshot& snapshot, const json& areas)
{
    const uint32_t width = snapshot.project.originalWidth;
    const uint32_t height = snapshot.project.originalHeight;
    std::vector<uint8_t> plane(static_cast<std::size_t>(width) * height, 0);
    for (const json& area : areas) {
        const int x0 = std::max(0, area.at("x").get<int>());
        const int y0 = std::max(0, area.at("y").get<int>());
        const int x1 = std::min(static_cast<int>(width), x0 + area.at("width").get<int>());
        const int y1 = std::min(static_cast<int>(height), y0 + area.at("height").get<int>());
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                plane[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
    }
    return plane;
}

// A mask: "rows" with '#' for the ignored pixels (the get_frame_text mask block), or
// "ignore_areas" with "compare_areas" cut out of them.
std::vector<uint8_t> maskFrom(const Snapshot& snapshot, const json& change)
{
    const uint32_t width = snapshot.project.originalWidth;
    const uint32_t height = snapshot.project.originalHeight;
    if (change.contains("rows")) {
        const json& rows = change["rows"];
        if (rows.size() != height) {
            throw ToolError("A mask needs " + std::to_string(height) + " rows.");
        }
        std::vector<uint8_t> mask;
        for (const json& row : rows) {
            const std::string line = row.get<std::string>();
            if (line.size() != width) {
                throw ToolError("A mask row needs " + std::to_string(width) + " characters.");
            }
            for (char c : line) {
                mask.push_back(c == '#' ? 1 : 0);
            }
        }
        return mask;
    }
    std::vector<uint8_t> mask = areasPlane(snapshot, change.value("ignore_areas", json::array()));
    const std::vector<uint8_t> compare =
        areasPlane(snapshot, change.value("compare_areas", json::array()));
    for (std::size_t index = 0; index < mask.size(); ++index) {
        mask[index] = mask[index] && !compare[index] ? 1 : 0;
    }
    return mask;
}

uint16_t rgb565From(const std::string& text)
{
    if (text.size() != 7 || text[0] != '#') {
        throw ToolError("Colors are \"#rrggbb\", not '" + text + "'.");
    }
    const unsigned long value = std::stoul(text.substr(1), nullptr, 16);
    const unsigned r = (value >> 16) & 0xff;
    const unsigned g = (value >> 8) & 0xff;
    const unsigned b = value & 0xff;
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// A frame of the project, frames added by earlier changes included.
uint32_t frameArgument(uint32_t frameCount, const json& change, const char* key = "frame")
{
    const long long frame = change.at(key).get<long long>();
    if (frame < 0 || frame >= static_cast<long long>(frameCount)) {
        throw ToolError("Frame " + std::to_string(frame) + " does not exist.");
    }
    return static_cast<uint32_t>(frame);
}

// Makes the changes in the open batch (on the main thread); a description per change, or a
// ToolError naming the change the editor refused.
std::vector<std::string> makeChanges(const EditSession& session, const Snapshot& snapshot,
                                     const json& changes)
{
    const ColorizingDMDEditorAPI* api = session.api();
    std::vector<std::string> done;
    uint32_t maskCount = snapshot.project.comparisonMaskCount;
    uint32_t frameCount = snapshot.project.frameCount;
    for (const json& change : changes) {
        const std::string op = change.value("op", std::string());
        bool made = false;
        std::string what;
        if (op == "set_comparison") {
            const uint32_t frame = frameArgument(frameCount, change);
            const uint32_t mask = change.contains("mask") && !change["mask"].is_null()
                                      ? change["mask"].get<uint32_t>()
                                      : CDMDPI_NONE;
            const bool shape = change.value("shape", false);
            made = session.setFrameComparison(frame, mask, shape);
            what = "frame " + std::to_string(frame) + ": mask " + idText(mask) +
                   (shape ? ", shape mode" : "");
        } else if (op == "add_mask") {
            std::vector<uint32_t> frames;
            for (const json& frame : change.value("frames", json::array())) {
                frames.push_back(frame.get<uint32_t>());
            }
            made = session.addComparisonMask(maskFrom(snapshot, change), frames);
            what = "new mask " + std::to_string(maskCount) + " for " +
                   std::to_string(frames.size()) + " frames";
            maskCount += made ? 1 : 0;
        } else if (op == "set_mask") {
            const uint32_t mask = change.at("mask").get<uint32_t>();
            made = session.setComparisonMask(mask, maskFrom(snapshot, change));
            what = "mask " + std::to_string(mask) + " changed";
        } else if (op == "set_dynamic_zones") {
            const uint32_t frame = frameArgument(frameCount, change);
            const long long set = change.value("set", 0LL);
            if (set < 0 || set >= CDMDPI_DYNAMIC_SETS) {
                throw ToolError("A dynamic set is 0 to 31.");
            }
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> zones(api->GetDynamicMask(frame, nullptr, 0, &width, &height));
            api->GetDynamicMask(frame, zones.data(), static_cast<uint32_t>(zones.size()), nullptr,
                                nullptr);
            if (change.value("clear", false)) {
                std::fill(zones.begin(), zones.end(), uint8_t{255});
            }
            const std::vector<uint8_t> inside = areasPlane(snapshot, change.value("areas", json()));
            const uint32_t sx = std::max<uint32_t>(1, width / snapshot.project.originalWidth);
            const uint32_t sy = std::max<uint32_t>(1, height / snapshot.project.originalHeight);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    if (inside[static_cast<std::size_t>(y / sy) * snapshot.project.originalWidth +
                               x / sx]) {
                        zones[static_cast<std::size_t>(y) * width + x] = static_cast<uint8_t>(set);
                    }
                }
            }
            made = session.setDynamicZones(frame, zones);
            what = "frame " + std::to_string(frame) + ": dynamic zones of set " +
                   std::to_string(set);
        } else if (op == "copy_dynamic") {
            // Another frame's dynamic zones and the colors of all its sets, as Copy Frame
            // Settings copies them: for a frame whose zones should match frames of its kind.
            const uint32_t frame = frameArgument(frameCount, change);
            const uint32_t like = frameArgument(frameCount, change, "like");
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> zones(api->GetDynamicMask(like, nullptr, 0, &width, &height));
            api->GetDynamicMask(like, zones.data(), static_cast<uint32_t>(zones.size()), nullptr,
                                nullptr);
            std::vector<uint16_t> colors(api->GetDynamicColors(like, nullptr, 0));
            api->GetDynamicColors(like, colors.data(), static_cast<uint32_t>(colors.size()));
            const std::size_t perSet = colors.size() / CDMDPI_DYNAMIC_SETS;
            made = !zones.empty() && perSet > 0 && session.setDynamicZones(frame, zones);
            for (uint32_t set = 0; made && set < CDMDPI_DYNAMIC_SETS; ++set) {
                const std::vector<uint16_t> one(colors.begin() + set * perSet,
                                                colors.begin() + (set + 1) * perSet);
                made = session.setDynamicColors(frame, set, one);
            }
            what = "frame " + std::to_string(frame) + ": dynamic zones and colors of frame " +
                   std::to_string(like);
        } else if (op == "set_dynamic_colors") {
            const uint32_t frame = frameArgument(frameCount, change);
            std::vector<uint16_t> colors;
            for (const json& color : change.at("colors")) {
                colors.push_back(rgb565From(color.get<std::string>()));
            }
            const uint32_t set = change.value("set", 0U);
            made = session.setDynamicColors(frame, set, colors);
            what = "frame " + std::to_string(frame) + ": colors of dynamic set " +
                   std::to_string(set);
        } else if (op == "add_frame") {
            if (api->version < 6) {
                throw ToolError("Adding frames needs editor API version 6; this editor has " +
                                std::to_string(api->version) + ".");
            }
            const uint32_t like = frameArgument(frameCount, change, "like");
            if (!change.contains("original")) {
                throw ToolError("add_frame needs the original, as a sequence item.");
            }
            const auto original = sequenceFrom(snapshot, json::array({change["original"]}));
            const std::optional<uint32_t> frame =
                session.addFrame(original.front().second.pixels, like);
            made = frame.has_value();
            what = made ? "new frame " + std::to_string(*frame) + " like frame " +
                              std::to_string(like)
                        : "a frame like frame " + std::to_string(like);
            frameCount += made ? 1 : 0;
        } else if (op == "move_frames") {
            if (api->version < 6) {
                throw ToolError("Moving frames needs editor API version 6; this editor has " +
                                std::to_string(api->version) + ".");
            }
            std::vector<uint32_t> frames;
            for (const json& frame : change.value("frames", json::array())) {
                frames.push_back(frame.get<uint32_t>());
            }
            const uint32_t before = change.at("before").get<uint32_t>();
            made = session.moveFrames(frames, before);
            what = std::to_string(frames.size()) + " frames moved before frame " +
                   std::to_string(before);
        } else {
            throw ToolError("Unknown change '" + op + "'.");
        }
        if (!made) {
            throw ToolError("The editor refused the change " + std::to_string(done.size()) +
                            " (" + (what.empty() ? op : what) + "); nothing was changed.");
        }
        done.push_back(what);
    }
    return done;
}

json tryIdentificationChanges(const Editor& editor, const json& arguments)
{
    requireRuntime(editor);
    const Snapshot snapshot = requireSnapshot(editor, false);
    const auto inputs = sequenceFrom(snapshot, arguments.value("sequence", json()));
    const bool sheet = arguments.value("pictures", true);
    const std::vector<uint8_t> originals = concatenated(inputs);
    std::optional<RuntimeRun> run;
    std::vector<std::string> done;
    std::string refused;
    // The frames' comparisons as the changes leave them, for the table.
    Snapshot changed = snapshot;
    const bool begun = editor.edit("Try", [&](const EditSession& session) {
        try {
            done = makeChanges(session, snapshot, arguments.value("changes", json::array()));
        } catch (const ToolError& error) {
            refused = error.what();
            return false;
        }
        // Frames the changes added are in the table too.
        ColorizingDMDProjectInfo info{};
        if (session.api()->GetProjectInfo(&info) && info.frameCount > changed.frames.size()) {
            changed.frames.resize(info.frameCount);
        }
        for (uint32_t frame = 0; frame < changed.frames.size(); ++frame) {
            session.api()->GetFrameInfo(frame, &changed.frames[frame]);
        }
        run = runOriginalsNow(session.api(), originals, static_cast<uint32_t>(inputs.size()),
                              sheet);
        return false;
    });
    if (!begun) {
        throw ToolError("The editor could not begin an edit (the author may be editing).");
    }
    if (!refused.empty()) {
        throw ToolError(refused);
    }
    if (!run) {
        throw ToolError("The editor could not run libserum on the changed project.");
    }
    std::string heading = "With the changes (taken back again, the project is as before): ";
    for (std::size_t index = 0; index < done.size(); ++index) {
        heading += (index ? "; " : "") + done[index];
    }
    heading += ".\nlibserum fed the frames in order from a fresh start; the comparisons below "
               "are those with the changes.\n";
    return json{{"content",
                 runContent(changed, inputs, *run, sheet, pictureStart(arguments), heading)}};
}

json applyIdentificationChanges(const Editor& editor, const json& arguments)
{
    requireRuntime(editor);
    const Snapshot snapshot = requireSnapshot(editor, false);
    std::vector<std::string> done;
    std::string refused;
    const std::string name = arguments.value("name", std::string("Identification"));
    bool notRecorded = false;
    const bool begun = editor.edit(
        name,
        [&](const EditSession& session) {
            try {
                done = makeChanges(session, snapshot, arguments.value("changes", json::array()));
            } catch (const ToolError& error) {
                refused = error.what();
                return false;
            }
            return true;
        },
        &notRecorded);
    if (!begun) {
        throw ToolError("The editor could not begin an edit (the author may be editing).");
    }
    if (!refused.empty()) {
        throw ToolError(refused);
    }
    if (notRecorded) {
        throw ToolError("The editor refused the changes and took them back: in a team project a "
                        "partner works on what they change (the editor's status bar says who). "
                        "Nothing was changed; the author merges the partners' changes or asks "
                        "them first.");
    }
    std::string text = "Made as one undo step '" + name + "': ";
    for (std::size_t index = 0; index < done.size(); ++index) {
        text += (index ? "; " : "") + done[index];
    }
    text += ". The author saves the project to keep it.";
    return json{{"content", json::array({textContent(text)})}};
}

// The arguments the retheming tools share.
json choiceProperties()
{
    const json area{
        {"type", "object"},
        {"properties",
         {{"frames", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
          {"section", {{"type", "string"}, {"description", "All frames of this section"}}},
          {"x", {{"type", "integer"}}},
          {"y", {{"type", "integer"}}},
          {"width", {{"type", "integer"}}},
          {"height", {{"type", "integer"}}}}},
        {"required", {"x", "y", "width", "height"}}};
    const auto areas = [&area](const std::string& description) {
        return json{{"type", "array"}, {"items", area}, {"description", description}};
    };
    return json{
        {"keep_frames",
         {{"type", "array"},
          {"items", {{"type", "integer"}}},
          {"description",
           "Frames that keep all their text with its colors and dynamic zones: text any game "
           "has (BONUS 12X, MULTIBALL, SHOOT AGAIN, PLAYER 1), the author's pick. Their "
           "graphics, backgrounds and rotations go as everywhere."}}},
        {"fill",
         {{"type", "string"},
          {"enum", {"black", "plain"}},
          {"description", "What the rest gets: black (default) or plain (the original's shades "
                          "as a frame fresh from a dump)"}}},
        {"keep_areas",
         areas("Static content kept in its colors, in original pixels: labels that belong to "
               "varying content (BONUS of BONUS 2X, BALL, PLAYER, CREDITS)")},
        {"keep_dynamic",
         areas("Text lines and dynamic zones kept although no comparison mask lies under them")},
        {"drop_dynamic",
         areas("Text lines and dynamic zones dropped although a comparison mask lies under "
               "them")},
    };
}

json schema(json properties, json required = json::array())
{
    return json{{"type", "object"}, {"properties", std::move(properties)}, {"required", required}};
}

json changesSchema()
{
    const json area{{"type", "object"},
                    {"properties",
                     {{"x", {{"type", "integer"}}},
                      {"y", {{"type", "integer"}}},
                      {"width", {{"type", "integer"}}},
                      {"height", {{"type", "integer"}}}}},
                    {"required", {"x", "y", "width", "height"}}};
    return json{
        {"type", "array"},
        {"description",
         "Changes, in order. op set_comparison: frame, mask (index or null), shape (bool). "
         "op add_mask: frames, and the mask as rows ('#' ignored, anything else compared, the "
         "get_frame_text mask block) or ignore_areas with compare_areas cut out (original "
         "pixels); the new mask's index is the project's mask count. op set_mask: mask, rows or "
         "areas as add_mask. op set_dynamic_zones: frame, set (0-31), areas (original pixels) "
         "added to the frame's zones (clear: true drops the others first). op "
         "set_dynamic_colors: frame, set, colors (\"#rrggbb\" per shade of the original, the "
         "darkest first). op copy_dynamic: frame, like: the dynamic zones and the colors of "
         "every set of the frame like, in place of the frame's. op add_frame (editor API 6): "
         "a frame the ROM shows that the project "
         "lacks (a score with more digits), original (a sequence item: rows, or a frame with "
         "patches), made like the frame like: its pictures and settings (dynamic zones, "
         "colors, background, comparison mask); appended, its index is the frame count so far, "
         "and later changes can name it. op move_frames (editor API 6): frames, as one block "
         "in their order before the frame before (numbered as before the move; the frame count "
         "for behind the last). libserum searches in frame order, so the order matters."},
        {"items",
         {{"type", "object"},
          {"properties",
           {{"op",
             {{"type", "string"},
              {"enum", {"set_comparison", "add_mask", "set_mask", "set_dynamic_zones",
                        "set_dynamic_colors", "copy_dynamic", "add_frame",
                        "move_frames"}}}},
            {"like", {{"type", "integer"}}},
            {"before", {{"type", "integer"}}},
            {"original", {{"type", "object"}}},
            {"frame", {{"type", "integer"}}},
            {"frames", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
            {"mask", {{"type", {"integer", "null"}}}},
            {"shape", {{"type", "boolean"}}},
            {"rows", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"ignore_areas", {{"type", "array"}, {"items", area}}},
            {"compare_areas", {{"type", "array"}, {"items", area}}},
            {"areas", {{"type", "array"}, {"items", area}}},
            {"set", {{"type", "integer"}}},
            {"clear", {{"type", "boolean"}}},
            {"colors", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
          {"required", {"op"}}}}};
}

json sequenceSchema()
{
    return json{
        {"type", "array"},
        {"description",
         "The ROM frames fed to libserum, in order: a project frame's index, {\"frame\": n}, or "
         "{\"rows\": [...]} (an original in the get_frame_text format); objects may have "
         "patches ([{\"x\", \"y\", \"rows\"}]: shades written over it, '?' keeps a pixel), "
         "repeat (1-100) and label. Make frames the ROM shows that the project lacks (a score "
         "with other digits) by patching a project frame with glyphs read with get_frame_text."},
        {"items", {{"type", {"integer", "object"}}}}};
}

}  // namespace

json toolList()
{
    return json::array({
        {{"name", "project_summary"},
         {"description",
          "The project open in the ColorizingDMD editor: ROM name, frame count, shades of the "
          "original frames, plane sizes, numbers of comparison masks, sprites, backgrounds and "
          "palettes, the sections with their first frames, the current and the selected "
          "frames. Start here."},
         {"inputSchema", schema(json::object())}},
        {{"name", "list_frames"},
         {"description",
          "A table of frames: duration, comparison mask, shape mode, background, number of "
          "sprites, whether it has dynamic zones and an HD picture, trigger ID and section. "
          "'-' means none."},
         {"inputSchema",
          schema({{"start", {{"type", "integer"}, {"description", "First frame, default 0"}}},
                  {"count",
                   {{"type", "integer"}, {"description", "How many, 1 to 1000, default 100"}}}})}},
        {{"name", "get_frame_image"},
         {"description",
          "Pictures of a frame, enlarged with a grid so pixels can be counted. Views: "
          "original_and_colorized (default), original (the ROM frame in gray, one gray per "
          "shade), colorized, comparison_mask (the pixels its mask ignores in magenta), dynamic "
          "(the dynamic zones, one label color per set, over the original)."},
         {"inputSchema",
          schema({{"frame", {{"type", "integer"}, {"description", "Frame index"}}},
                  {"view",
                   {{"type", "string"},
                    {"enum", {"original_and_colorized", "original", "colorized",
                              "comparison_mask", "dynamic"}}}},
                  {"plane",
                   {{"type", "string"},
                    {"enum", {"sd", "hd"}},
                    {"description", "The colorized plane, default sd"}}},
                  {"scale", {{"type", "integer"}, {"description", "1 to 8, default 4"}}}},
                 json::array({"frame"}))}},
        {{"name", "retheme_survey"},
         {"description",
          "Retheming template, step 1 (reads only): per section, how many frames have dynamic "
          "zones and backgrounds, how much of the dynamic content the rule keeps (dynamic zones "
          "where the comparison mask lies under them: scores, counts, ball, player, credits) and "
          "the frames where the rule is doubtful. Pass the corrections and keep_frames you plan "
          "to see their effect."},
         {"inputSchema",
          schema([] {
              json properties = choiceProperties();
              properties["section"] = {{"type", "string"}, {"description", "Only this section"}};
              return properties;
          }())}},
        {{"name", "get_contact_sheet"},
         {"description",
          "Many frames in one picture, row by row, the caption names each frame and its "
          "section: to look over a project quickly, for example to find the frames with text any "
          "game has. Without frames or section: the representative frames of every section, or "
          "with candidates the text frames the retheming template keeps nothing of (see "
          "retheme_survey). With a section: its frames that differ from each other (all of them "
          "with all). The colorized view shows frames as the runtime colors them, dynamic zones "
          "included. At most 48 per call; page with start."},
         {"inputSchema",
          schema({{"frames", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                  {"section", {{"type", "string"}}},
                  {"all", {{"type", "boolean"}, {"description", "Every frame of the section"}}},
                  {"candidates",
                   {{"type", "boolean"},
                    {"description", "The text frames of all sections nothing is kept of"}}},
                  {"start", {{"type", "integer"}, {"description", "Skip this many, default 0"}}},
                  {"view",
                   {{"type", "string"},
                    {"enum", {"original", "colorized"}},
                    {"description", "Default original"}}},
                  {"columns", {{"type", "integer"}, {"description", "1 to 8, default 4"}}},
                  {"scale", {{"type", "integer"}, {"description", "1 to 4, default 2"}}}})}},
        {{"name", "retheme_preview"},
         {"description",
          "Retheming template, step 2: a frame now and as the template will have it, with the "
          "choices given (the same arguments as retheme_apply). Changes nothing. Show doubtful "
          "frames and proposed keep_areas to the author this way."},
         {"inputSchema",
          schema([] {
              json properties = choiceProperties();
              properties["frame"] = {{"type", "integer"}, {"description", "Frame index"}};
              properties["scale"] = {{"type", "integer"}, {"description", "1 to 8, default 4"}};
              return properties;
          }(),
                 json::array({"frame"}))}},
        {{"name", "retheme_apply"},
         {"description",
          "Retheming template, step 3, only after the author agreed to the choices: strips the "
          "whole project in one undoable step. Everything is blacked out except the kept text "
          "(varying lines and their labels, all text of keep_frames) with the dynamic zones "
          "that reach it, and keep_areas; graphics, dynamic zones not kept, all backgrounds and "
          "all color rotations go. Comparison masks, shape modes, sprites, triggers, sections "
          "and durations stay."},
         {"inputSchema",
          schema([] {
              json properties = choiceProperties();
              properties["name"] = {{"type", "string"},
                                    {"description", "The undo step's name, default "
                                                    "'Retheming template'"}};
              return properties;
          }())}},
        {{"name", "comparison_masks"},
         {"description",
          "Every comparison mask: how many original pixels it compares and where, the fewest "
          "lit pixels it compares of the frames using it, and those frames. Masks that compare "
          "little may identify frames that are not theirs."},
         {"inputSchema", schema(json::object())}},
        {{"name", "find_identification_conflicts"},
         {"description",
          "Frames whose comparison (mask and shape mode) also matches other frames' originals, "
          "so that the runtime may show them instead of those frames (a mask that ignores a "
          "whole score matches every score). Use it when the author reports a wrong frame "
          "shown, or before changing masks."},
         {"inputSchema",
          schema({{"frames",
                   {{"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "Only conflicts these frames are part of"}}},
                  {"same_comparison",
                   {{"type", "boolean"},
                    {"description", "Also catches under the same comparison, default false"}}},
                  {"limit", {{"type", "integer"}, {"description", "1 to 500, default 30"}}}})}},
        {{"name", "get_frame_text"},
         {"description",
          "A frame's original as text, a character per pixel, with its comparison mask: exact "
          "pixels to reason about, and rows to edit for identify_original."},
         {"inputSchema",
          schema({{"frame", {{"type", "integer"}}},
                  {"with_mask", {{"type", "boolean"}, {"description", "Default true"}}}},
                 json::array({"frame"}))}},
        {{"name", "identify_original"},
         {"description",
          "Which project frames an original matches, as the runtime identifies: a frame of the "
          "project, or a frame the ROM shows that is not in the project, given as rows in the "
          "get_frame_text format (for example a project frame with the score changed)."},
         {"inputSchema",
          schema({{"frame", {{"type", "integer"}, {"description", "A project frame's original"}}},
                  {"rows",
                   {{"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "An original, one string per row"}}}})}},
        {{"name", "run_runtime"},
         {"description",
          "Feeds ROM frames to libserum with the whole project loaded, as a machine does, and "
          "tells per frame what libserum identified and what the player sees (a picture of the "
          "input and the display per frame). The ground truth for identification: the order "
          "matters, as libserum searches from the frame it found last. Needs editor API 5."},
         {"inputSchema",
          schema({{"sequence", sequenceSchema()},
                  {"pictures",
                   {{"type", "boolean"}, {"description", "The pictures, default true"}}},
                  {"pictures_start",
                   {{"type", "integer"},
                    {"description", "The first input pictured (24 per call), default 0"}}}},
                 json::array({"sequence"}))}},
        {{"name", "try_identification_changes"},
         {"description",
          "Makes changes to identification and dynamic content, runs the sequence through "
          "libserum as run_runtime does, and takes the changes back: to check a fix before "
          "proposing it. Changes nothing in the end."},
         {"inputSchema",
          schema({{"changes", changesSchema()},
                  {"sequence", sequenceSchema()},
                  {"pictures", {{"type", "boolean"}}},
                  {"pictures_start", {{"type", "integer"}}}},
                 json::array({"changes", "sequence"}))}},
        {{"name", "apply_identification_changes"},
         {"description",
          "Makes the changes (as try_identification_changes takes them) in the project as one "
          "undoable step. Only after the author agreed to them."},
         {"inputSchema",
          schema({{"changes", changesSchema()},
                  {"name",
                   {{"type", "string"}, {"description", "The undo step's name"}}}},
                 json::array({"changes"}))}},
        {{"name", "get_selection"},
         {"description", "The frame the author looks at and the frames selected in the editor."},
         {"inputSchema", schema(json::object())}},
        {{"name", "show_frames"},
         {"description",
          "Shows a frame in the editor and selects frames, to point the author at them. "
          "Changes nothing in the project."},
         {"inputSchema",
          schema({{"frame", {{"type", "integer"}, {"description", "The frame to show"}}},
                  {"select",
                   {{"type", "array"},
                    {"items", {{"type", "integer"}}},
                    {"description", "Frames to select; default the shown one"}}}},
                 json::array({"frame"}))}},
    });
}

json callTool(const Editor& editor, const std::string& name, const json& arguments)
{
    try {
        if (name == "project_summary") {
            return projectSummary(editor);
        }
        if (name == "list_frames") {
            return listFrames(editor, arguments);
        }
        if (name == "get_frame_image") {
            return frameImage(editor, arguments);
        }
        if (name == "retheme_survey") {
            return rethemeSurvey(editor, arguments);
        }
        if (name == "get_contact_sheet") {
            return contactSheet(editor, arguments);
        }
        if (name == "retheme_preview") {
            return rethemePreview(editor, arguments);
        }
        if (name == "retheme_apply") {
            return rethemeApply(editor, arguments);
        }
        if (name == "comparison_masks") {
            return comparisonMasks(editor);
        }
        if (name == "find_identification_conflicts") {
            return identificationConflicts(editor, arguments);
        }
        if (name == "get_frame_text") {
            return frameText(editor, arguments);
        }
        if (name == "identify_original") {
            return identifyOriginal(editor, arguments);
        }
        if (name == "run_runtime") {
            return runRuntime(editor, arguments);
        }
        if (name == "try_identification_changes") {
            return tryIdentificationChanges(editor, arguments);
        }
        if (name == "apply_identification_changes") {
            return applyIdentificationChanges(editor, arguments);
        }
        if (name == "get_selection") {
            return selection(editor);
        }
        if (name == "show_frames") {
            return showFrames(editor, arguments);
        }
        throw ToolError("Unknown tool '" + name + "'.");
    } catch (const ToolError& error) {
        return json{{"content", json::array({textContent(error.what())})}, {"isError", true}};
    } catch (const json::exception& error) {
        return json{{"content", json::array({textContent(std::string("Bad arguments: ") +
                                                         error.what())})},
                    {"isError", true}};
    }
}

std::string serverInstructions()
{
    return "You are connected to the ColorizingDMD editor, where an author colorizes pinball "
           "DMD animations for Serum. The project open in the editor is what you see; the "
           "tools read it, point the author at frames, run libserum on it and make the changes "
           "the author agreed to as undoable steps. A frame has an "
           "original (the ROM's shades) and a colorized picture; comparison masks, dynamic "
           "zones, sprites and backgrounds decide how the runtime recognizes and colors frames. "
           "Look before you judge (get_frame_image), and leave artistic choices to the author.\n\n"
           "Retheming: to give a game a new look, the project is stripped to a template that "
           "keeps what varies (scores, bonus counts, ball, player, credits) and the frame "
           "identification: the text that varies with its labels; the rest is blacked out, "
           "graphics always, as they are the old theme's artwork. Run retheme_survey. Read the "
           "text frames nothing is kept of (get_contact_sheet with candidates: true) and "
           "propose as keep_frames those with text any game has (BONUS 12X, MULTIBALL, SHOOT "
           "AGAIN, PLAYER 1, EXTRA BALL, JACKPOT, REPLAY, MATCH, BALL SAVED), with the frames of "
           "their section that look alike; section names help. Look at the doubtful frames and "
           "propose corrections (keep_dynamic, drop_dynamic). Show your proposal as a list the "
           "author can check quickly, with retheme_preview of a few frames, and run "
           "retheme_apply only when the author agrees. It is one undo step.\n\n"
           "Wrong frames shown (the author or players report a frame colored as another, or a "
           "frame not shown): libserum shows the first frame whose comparison (mask, shape mode) "
           "matches, searching from the frame it found last. comparison_masks shows masks that "
           "compare little, find_identification_conflicts frames that catch others, "
           "get_frame_text the exact pixels. Frames the ROM shows that the project lacks (a "
           "score with any digits) are made by patching a project frame with glyphs read from "
           "others; identify_original and run_runtime tell what libserum does with them. A "
           "frame whose mask ignores varying content needs dynamic zones there, else it shows "
           "its own pixels instead of the ROM's. A ROM frame no project frame can catch without "
           "catching others (a score layout the project has no frame of) gets a frame of its "
           "own: the add_frame change, made like a frame of the same kind. Ask the author "
           "whether the ROM really shows such a frame, as it is made, not dumped. Check a fix "
           "with try_identification_changes on sequences as a game plays them (what comes "
           "before matters), show the author the result and apply it with "
           "apply_identification_changes only when the author agrees.";
}

}  // namespace aiassist
