# ColorizingDMD AI Assist

A plugin for the [ColorizingDMD](https://github.com/PPUC/ColorizingDMD) editor that lets an AI
assistant (Claude Code, Claude Desktop) work on the project open in the editor, through a local
MCP endpoint. The author stays in control: the assistant reads the project, analyses it and
proposes; changes become ordinary undoable edits in the editor.

For a closed group of testers; it is not part of the editor's public releases. See
[docs/plan.md](docs/plan.md) for why and how.

## State

- Read-only: project summary, frame list, frames as images for the model, selection; pointing
  the author at frames.
- Planned: the analysis tools (varying pixels, mask merging, dynamic parts, sprite candidates)
  and proposals applied as undoable edits.

## Build

Needs the ColorizingDMD sources (for the editor API header) and the vpinball sources the editor
is built with (for the plugin SDK headers); nothing else to install: the HTTP server
(cpp-httplib 0.18.3) and JSON (nlohmann/json 3.11.3) are in `third-party/`.

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCOLORIZINGDMD_DIR=/path/to/ColorizingDMD \
        -DVPINBALL_DIR=/path/to/vpinball \
        -DCOLORIZINGDMD_PLUGIN_DIR=/path/to/editor/build/vpx-plugins
    cmake --build build

The plugin's folder is `build/AIAssist` (`plugin.cfg` and the library); with
`COLORIZINGDMD_PLUGIN_DIR` it is copied into a development editor's plugin folder. For a
packaged editor, copy `AIAssist` into its plugin folder (`plugins` beside the program on
Windows and Linux, `ColorizingDMD.app/Contents/PlugIns/vpx` on macOS).

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
mask, dynamic zones; enlarged with a pixel grid), `get_selection`, `show_frames`.

## License

GPLv3 or later, as the editor.
