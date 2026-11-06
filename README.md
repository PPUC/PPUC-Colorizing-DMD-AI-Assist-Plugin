# ColorizingDMD AI Assist

A plugin for the [PPUC Colorizing DMD](https://github.com/PPUC/PPUC-Colorizing-DMD) editor
that lets an AI assistant (Claude Code, Claude Desktop) work on the project open in the editor,
through a local MCP endpoint. The author stays in control: the assistant reads the project,
analyses it and proposes; changes become ordinary undoable edits in the editor.

For a closed group of testers; it is not part of the editor's public releases. See
[docs/plan.md](docs/plan.md) for why and how.

## State

- Reading: project summary, frame list, frames as images for the model, selection; pointing
  the author at frames.
- Retheming template: a survey, a preview per frame and the strip itself, applied as one
  undoable step (needs editor API version 2).
- Identification: comparison masks and the frames they catch, originals as text, which frames
  an original matches; libserum run over sequences of ROM frames (also frames the project
  lacks), and changes to masks, shape modes, dynamic zones and colors tried on such sequences
  and taken back, or applied as one undoable step (needs editor API version 5); new frames for
  ROM frames no project frame can catch, made like another frame, and frames moved, as the
  search order matters (needs version 6).
- Planned: the analysis tools (varying pixels, mask merging, dynamic parts, sprite candidates).

## Build

Needs the ColorizingDMD sources (for the editor API header) and the vpinball sources the editor
is built with (for the plugin SDK headers); nothing else to install: the HTTP server
(cpp-httplib 0.18.3) and JSON (nlohmann/json 3.11.3) are in `third-party/`.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCOLORIZINGDMD_DIR=/path/to/ColorizingDMD \
        -DVPINBALL_DIR=/path/to/vpinball \
        -DCOLORIZINGDMD_PLUGIN_DIR=/path/to/editor/build/vpx-plugins
    cmake --build build

The plugin's folder is `build/AIAssist` (`plugin.cfg`, the library, `LICENSE`, this README
and `THIRD-PARTY-NOTICES.md`); with
`COLORIZINGDMD_PLUGIN_DIR` it is copied into a development editor's plugin folder. For a
packaged editor, copy `AIAssist` into its plugin folder (`plugins` beside the program on
Windows and Linux, `ColorizingDMD.app/Contents/PlugIns/vpx` on macOS).

CI (`.github/workflows/build.yml`) builds and tests the plugin with warnings as errors for
Windows (MinGW), Linux x86_64 and aarch64 and macOS arm64, against the editor API of a pinned
editor commit (`COLORIZINGDMD_SHA`) and the vpinball plugin SDK the editor pins
(`VPINBALL_SHA`). Every run uploads a plugin package per platform; a tag `v<version>` (as in
`CMakeLists.txt` and `plugin.cfg`) collects them into a draft release.

## Use

1. In the editor, check `AI Assist` under Settings -> Plugins (it loads at once and at every
   start).
2. `Plugins -> AI Assist: Connection...` shows the address and the command for Claude Code:
   `claude mcp add --transport http colorizingdmd http://127.0.0.1:8975/mcp --header
   "Authorization: Bearer <token>"`. The token is kept by the editor and stays the same.
3. Open a project in the editor and ask Claude about it.

The endpoint listens on 127.0.0.1 only (port 8975, or the next free one up to 8984) and
answers only requests with the token.

Tools: `project_summary`, `list_frames`, `get_frame_image` (original, colorized, comparison
mask, dynamic zones; enlarged with a pixel grid), `get_selection`, `show_frames`,
`get_contact_sheet` (many frames in one picture), for the retheming template `retheme_survey`,
`retheme_preview` and `retheme_apply`, and for identification `comparison_masks`,
`find_identification_conflicts`, `get_frame_text`, `identify_original`, `run_runtime`,
`try_identification_changes` and `apply_identification_changes`.

Identification: when a wrong frame or no frame shows (players report a score that is not
shown after the game, say), ask Claude to find out why. It looks for masks that compare
little and frames that catch others, builds the ROM frames the project lacks (a score with
other digits) from the glyphs of project frames, runs them through libserum in the order a
game shows them, tries a fix on the same sequences and shows you the result; it applies the
fix only when you agree, as one undo step.

Retheming: ask for a retheming template of the open project. The template keeps the text that
varies (scores, counts, ball, player) as whole lines with their labels and blacks out the rest,
graphics always, as they are the old theme's artwork. The assistant runs the survey, reads the
text frames nothing is kept of on contact sheets for text any game has (BONUS 12X, MULTIBALL,
SHOOT AGAIN, PLAYER 1) to keep, looks at the frames where the rule is doubtful, shows its
proposals, and strips the project only when you agree. One Undo takes it back; save the template under a new name.

Tests (no editor needed): `ctest --test-dir build`.

## License

GPLv3 or later, as the editor.

cpp-httplib and nlohmann/json are MIT licensed; `THIRD-PARTY-NOTICES.md` has their notices and
goes into the plugin's folder.
