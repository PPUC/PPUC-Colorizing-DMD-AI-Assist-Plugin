# ColorizingDMD AI Assist

A plugin for the [ColorizingDMD](https://github.com/PPUC/ColorizingDMD) editor that lets an AI
assistant (Claude Code, Claude Desktop) work on the project open in the editor, through a local
MCP endpoint. The author stays in control: the assistant reads the project, analyses it and
proposes; changes become ordinary undoable edits in the editor.

For a closed group of testers; it is not part of the editor's public releases. See
[docs/plan.md](docs/plan.md) for why and how.

## State

- Read-only: project summary, frames (as images for the model), selection.
- Planned: the analysis tools (varying pixels, mask merging, dynamic parts, sprite candidates)
  and proposals applied as undoable edits.

## License

GPLv3 or later, as the editor.
