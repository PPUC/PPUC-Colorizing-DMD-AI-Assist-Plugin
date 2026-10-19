# AI-Assisted Colorization: the plan

Status: in development. This repository is the plugin; the editor API it uses is part of the
editor (`plugin-api/ColorizingDMDPlugin.h` in the ColorizingDMD repository).

This is an add-on for a closed group of testers, not part of the public editor releases. The goal
is to save authors time on the tedious parts, without making it easy to publish low-quality,
machine-made colorizations.

## What the first experiment showed

A user had a general-purpose AI chat colorize Pinball Magic (`pmv112`, 128x32, 4 shades,
4161 frames) from a VPX table, its ROM and gameplay videos, then write a ColorizingDMD project
(`pmv112.cRom` + `.cRP`) from the format's source code. The project loads in this editor and
saves back byte-identical, so writing the format was not the problem. What it contains is:

| | AI project | AFM (human) | TRON (human) |
|---|---|---|---|
| Frames | 4161 | 3047 | 5211 |
| Sections | 2 | 68 | 70 |
| Mean share of a frame that is dynamic | 100% | 21% | 37% |
| Distinct static colors per frame | 0 | 26 | 321 |
| Sprites / backgrounds | 0 / 0 | 55 / 102 | 31 / 0 |
| Frames with a comparison mask / shape mode | 13% / 0% | 59% / 48% | 98% / 0% |
| Color rotations | none | 2% of frames | rare |

Every frame is 100% dynamic: the dynamic sets serve as a per-region "shade -> color" lookup
(up to 32 regions per frame, 110 distinct tables). Nothing is painted, and there is no static
content. Its 44 comparison masks are rectangles cutting away 25-87% of the frame, chosen without
looking at what actually varies. The authors' first reactions:

- the result is recognizable but flat, with a lot of room for improvement;
- "all frames seem to be fully dynamic, no static content at all";
- masking oversights: the masks cover the player and ball numbers (so frames of different
  players match each other), and they will fail on large scores, which grow past the masked
  rectangle.

The way it worked was costly too. The chat had to reverse-engineer the file format, render
frames itself to look at them, and could not check its masks against the real frame stream. The
person steering it described results ("muddy", "needs more separation") instead of the AI
seeing them.

Conclusion: the weak points are decisions (what is static, what varies, what a mask must hide,
what is a sprite), and the missing feedback loop. The file format is not one of them. Both are
where the editor can help, for a human author as much as for an AI.

## Shape of the add-on

A plugin in its own repository, which authors in the tester group install into the editor's
plugin folder. (A build option of the editor was the fallback; the plugin route was chosen.)

- **Inside the editor only.** No standalone server: the AI works on the project the author has
  open, through the same edits a human makes. Each change is an ordinary named `EditCommand`:
  visible on the canvas, reviewable, undoable. The author drives, and the AI proposes.
- **A real plugin, through the plugin host the editor already has.** The editor loads VPX
  plugins (DMDUtil for DMD output) through libvpxhost on all three platforms. That interface is
  a C ABI over a message bus: a plugin depends neither on the editor's compiler nor on its Qt
  version, and is found in the plugin folder (`plugins/` beside the exe, `PlugIns/vpx` in the
  macOS bundle). The AI plugin uses the same mechanism, so loading and packaging exist already.
- **What the editor adds: a small editor API on that bus**, versioned and generic (nothing about
  AI in it), so it can live in the main branch and ship in public releases:
  - read: project summary, sections, frames (original, colorized, masks, dynamic data), sprites,
    backgrounds, palettes; frames rendered through libserum; the coverage of a dump;
  - edit: a batch of changes under one name, applied as one undoable step (pixels, dynamic
    zones and sets, comparison masks and their assignment, shape mode, sections, sprites,
    backgrounds), and proposals shown on the canvas before they are applied;
  - UI: a menu entry and a status line for the plugin, the selected frames, a notification when
    the project changes.
- **The plugin repository** (for example `PPUC/ColorizingDMD-AI-Assist`): the analysis code
  (variation, mask merging, sprite candidates, carry-over of colors), the local MCP endpoint
  (Streamable HTTP on `127.0.0.1`, a token shown in the editor), the playbook skill, and CI
  that builds the plugin for macOS arm64, Windows x64 and Linux x64/aarch64. It needs no Qt:
  the API's C types, a header-only HTTP server and a PNG encoder (frames go to the model as
  images).
- **Distribution**: plugin builds go to the tester group only; public releases have the API and
  no AI plugin.

Points to keep in mind:

- The plugin is GPLv3-or-later like the editor, and its source is open to the authors. The
  point of the closed group is only that not everyone who downloads the editor gets it.
- The editor API makes it easier for anyone to write such a plugin. The editor is open source,
  so this cannot be prevented anyway; what the closed group avoids is a ready-made tool in
  everybody's hands.

## Dedicated tools

Each tool has a deterministic part (measured from the project and the dump, testable, and
often useful to authors without any AI) and a judgment part (labels, artistic choices), which
the AI proposes and the author accepts or rejects. Results are proposals shown in the editor
first, then applied as undoable commands.

### Colorize a set of static frames

Authors paint one frame of a scene; the frames around it show the same picture with small
differences.

- Deterministic: carry the reference frame's colors to every frame of the set where the original
  pixel is the same, and list what is left: pixels whose original differs (as `Auto Copy` does
  for a selection, but for whole frames and sets).
- Judgment: the leftover pixels, in the style of the reference: which shade maps to which color
  in which region, from what the reference does with the same shades nearby. Shown as a preview
  per frame.

### Optimize masks for reuse

Authors spend a lot of time merging comparison masks so that fewer remain: fewer masks mean
fewer hashes for libserum per incoming frame.

- Deterministic: for the frames that use masks, find masks that can be merged or replaced by
  another mask without any two frames that must stay apart starting to match (checked against
  the project's frames and, with a dump, against the real frame stream); propose the merges with
  the number of masks saved, and refuse those that break identification.
- Judgment: little; the AI can explain a proposal or rank them.

### Identify dynamic parts

Points, ball in play, player number, credits, timers.

- Deterministic: variation analysis across the frames of a scene in the dump: which pixels
  change, grouped into regions, with how they change (digit-like glyphs that grow to the left,
  a single changing character).
- Judgment: what each region is (score, ball, player), and so what it needs: a comparison mask
  over a score that may grow (sized for the longest score, not the one in the dump), never a mask
  over the player or ball number when frames of different players must stay apart; dynamic
  zones where the content's color must follow its shade.

### Identify possible sprites

- Deterministic: patterns of original pixels that recur at different positions across frames
  (translation-invariant matching), with where and how often they appear, as sprite candidates
  with detection areas.
- Judgment: which candidates are real objects worth a sprite (a ball, a character) and which
  are coincidences (letters of a font).

### Retheming template

Retheming gives a game a new look while keeping what an existing colorization already solved:
which frames are told apart by which comparison masks, and where the content that varies sits
(scores, bonus counts, ball in play, number of players, credits). The template is the project
stripped down to that, for a new author (or the assistant) to paint the new theme on.

What `afm_113b` shows (3047 frames, 68 sections): 1816 frames have dynamic zones, and they are
used for more than the varying content: in many sections they cover 25-60% of the frame, to
color fixed text by its shades. But about 90% of the dynamic pixels lie under the frame's
comparison mask, which hides exactly the content that varies from identification. The
exceptions are few sections (6% in one, 37% in another, two with dynamic zones and no mask).

- Deterministic: for every frame, keep the dynamic zones where they lie under the frame's
  comparison mask, with their colors; everything else gets the look of a frame fresh from a
  dump (the original shades in plain colors), so nothing disappears and the new author colors
  it with the usual tools. Backgrounds and color rotations go. Sprites, comparison masks, shape
  modes, triggers, sections and durations stay. A survey lists, per section, how much of the
  dynamic content that rule keeps and the frames where it is doubtful.
- Judgment: the doubtful frames (dynamic zones outside the mask that are varying content after
  all, or masked ones that are not); static labels that belong to varying content and are kept
  in their colors (`BONUS` in `BONUS 2X`, `BALL`, `PLAYER`, `CREDITS`); frames kept whole as
  references of the old theme (with their backgrounds and rotations). The assistant proposes
  them, the author picks.
- Applied as one undoable step (editor API 2: an edit batch).
- Later: the same from a dump without a project, finding the varying content itself.

### Read and check

Project summary, sections, frames with filters, masks, dynamic sets, sprites, backgrounds;
rendering through libserum (Runtime view); a detection coverage report for a dump (identified
frames, frames matching the wrong scene, unidentified frames, as the Frame Tester does). These
close the feedback loop the first experiment lacked.

## Knowledge instead of training

Fine-tuning a model is out of reach and unnecessary at first. What helps is context:

- **A colorization playbook** (a Claude skill shipped with the add-on): the workflow authors
  follow, how to split scenes into sections, when to use static paint, dynamic zones, comparison
  masks, shape mode, sprites, backgrounds and rotations, common mistakes (the ones above), and
  that the AI asks the author about artistic choices instead of guessing them.
- **Reference statistics from authors' projects**, only with their consent: how existing
  projects solved similar scenes, as numbers and examples the tools can retrieve. The projects
  stay local; only derived patterns go into the playbook.
- **MCP prompts** for the main tasks (colorize a set of static frames, review the masks of a
  section, find dynamic parts in a dump), each pulling in the playbook and the relevant tools.

## How to tell whether it works

- Coverage of a real dump: identified frames, wrong matches, unidentified frames.
- Fewer masks after optimization with identical coverage.
- Testers' time for a section with and without the add-on, and their review of the result.
- The statistics of the first section as a smell test (fully dynamic frames, masks over player
  numbers).

## First steps

1. The editor API's read part on the plugin bus (main branch), and the plugin repository with an
   endpoint that only reads (project summary, frames as images, coverage report), built on CI
   for every platform.
2. Variation analysis and the mask merge check in the plugin, with tests; then the
   "identify dynamic parts" and "optimize masks" tools on top.
3. "Colorize a set of static frames": the deterministic carry-over first, the judgment part
   after.
4. Sprite candidates.
5. The playbook draft, reviewed by testers.

Open questions:

- Who is in the tester group, and on what terms authors share projects for reference
  statistics.
- Whether a project saved with assisted edits should say so (a note in the `.cRPX` chunks, for
  example), so that assistance is visible to whoever later works on it.
