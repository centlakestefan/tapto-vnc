# Clicking accurately with a local vision model

How tapto-vnc gets a model to put the pointer where it means to, why the
obvious approaches fail, and how to qualify a new model in about ten minutes.

Measured against **Gemma-4-31B (AWQ 4-bit, vLLM)** and **Qwen3.6-27B (NVFP4,
vLLM)** driving a Windows 10 guest at 1280x1024, with the model named wherever
the two differ — and they differ more than they look, starting with
[how much of the screen each one can see](#how-much-of-the-screen-reaches-the-model).
Numbers are from saved frames, not from what the model said it did — see
[Only the frames are evidence](#only-the-frames-are-evidence).

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

## How much of the screen reaches the model

A screenshot does not reach the model as pixels. It reaches it as a grid of
vision tokens, and how coarse that grid is decides what can be located at all.
**This is the single most model-specific number in this document**, and the two
families behave nothing alike.

**Fixed budget.** Gemma 4's processor sets `image_seq_length: 280`, so every
image becomes ~17x17 tokens no matter what it was. A whole screen and a
postage stamp cost the same and arrive at the same resolution.

**Area-proportional.** Qwen3-VL uses patch 16 with merge 2, so one token covers
a **32x32 px block** and the count is simply `pixels / 1024`, with the sides
snapped to multiples of 32. Nothing is downscaled until the image exceeds the
server's area ceiling.

What that means for the images this tool actually sends:

| image sent | | Gemma-4-31B | Qwen3.6-27B |
| --- | --- | --- | --- |
| full screen, 1280x1024 | tokens | 280 | **1280** |
| | screen px per cell | 75 x 60 | **32 x 32** |
| | 21 px list rows per cell | 2.9 | 1.5 |
| full screen + `--grid`, 1358x1050 | tokens | 280 | 1386 |
| zoom, 320x100 at 3x (1014x319) | tokens | 280 | 320 |
| | screen px per cell | 19 x 6 | 10.7 x 10.7 |
| | cells per 21 px list row | 3.5 | 2.0 |

Rows in a file listing are **21 px** apart. For Gemma at full screen one token
cell spans nearly three of them, so the information needed to pick a row is not
in the input at all — no prompting fixes that, and the fix is to send less
screen. Qwen sees the same screen **5.6x finer per axis**, which is enough that
the same argument does not carry: when it clicks the wrong row, that is a
different failure and needs a different remedy. Do not quote the 17x17 figure
at a model that does not have it.

Two consequences that also do not transfer:

- **Enlarging is free only under a fixed budget.** For Gemma, a 3x zoom costs
  the same 280 tokens as the raw crop, so there is no reason not to enlarge to
  the vision tower's native ~896 px side. For Qwen the same zoom costs **320
  tokens against about 30** — 10x — because tokens track area. It is still
  worth doing, but the mechanism is different and worth stating correctly:
  nearest-neighbour enlargement adds no information, yet it changes the
  *tokenisation*, so a 32x32 patch holding three rows of text becomes three
  patches holding one row each.
- **Only Gemma squashes the aspect ratio,** and it does not undo the squash.
  Its `do_resize` is on and pan-and-scan off, so a non-square screenshot is
  squeezed into a square rather than letterboxed — and the horizontal
  coordinates it reports come back compressed by exactly that squeeze. Give it
  a square screen, or pad one. See
  [Gemma needs a square screen](#gemma-needs-a-square-screen). Qwen preserves
  the aspect ratio and has no such problem.

### Finding the numbers for a model you have not measured

Do not infer them from the family name. The OpenAI-compatible provider logs
`usage:` on every call, so the cost of one image is the jump in `prompt_tokens`
between an otherwise identical call with and without it. For a patch-grid model
that divides straight back out to the grid: 320 tokens at 32x32 is 1014x319,
which is the zoom, unresized.

**Check the serving config too, not just the model.** The ceiling that decides
whether an image is downscaled is set at serve time and can silently be the
wrong knob — on this deployment `--mm-processor-kwargs '{"max_pixels": ...}'`
was accepted and ignored, the effective setting being `size.longest_edge`,
which is an *area* bound despite the name. A 12 MP test image tokenised
identically at three different `max_pixels` values, which is how the dead flag
was found. If a cap matters to you, verify it changes the token count.

For this tool the caps worth asking for are: zooms are 0.32 MP and safe under
anything, but full screenshots are 1.43 MP at 1280x1024 with ruler margins and
1.76 MP at 1904x861. A 1 MP ceiling silently shrinks those to 84% and 75%,
which lands on the ruler digits — the one part of the image drawn at 3x
precisely because that image is the one at risk. Ask for 2 MP.

## What the model actually does

### Gemma needs a square screen

The strongest result in this document, because it is the only one where a
prediction was made in advance, one variable was changed, and the effect
arrived — rather than a line fitted to points already collected.

Gemma squeezes a non-square screenshot into a square. The doc used to claim
that needed no correction, on the reasoning that the model answers in the
screenshot's own pixel coordinates and so undoes the squash on its side. Half
of that is true. It undoes it on **y** and not on **x**, which is why every
run showed x drifting while y stayed honest.

Asked repeatedly for the same close button on a 1280x1024 guest, it aimed its
zoom at almost the same wrong place every time:

| guest | image sent | aspect | target x | where it aimed | ratio | zoom contained the target |
| --- | --- | --- | --- | --- | --- | --- |
| 1280x1024 | 1358x1050 | 1.29 | 867 | 685, 688, 690, 690 | **0.79** | **0 of 4** |
| **1024x1024** | 1102x1050 | 1.05 | 852 | 840 | **0.99** | **first try** |

The reciprocal of the aspect ratio predicts 0.77 and 0.95; the measurements are
0.79 and 0.99. Four repeats at 4:3 rule out noise — the model was pointing at
0.79x the truth consistently, and a 320px-wide zoom cannot rescue a 180px
error, so it zoomed, found the *maximise* button instead, and reported that.

The likely mechanism is that the model reasons in the resized square frame and
scales back by a single factor, the way an aspect-preserving pipeline would
require. Squashing violates that assumption on one axis only.

**So: run Gemma against a square guest** (`--resolution 1024x1024`), or pad the
screenshot to square before sending it. Pad on the right and bottom, so the
origin does not move and every coordinate the model returns is still a screen
coordinate. The earlier advice against padding — that it wastes ~20% of the
budget on black bars — had the trade backwards: 20% of effective resolution is
cheap against a 21% systematic error on an axis, and for a fixed-budget model
the *token* cost of padding is zero.

None of this applies to Qwen, which preserves aspect ratio.

### Verify with the pointer, not by asking

The measurement above is trustworthy because the guest confirmed it. Told to
move the mouse to its answer and look, the model reported the close button had
turned red — and the hover highlight is Windows asserting which control the
pointer is over, in the guest's own words rather than the model's.

That matters because **this model agrees with whatever it is told**. In one
session it justified the close button being between gridlines 800 and 850,
was asked "are you positive?", produced more supporting detail, and then
switched to 850-900 the moment the operator suggested it — with an equally
confident account of re-examining the screenshot both times. Asking a model
whether it saw something invites yes; asking it to describe a distinguishing
feature ("what shape is the symbol?") made it look again and find the error
itself.

`vnc_move` exists for this and no autonomous run has ever reached for it
unprompted. For anything irreversible it is the cheapest check available: move,
look for the highlight, then click. `--move-first on` asks for it on every
click — see "Move before clicking" below for what that costs and what is still
unmeasured about it.

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

The same reasoning applies to a line that carries no number. Where ticks fall
too close together to label every one, the unlabelled ones are drawn at half
strength — otherwise finding "the line after 300" means counting identical
lines, and a miscount is a whole grid step of error. Only a numbered line is
drawn at full strength.

## Design rules, and where they live in the code

| rule | enforced by |
| --- | --- |
| Grid resolution equal on both axes | `minorStep` in `screenshotRegionPng()` |
| Only a numbered line is drawn at full strength | `labelled ? 2 : 1` blend weight |
| Every zoom is 320x100 at 3x | `kZoomWidth` / `kZoomHeight` / `kZoomScale` |
| A zoom is centred on the point asked for | `doZoom()` |
| At a screen edge the view slides, it does not shrink | `doZoom()` |
| One coordinate system everywhere | rulers in `screenshotRegionPng()`; there is no second one |
| Click must follow a zoom containing it | `--require-zoom on`, off by default |

None of this is requested; all of it is enforced. The tool description asked
for short regions and a real run chose 400, then 300, then 200 and missed with
all three before succeeding at 100. Guidance a model can decline is not a
mechanism.

## Why the zoom has no size

`vnc_zoom` takes a point and nothing else. It used to take a rectangle, with
floors at 240x80, caps at 640x200, and a magnification derived from whichever
side was longest.

The freedom went unused. Across one 17-zoom run the model asked for eleven
different rectangles, converged on roughly 300x80 by itself after the first
call, and spent four calls being clamped up from something too small to contain
what it was aiming at. One earlier run asked repeatedly for 200x64 regions that
did not reach the button, then clicked inside them anyway.

What the freedom cost was a **ruler whose spacing changed with every call**.
`niceStep()` runs on whatever rectangle arrives, so a 240x74 view is gridded
every 25 px across and 10 px down while a 320x100 view is gridded every 10 px
on both — and 25 and 10 do not divide, so that first one draws an irregular
ladder of gaps 10, 10, 5, 5. Asked to read it, Qwen3.6-27B reported a 48 px
Start button as 80-90 px wide and misread the y ruler's 10 px labels as 20 px.
Its own reasoning shows it trying to reconcile the pitch it was told about with
the pitch in front of it.

Fixed, the ruler is identical in every zoom the model will ever see: **lines
every 10 px on both axes**, x labelled every 50, y every 10, image always
1014x319. One pitch to learn instead of one to re-derive per call.

The centre semantics follow from the fixed size. Naming a corner means
constructing a rectangle around a target, which is the arithmetic that produced
the regions that missed; naming a point is just pointing. At a screen edge the
view slides back inside rather than shrinking, because a shrunken region would
be gridded differently and the constant pitch is the whole point.

**What this gives up** is the survey zoom — one 640x150 call that read a
17-row file listing in one go. Scanning a list now takes three or four calls.
Watch for identification errors rather than aiming errors; if they appear, a
second wider mode is the knob to add back, and only then.

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

## Rulers on the full screenshot — open

`--grid <px>` (config key `screenshot-grid`, `0` by default) puts the same
rulers and grid on the ordinary screenshots every tool returns, at a fixed
spacing rather than one chosen from the region size. `--grid 50` is the
spacing worth trying first.

It is off by default because it is not yet known to help, and it is not
obvious either way:

- **For it.** The zoom's rulers work, and nothing about the mechanism is
  specific to an enlarged image. Some misses are not identification failures at
  all — the model can see the button perfectly well and still reports a
  coordinate 80 px off. A ruler fixes that class directly, and on the frame the
  model already has, without a round trip.
- **Against it.** A full screenshot is where the vision tower's downscaling
  bites hardest. A 1904 px-wide screen is resized to about 896, so the labels
  are least legible exactly where the picture is least detailed — which is why
  the digits are drawn 3x rather than 2x at 1:1 (`rulerGlyphScale`). And a
  grid at 50 px crosses every piece of text on the screen; the lines are
  half-blended so text survives them at full resolution, but "survives at full
  resolution" is not the condition that matters.

**Look at it before running it.** `vnc-smoketest --grid 50` writes the same
render with no model involved:

```sh
vnc-smoketest --vcenter <host> --vm <name> --grid 50 --out grid.png
```

Open it, then shrink it to 896 px wide and look again — that second view is
closer to what the model gets.

**What would settle it.** The placement test below, run twice on the same
target with `--grid 50` and without, comparing the error of a click made from
the full screenshot alone. If gridding helps, the interesting follow-up is
whether it reduces how often a model needs to zoom at all, which is where the
wall-clock saving would be.

**What it cannot do.** A grid adds no detail. It tells the model where
something is, not what it is — so it does nothing for picking one row of a file
list out of five, and the zoom-to-identify rule stands unchanged. Both the
system prompt and the tool description say so when the grid is on, because a
model handed a measuring instrument will otherwise assume it has been handed a
better picture.

## Move before clicking — open

`--move-first on` (config key `move-first`, off by default) adds four
paragraphs to the system prompt asking the model to `vnc_move` to a point, look
at the screenshot that comes back, and only then click the same point.

It is prompt only. Nothing refuses a click that skipped the move, which is the
difference from the zoom gate above and the reason the flag is not called
`--require-move`. The mechanism would be the same shape — a flag cleared by
every action, a check that the click matches the move — and it is deliberately
not built yet, because a rule the model follows because it was asked is cheap
to withdraw and a rule enforced in code is not.

If a model turns up that needs it enforced, the gate arrives as a third value
here — `--move-first force` — rather than as a separate `--require-move`. Off,
asked and enforced are three settings of one idea, and on one switch they
cannot be set to contradict each other.

**Why it might be worth a step.** A hover reaction is the *guest* saying what
is under the pointer: a button lightens, a row highlights, a close button turns
red. That is a different kind of evidence from the model's reading of a
picture, and it is the kind that cannot be a misjudged position — see "Verify
with the pointer, not by asking" above, where the model reported the close
button turning red and the highlight sat at 829..874 against an answer of 855.
The asymmetry pays for the round trip: a wrong move costs nothing, while a
wrong click has opened an installer for the wrong file, dismissed a dialog and
pushed a wizard backwards in earlier runs, each of those five steps or more to
undo.

The prompt also covers the case with no reaction — a desktop icon, a text
field, empty space — where the check is the pointer itself, since the server
draws it into the framebuffer. And it says that a menu opening on hover is
information rather than a mistake, because a model told to be careful will
otherwise treat a screen that changed under it as its own error.

**Why it is off.** One measurement is not a result. The cost is certain — a
step and a screenshot per click, on top of the zoom the same click already
needs — and the benefit is a class of error that the zoom gate already reduces.
The models it should help most are the local ones, which are also the ones for
which an extra round trip hurts most.

**What would settle it.** The same task run twice on one model with the flag on
and off, counting clicks that landed on the wrong control rather than clicks
that missed by pixels: the hover check is evidence about *what* is under the
pointer, so a wrong-row click is what it should catch and a 20 px miss is not.

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

Run with `--screenshots <dir>`. A click writes **two** frames: the screen as it
was, with a **red dot** at the aim point, and then the screen the click
produced. Both are files on disk only — the model is never shown the dot, since
showing it where its last shot landed would feed back into the behaviour being
measured.

The pairing is the point. A dot on the *result* answers a question nobody
asked: by then the menu has opened or the window has closed, and the button the
click was aiming at may not be on the screen any more. The dot belongs on the
picture the model was looking at when it chose the coordinate, because that is
the screen the miss happened on — the button is still there, 60 px above the
dot, and the error is visible instead of inferred. Drag frames mark the point
it grabs, for the same reason.

The "before" frame is rendered from the composite as it already stands, without
capturing first: no round trip, no settle, and — more importantly — no risk of
showing the dot against a screen the model never reasoned about.

Filenames record the geometry, so a run can be reconstructed arithmetically
without opening anything:

```
0036-zoomed-3x-on-320x100-at-520-620.png     region (520,620) 320x100 at 3x
0037-about-to-left-click-750-660.png         where it aimed, on the screen it saw
0038-left-clicked-at-750-660.png             what that click produced
```

Alongside them, `frames.jsonl` carries one object per frame — sequence, file,
milliseconds since the first frame, `kind` (`full` or `zoom`), `phase`, label,
image size, and the aim point where there is one. It exists because the things
a later pass needs are the things a filename cannot carry: zooms are a
different size from full screens, so they cannot join a video track unaltered,
and every frame stands for a stretch of real time that runs from 200 ms to
several minutes of the model thinking.

The zoom filename records the region's **top-left corner**, not the point the
model asked for — that is what was rendered, and it is what the error has to be
measured against. Add (160, 50) to recover the request, except where the view
slid off a screen edge.

## Qualifying a new model

Ten minutes, four runs, before trusting anything to a long task.

**Set up.** `--screenshots` to a fresh directory, and a stable screen with a
known target. A file listing is
ideal: rows are evenly spaced and their positions are easy to establish with
`vnc-smoketest --zoom`.

**1. Placement test.** Pick one target. Run three separate tasks, each naming
an exact zoom point, chosen so the target sits at roughly 0.05, 0.5 and 0.9 of
the region's height. The view is 320x100 centred on the point given, so for a
target at screen y=231 those three are `y = 231 + 44`, `231`, and `231 - 40`.
Instruct the model explicitly:

```
Call vnc_zoom with exactly these arguments: x=310, y=275.
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
read Y off the ruler and X in image pixels. Move the centre horizontally so the
target sits at 0.05, 0.5 and 0.9 of the region's width.

**2b. Shift the region's origin.** Run the same target twice, once with the
view centred so the region starts at `x=0` and once at `x=150`. If the answer
moves with the image rather than with the screen, the model is reporting image
pixels — see [It may read one axis off
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
