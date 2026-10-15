#include "Tools.h"

#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>

#include "Png.h"

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
           "Look before you judge (get_frame_image), and leave artistic choices to the author.";
}

}  // namespace aiassist
