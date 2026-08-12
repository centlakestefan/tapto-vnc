# Clicking accurately with a local vision model

How tapto-vnc gets a model to put the pointer where it means to, why the
obvious approaches fail, and how to qualify a new model in about ten minutes.

Everything below was measured against **Gemma-4-31B (AWQ 4-bit, vLLM)** driving
a Windows 10 guest at 1280x1024. Numbers are from saved frames, not from what
the model said it did — see [Only the frames are evidence](#only-the-frames-are-evidence).

## The short version

A vision model that reads a screen perfectly well can still be unable to say
*where* on it something is. Describing and locating are different capabilities,
and the second one fails quietly: the model reports success either way.

What works is to stop asking it to estimate a position and start asking it to
read one. `vnc_zoom` returns the requested region enlarged, with a numbered
ruler along the top and left edge and a grid drawn from them, **labelled in
screen coordinates**. The model finds the gridlines bracketing its target,
reads their numbers, and passes those to `vnc_click`.

With that in place the same model that missed a checkbox by 103 px hit a
`Finish` button to within 5 px, and completed an unattended MSI install.

## Why full-screen clicking cannot work

A vision model compresses every image to a fixed token budget regardless of its
size. Gemma 4's processor config sets `image_seq_length: 280`, so the whole
screen arrives as roughly a 17x17 grid of soft tokens:

| what is sent | screen px per token cell |
| --- | --- |
| full 1280x1024 | ~75 x 60 |
| 320x256 crop | ~19 x 15 |
| 320x100 crop | ~19 x 6 |

Rows in a file listing are **21 px** apart. At full screen one token cell spans
three of them, so the information needed to pick a row is not in the input at
all. No prompting fixes that; the fix is to send less screen.

Two further properties of that processor are worth knowing:

- `do_resize` is on and pan-and-scan is off by default, so a non-square
  screenshot is **squashed** to a square rather than letterboxed. Nothing needs
  to invert that: the model answers in the screenshot's own pixel coordinates
  (see below), so the squash is already undone on its side. Do not "fix" it by
  padding client-side either, which would spend ~20% of the token budget on
  black bars.
- Enlarging a crop costs **no extra tokens**, since the budget is fixed. That
  is why `vnc_zoom` enlarges to a ~896 px long side (the vision tower's native
  resize) and never returns a region at 1:1.

## What the model actually does

### It answers in pixels, not normalised units

Three independent lines of evidence, all pointing the same way:

1. In a 200-tall image the model answered `y=100` for a target at image y=102,
   and the click landed 1 px away. Under a 0-1000 reading that value would mean
   image y=20 and miss by 40 px.
2. X has been accurate in pixel terms throughout, full screen and zoomed. A
   model emitting 0-1000 would be wrong in X by the same factor as Y.
3. A failing answer of `380` in an 800-wide image is empty panel under a
   0-1000 reading; as pixels it is the exact centre of the **Back** button,
   which is what the frames show it hit.

There used to be a `--coord-span` flag that rescaled coordinates from a
normalised 0..N grid, on the theory that the PaliGemma `<locNNNN>` lineage
would answer that way. It has been removed. No model tested here ever needed
it — the evidence above says they all answer in pixels — and keeping it made
the pixel contract conditional on a flag when the point of the whole design is
that there is exactly one coordinate system. The tool descriptions still say
"not a normalised 0-1000 or 0-1 value", which is the cheap half of the fix and
the half that works.

### Its error is a fraction of the region, biased toward the centre

Same target, same region size, three different placements within the region
(image 640x200, target `readme` at screen y=231):

| region top | target's fraction of region | model's answer | error |
| --- | --- | --- | --- |
| 225 | 0.06 | 0.15 | +9 px |
| 210 | 0.21 | 0.25 | +4 px |
| 180 | **0.51** | **0.50** | **-1 px** |

A line through those is `answer ~= 0.78 x true + 20` in image pixels: correct
units, compressed about 22% toward the centre of the image. Hence two rules
that fall straight out of it — **keep the region short**, because the error
scales with its height, and **put the target near the middle**, because the
compression vanishes there.

### It may read one axis off the ruler and the other in image pixels

Qwen3.6-27B, asked for a file row in a ruled 320x100 view at 3x, answered Y
correctly in screen coordinates (errors +6, -6, -1 across three placements) and
X as **raw image pixels** — measuring the target accurately, then reporting the
measurement in the wrong coordinate space.

This hides easily. With the region at x=150 the two readings differ by only
~100 px and look like ordinary aiming error. Move the region to **x=0** and
they separate completely:

| region x | model's X | image centre | screen centre |
| --- | --- | --- | --- |
| 150 | 300-340 | 352 | 250 |
| **0** | **~790** | **804** | **250** |

At x=0 the answer tracks the *image* centre, and the model's own stated span
(730-860) is within 10 px of the true image span (735-870). Perception was
never the problem; units were.

**Shifting the region's origin is the cheap discriminator** — a constant
offset, a scaling error and a coordinate-space confusion all look alike until
you move it.

The likely reason the two axes differ: for Y the ruler label sits on the same
horizontal line as the target, so it is read straight across; for X the label
is at the top and requires tracing a long gridline. The fix was to state the
conversion in the `vnc_zoom` result with the run's actual numbers substituted:

```
screen_x = <region.x> + (image_x - 54) / <scale>
screen_y = <region.y> + (image_y - 19) / <scale>
```

That alone moved the same request from x=790 to x=230, against a target
spanning 227-272. It plays to a reasoning model's strength — it will do the
arithmetic if told what arithmetic to do.

### The failing axis is whichever one has the coarser grid

This is the finding that made the difference, and it was a bug in this
codebase rather than a property of the model.

`niceStep()` originally chose gridline spacing per axis from that axis's own
extent. A 400x100 region therefore got lines every **50 px horizontally** and
every **10 px vertically**. Asked to tick a checkbox whose true position was
(426, 610):

| attempt | model's answer | Y error | X error |
| --- | --- | --- | --- |
| 1 | (323, 610) | **0** | **-103** |
| 2 | (351, 610) | **0** | **-75** |

Same image, same model, same moment: the axis with the 10 px grid was read to
the exact pixel, the axis with the 50 px grid was wrong by roughly one grid
step. A position can only be read as precisely as the nearest line.

Subdivisions at the finer of the two steps are now drawn on **both** axes
(fainter than the labelled lines, so they read as subdivisions). After that
change, on the same guest, a `Finish` button centred at (745, 658) was clicked
at (750, 660).

**If you change the grid code, keep the two axes at the same resolution.**

## Design rules, and where they live in the code

| rule | enforced by |
| --- | --- |
| Grid resolution equal on both axes | `minorStep` in `screenshotRegionPng()` |
| Region at most 640x200 | `kMaxZoomWidth` / `kMaxZoomHeight`, clamped and reported |
| Never returned at less than 2x | `kMinZoomScale` |
| Default region 320x100 | `doZoom()` |
| One coordinate system everywhere | rulers in `screenshotRegionPng()`; there is no second one |
| Click must follow a zoom containing it | `--require-zoom on`, off by default |

Caps are enforced rather than requested. The tool description asked for short
regions and a real run chose 400, then 300, then 200 and missed with all three
before succeeding at 100. Guidance a model can decline is not a mechanism.

## The zoom gate

`--require-zoom on` (config key `require-zoom`) refuses a `vnc_click` unless
both hold:

1. `vnc_zoom` has been called since the last action. Every action clears the
   flag, because a zoom of the previous screen is not evidence about the
   current one.
2. The clicked point lies **inside** that region. Without this the rule is
   satisfiable by zooming anywhere and clicking anywhere.

A refused click is not delivered — no pointer event reaches the guest — and the
error says what to do instead, including the case worth naming explicitly: if
the target was not in the last zoom, widen the region rather than clicking past
its edge.

Off by default, and that default is load-bearing rather than cautious.
gemini-3.6-flash completed the same task with no gate, no misses, and two
correct clicks that followed another click without an intervening zoom — both
of which the gate would have refused, adding round trips to a run that was
already five times faster than either local model. Turn it on for local models:
every one tried here has been told in the tool description to zoom first, and
every one has ignored it at least once. Guessing a position from a full screenshot is the single most common way
a run goes wrong, and it is the one failure this makes impossible.

It forces the model to *look*, not to look *correctly* — a click inside a
correctly-zoomed region can still land on the wrong row, and it does nothing
about picking the wrong item by name.

**Do not widen it to a proximity check.** An obvious-looking extension is to
warn when a click repeats a nearby earlier one. In a wizard, Next on one page
and Install on the next occupy nearly the same pixels: one successful run
clicked the identical coordinate three times, correctly, on three consecutive
pages. The repeat-click warning is therefore exact-match only, and catches
less than it could on purpose.

## The design this replaced

Before the rulers, `vnc_zoom` returned a plain enlarged crop and a second tool,
`vnc_click_zoom`, took a position measured *inside* that image and mapped it
back to the screen. Both tools existed at once, so two pixel grids were live at
once and the model had to track which one it was answering in.

It did not. Asked for a checkbox at image (377, 181) in a 900x300 view it
answered **(50, 180)**: the Y exact, the X collapsed to the left edge. That is
the failure the rulers were built to remove — reading a labelled gridline is a
different task from estimating a fraction of an image, and reading is what
these models are good at.

Both the second tool and the switch that selected between the two schemes have
since been deleted. They are recorded here rather than kept behind a flag: a
configurable coordinate system is the problem, not a hedge against it. If a
future model turns out not to need the rulers, the thing to remove is the
rulers, not to reintroduce a second grid alongside them.

## Aim at the largest part of a control

Independent of any of the above, and worth applying first because it costs
nothing:

- A checkbox's **label belongs to the same control as the box**. The box is
  ~13 px wide; the sentence beside it is ~200. Measured: a click at x=436
  ticked a box centred at x=426, i.e. past the box's right edge and on the
  label.
- A list row's **name** is wide; its icon is 16 px.
- Verify afterwards. A label is not always wired to its control — this holds
  for Win32 dialogs, not universally.

The system prompt says all of this. It composes with the grid rather than
competing: the grid narrows the error, a large target makes the remainder
harmless.

## Only the frames are evidence

One file name in the examples below and in the placement test was changed for
publication (`readme`). Nothing else in the quotes or the numbers has been
touched — which in a document arguing that only the frames count would rather
defeat the point.

This model reports success it did not achieve, consistently and fluently:

- "Procmon is selected" when `install` was selected.
- "the `readme` file has been left-clicked and is now selected" when
  `bazel-8.5.0-windows-x86_64` was.
- "I zoomed into the specified region and left-clicked the `readme` file" when
  the click had navigated File Explorer to the Desktop.

It is also unreliable about **its own history**, separately from the screen.
Asked why it had missed, it explained that "the initial full-screen coordinates
were slightly off" and that switching to zoom fixed it — but the filenames show
there was never a full-screen click; the miss happened inside the zoom
workflow. Asking a model to introspect on how it produced a coordinate yields a
plausible story, not data.

Interestingly, its reports about the **current screen** became trustworthy once
zoom was available: it correctly volunteered "the file `bazel-8.5.0-...` is
currently selected instead of `readme`". Reading is reliable; recall and
self-explanation are not.

Run with `--screenshots <dir>`. Click frames carry a **red dot** at the aim
point, written only to the file on disk — never to the image the model sees,
since showing it where its last shot landed would feed back into the behaviour
being measured. Filenames record the geometry, so a run can be reconstructed
arithmetically without opening anything:

```
0036-zoomed-3x-on-300x100-at-520-620.png     region (520,620) 300x100 at 3x
0037-left-clicked-at-750-660.png             the click that followed
```

## Qualifying a new model

Ten minutes, four runs, before trusting anything to a long task.

**Set up.** `--screenshots` to a fresh directory, and a stable screen with a
known target. A file listing is
ideal: rows are evenly spaced and their positions are easy to establish with
`vnc-smoketest --zoom`.

**1. Placement test.** Pick one target. Run three separate tasks, each naming
an exact zoom region of the same size, positioned so the target sits at roughly
0.05, 0.5 and 0.9 of the region's height. Instruct the model explicitly:

```
Call vnc_zoom with exactly these arguments: x=150, y=225, width=320, height=100.
Then call vnc_click exactly once on the readme file. Do not zoom again.
```

Compute each error from the filename. What you learn:

- errors within a few px at all three placements — the model can ground; go.
- errors that grow with distance from the region's centre — the centre-bias
  above; usable, keep regions short and targets centred.
- an answer that barely moves as the target moves — it is not localising at
  all, and no amount of scaling will fix it. Try a model trained for GUI
  grounding.

**2. Repeat on the X axis.** The failure is not axis-symmetric and it moves.
Gemma got Y exact while X collapsed, having previously done the reverse; Qwen
read Y off the ruler and X in image pixels. Use a wide, short region and a
target that moves horizontally.

**2b. Shift the region's origin.** Run the same target once with the region at
`x=0` and once at `x=150`. If the answer moves with the image rather than with
the screen, the model is reporting image pixels — see [It may read one axis off
the ruler and the other in image
pixels](#it-may-read-one-axis-off-the-ruler-and-the-other-in-image-pixels).
Keeping the origin fixed across every test will hide this.

**3. Verify the environment did not drift.** An earlier four-point calibration
here was invalidated because File Explorer navigated away mid-run, so two of
the four points measured a screen that no longer contained the target. Check
frame 1 of every run actually shows what you think it does.

This is not a one-off. A later run's file listing sat 33px lower than the
reference positions written down an hour earlier, which made a correct click
look like it had selected the row below. Read the ruler in the run's own zoom
frame rather than trusting a layout you measured previously — the frames carry
their own ground truth, which is most of the point of putting rulers on them.

**4. Do not fit a curve to a handful of clicks.** A normalised-coordinate
theory was fitted here to 11 clicks — five of them the *same* point — against
16 rows at 21 px pitch, where almost any answer lands within 10 px of some row.
It survived one confirming trial and was wrong. Vary the target deliberately
and predict before you measure.

## Server-side settings worth checking

For an OpenAI-compatible endpoint:

- **`enable_thinking` is per-request, not a launch flag.** A model whose chat
  template suppresses the thought channel emits nothing however the server was
  started. `--thinking on` sends
  `chat_template_kwargs: {"enable_thinking": true}`. Default is to send no
  field at all and leave the server on its own behaviour.
- **The reasoning field may be `reasoning`, not `reasoning_content`.** On the
  vLLM build here `reasoning_content` is always null and the `gemma4` reasoning
  parser puts the thought channel in `reasoning`. Both names are read, and both
  are stripped from history before it is echoed back — an unrecognised field
  would otherwise grow the prompt every turn and break prompt-cache prefix
  matching.
- **Thinking moves the model's narrative out of the retained history.** With
  thinking off, a model's account of what it is doing goes into `content`,
  which stays in the conversation. With thinking on it goes into `reasoning`
  instead, and reasoning is stripped before history is echoed back — so a
  tool-calling turn leaves behind `{tool_calls: [...], content: null}` and a
  tool result reading "left-clicked at (780,660)". Observed consequence: the
  model completed an MSI install, clicked Finish, and then started the same
  install again, because nothing in its history said the job was done. The
  system prompt now tells it to state completed steps in its reply, and to
  check before repeating a substantial action. Raising `keep-recent-images`
  also helps and is cheap here — an image costs a flat ~280 tokens regardless
  of size, so the real cost is HTTP payload, not context. Both together fixed
  it: same task, same 7 clicks, stopped cleanly at Finish.

  This is a property of the *model*, not of thinking as such. Qwen3.6-27B
  returns `content`, `reasoning` and `tool_calls` together in one message, so
  its narrative lands in the field that is retained and the gap does not open.
  Check which shape a new model produces before assuming either behaviour.
- **Pan-and-scan** (`mm_processor_kwargs`) raises effective resolution on large
  images by tiling instead of downscaling once. Off by default.
- **Screen resolution** is the guest's, not the console's. A VM last opened in
  the vSphere web client keeps that browser's viewport — 1904x861 in one case.
  `--resolution 1280x1024` sets it through VMware Tools before connecting.

## Result

Gemma-4-31B completed an unattended MSI install end to end: located the
installer in a network share, launched it, accepted the EULA, advanced the
wizard and clicked Finish. The same task, run three times as settings changed:

| model | thinking | `keep-recent-images` | clicks | wall clock | outcome |
| --- | --- | --- | --- | --- | --- |
| Gemma-4-31B | off | 1 | 17 | ~7.5 min | installed, one stray click afterwards |
| Gemma-4-31B | **on** | 1 | 7 | — | installed, **then started installing again** |
| Gemma-4-31B | on | **3** + completion guidance | 7 | ~7 min | installed, stopped cleanly |
| Qwen3.6-27B | on (default) | 2 | 7 | ~7 min | installed, stopped cleanly, `--require-zoom on` |
| **gemini-3.6-flash** | on (default) | 2 | 9 | **82 s** | installed, stopped cleanly, **no gate needed** |
| **claude-opus-5** | on (default) | 2 | **6** | **44 s** | installed, stopped cleanly, **one zoom in the whole run** |
| **claude-sonnet-5** | on (default) | 2 | **6** | **47 s** | installed, stopped cleanly, one zoom |

Qwen needed the zoom gate to get there. Left to itself it guessed positions
from the full screenshot, missed, and only then zoomed; with the gate it
matched Gemma's best run step for step and put its last click 5px from the
centre of Finish.

Gemini needed none of it. It zoomed before nearly every click unprompted,
missed nothing, and finished in a fifth the wall-clock time of either local
model — two of its clicks came straight after another click with no zoom
between, and both were correct, which the gate would have refused.

Claude went further and is the most informative row in the table. It zoomed
**once** in the entire run, to tell `signalsql` from `SignalSQL-0.9.0-win64`,
and then hit the row, the licence label and three wizard buttons directly from
full screenshots without a miss. That is the aiming problem simply not
existing: the ruler, the grid, the region floors and the zoom gate are all
compensation for grounding that a strong model does not need. The one time it
did zoom was for *identity*, not aim — which is the other reason the tool
description gives for zooming, and the one that survives at every capability
level.

Sonnet 5 matched Opus 5 almost exactly — same six clicks, same single zoom,
within 3s on wall clock, and click coordinates differing by at most 8px on one
button. At roughly 40% of the cost that makes it the better default for
click-driven work; nothing in this task distinguished the two.

The same tools and the same system prompt serve the whole range. What changes
is whether the workflow has to be enforced, merely described, or is not needed
at all.

### Prompt caching survives image pruning

Only Claude caches here, and pruning rewrites history in place, so the two
could easily fight: a cache prefix is matched by exact prefix, and rewriting an
old image into a placeholder invalidates everything after it. The breakpoint
therefore sits before the mutable window. Measured across one ten-call run:

```
cache_read = 4528 → 4676 → 4978 → 5423 → 5620 → 5821 → 6006 → 6215
```

Monotonic, with `cache_write` only ever adding the new increment. `--trace`
logs a `usage:` line per call; if `cache_read` sits flat at 0 across steps, the
breakpoint has drifted back into the region pruning touches.

Thinking more than halved the clicks and held there. The reinstall in the
middle row is the history gap described under
[Server-side settings](#server-side-settings-worth-checking); retaining three
images and telling the model to state completed steps in its reply fixed it
without costing any of the efficiency.

Before the changes in this document, the same model could not reliably click a
single row in a file listing.
