# Finding and fixing wrong frame identification with Claude

A short tutorial for colorization authors. It shows how to let Claude Code (or another MCP
client) check a project for frames the runtime identifies wrongly, try a fix, and apply it in
the editor, using the AI Assist plugin.

## What goes wrong

libserum identifies every ROM frame by comparing it with the originals of the project's
frames. A frame's comparison mask decides which pixels count. A frame matches when the
incoming frame equals its original on every compared pixel.

The search order matters. libserum starts at the frame it found last, goes on in frame order
and wraps around; the first match wins. Two consequences:

- A mask that ignores too much catches frames that belong to other frames. Whether the player
  sees the right one then depends on what was shown before. Such bugs come and go.
- A frame the project does not know keeps the previous picture on the display. A final score
  that is not identified at all leaves *Game Over* on the display.

A frame without dynamic zones shows its stored picture, whatever the ROM sends. A score frame
needs dynamic zones over its digits to show the player's real score.

## Setup

1. Turn on the plugin: check *AI Assist* in `Settings -> Plugins` and restart the editor.
2. `Plugins -> AI Assist: Connection...` shows the command for Claude Code. Run it once in a
   terminal:

   ```sh
   claude mcp add --transport http colorizingdmd http://127.0.0.1:8975/mcp \
       --header "Authorization: Bearer <token>"
   ```

3. Open the project in the editor and keep the editor running.
4. Start `claude` in a terminal and ask `/mcp` whether `colorizingdmd` is connected.

Claude only reads until you ask for a change. Every applied change is one undo step in the
editor (`Edit -> Undo`), and nothing is saved until you save the project.

## The tools Claude uses

| Tool | What it does |
| --- | --- |
| `find_identification_conflicts` | Lists frames whose mask also matches other frames' originals. |
| `comparison_masks` | How many pixels each mask compares, where, and which frames use it. |
| `get_frame_text` | A frame's original and mask as text, one character per pixel. |
| `identify_original` | Which frames an original (also an edited one) would match. |
| `run_runtime` | Feeds a sequence of ROM frames to libserum and shows input and display. |
| `try_identification_changes` | Runs the sequence with changes and takes them back. |
| `apply_identification_changes` | Makes the changes in the editor, as one undo step. |

`run_runtime` is the ground truth: it runs libserum with the whole project, as a machine does.
Sequences can contain frames the project does not have, for example a score with other
digits, made by editing a frame's text.

## Prompts

Copy a prompt and replace the parts in angle brackets.

### 1. Check a project for identification problems

```text
The project <ROM name> is open in the ColorizingDMD editor (MCP server colorizingdmd).
Check it for frames that the runtime may identify wrongly: use
find_identification_conflicts and comparison_masks, and for every conflict decide with
run_runtime whether a player can actually see a wrong picture (think about what the ROM
shows before that frame). Do not change anything. Report the real problems first, with the
frame numbers, the order in which they happen, and a picture from run_runtime.
```

### 2. Investigate a bug report

```text
The project <ROM name> is open in the ColorizingDMD editor (MCP server colorizingdmd).
A player reports: "<what the player saw, for example: after the game the final score is
not shown, Game Over stays on the display>".
Find the frames involved (list_frames, get_contact_sheet, get_frame_text). Rebuild what
the ROM sends as a run_runtime sequence, including frames the project does not have
(other scores, other player counts) made by editing frame text. Show me what the player
sees and explain the cause. Do not change anything yet.
```

### 3. Propose and try a fix

```text
Propose the smallest fix for this problem: comparison masks, shape mode, dynamic zones
and dynamic colors. Check it with try_identification_changes on the sequence that shows
the bug, and also on sequences around it (the normal game frames that use the same masks,
other scores), so the fix breaks nothing else. Show me before and after. Do not apply it.
```

### 4. Apply

```text
Apply the fix with apply_identification_changes under the name "<what it fixes>", then
run the sequences again with run_runtime and show me the result.
```

After that, look at the frames in the editor (Claude can point at them with `show_frames`),
and share it as usual: in a team project `Team -> Share My Changes` saves it and sends it to
the partners.

## Example: Baddest Cats final scores

The report: on the 5,000,000 shot a special frame comes up. Its mask hides the whole score.
The one-player final score frame hides everything but its last zero. After the game the
score is not shown in the loop.

What Claude found with the tools:

- The one-player final score frame compared only the last zero. It had no dynamic zones, so
  every player saw the stored score of the dump, not their own.
- The two-player final score frame had no mask, so the real two-player scores matched no
  frame. The display kept *Game Over*.
- The one-player final mask also matches the 5,000,000 frame. That only shows when the final
  score was the frame identified last, which the ROM does not do in a normal game: a new
  game shows the in-game score first. That conflict is harmless.

The fix, tried and applied through the tools:

1. Dynamic zones over the digits of the one-player final score, with dynamic colors.
2. A new mask for the two-player final score that compares the last zero of each score and
   ignores the other digits, plus dynamic zones over both scores.

`run_runtime` then showed the real scores for one to four players, in game and after it, and
the 5,000,000 frame unchanged during the game.

## Tips

- Ask for pictures. `run_runtime` and `try_identification_changes` show the ROM frame next to
  what the player sees, row by row.
- Let Claude test with scores the project does not have. A fix that works for the dump only
  is no fix.
- A conflict in `find_identification_conflicts` is not always a bug. Ask Claude whether the
  ROM ever sends the frames in the order that shows it.
- Keep changes small and review them in the editor before saving.
