#include "Tools.h"

#include <algorithm>
#include <array>
#include <set>
#include <sstream>
#include <stdexcept>

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
    text << "Rule: a dynamic zone stays (with its colors) where the frame's comparison mask lies "
            "under it; everything else is blacked out. Frames in keep_frames keep their colors "
            "and dynamic zones. All backgrounds and color rotations go.\n\n"
            "section | first frame | frames | with dynamic zones | with background | dynamic "
            "pixels kept | doubtful frames | representative frames\n";
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
            "frames to keep (text any game has: BONUS 12X, MULTIBALL, SHOOT AGAIN, PLAYER 1), "
            "look at the representative frames with get_contact_sheet.";
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
        for (const SectionSurvey& section : survey(snapshot, RethemeChoices{})) {
            frames.insert(frames.end(), section.representative.begin(),
                          section.representative.end());
        }
        what = "the representative frames of every section";
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
        const Picture tile = colorized ? colorizedPicture(pictures[index])
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
           "Frames that keep their colors and dynamic zones: text any game has (BONUS 12X, "
           "MULTIBALL, SHOOT AGAIN, PLAYER 1), the author's pick. Their backgrounds and "
           "rotations go too."}}},
        {"fill",
         {{"type", "string"},
          {"enum", {"black", "plain"}},
          {"description", "What the rest gets: black (default) or plain (the original's shades "
                          "as a frame fresh from a dump)"}}},
        {"keep_areas",
         areas("Static content kept in its colors, in original pixels: labels that belong to "
               "varying content (BONUS of BONUS 2X, BALL, PLAYER, CREDITS)")},
        {"keep_dynamic", areas("Dynamic zones kept although no comparison mask lies under them")},
        {"drop_dynamic", areas("Dynamic zones dropped although a comparison mask lies under them")},
    };
}

json schema(json properties, json required = json::array())
{
    return json{{"type", "object"}, {"properties", std::move(properties)}, {"required", required}};
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
          "game has. Without frames or section: the representative frames of every section "
          "(see retheme_survey). With a section: its frames that differ from each other (all of "
          "them with all). At most 48 per call; page with start."},
         {"inputSchema",
          schema({{"frames", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                  {"section", {{"type", "string"}}},
                  {"all", {{"type", "boolean"}, {"description", "Every frame of the section"}}},
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
          "whole project in one undoable step. Frames not in keep_frames are blacked out except "
          "the kept dynamic zones and keep_areas; dynamic zones not kept, all backgrounds and all "
          "color rotations go. Comparison masks, shape modes, sprites, triggers, sections and "
          "durations stay."},
         {"inputSchema",
          schema([] {
              json properties = choiceProperties();
              properties["name"] = {{"type", "string"},
                                    {"description", "The undo step's name, default "
                                                    "'Retheming template'"}};
              return properties;
          }())}},
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
           "tools read it and can point the author at frames, nothing more yet. A frame has an "
           "original (the ROM's shades) and a colorized picture; comparison masks, dynamic "
           "zones, sprites and backgrounds decide how the runtime recognizes and colors frames. "
           "Look before you judge (get_frame_image), and leave artistic choices to the author.\n\n"
           "Retheming: to give a game a new look, the project is stripped to a template that "
           "keeps what varies (scores, bonus counts, ball, player, credits) and the frame "
           "identification; the rest is blacked out, as it is the old theme's artwork. Run "
           "retheme_survey; find the frames with text any game has (BONUS 12X, MULTIBALL, SHOOT "
           "AGAIN, PLAYER 1, EXTRA BALL, JACKPOT) on contact sheets of the representative "
           "frames (get_contact_sheet) and propose them as keep_frames, or keep_areas for such "
           "text next to artwork; look at the doubtful frames and propose corrections "
           "(keep_dynamic, drop_dynamic); show them with retheme_preview, and run retheme_apply "
           "only when the author agrees. It is one undo step.";
}

}  // namespace aiassist
