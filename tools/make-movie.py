#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Centlake Software AB
"""Turn a --screenshots directory into a movie of the run.

The frames tapto-vnc writes are lossless PNGs of a desktop, and they arrive at
wildly uneven times: half a second between a click and its result, a quarter of
an hour while the model thinks, and occasionally two hours because somebody
went to lunch with the prompt open. Three things follow, and they are what this
script is.

Encoding. H.264 in mp4, because a desktop that sits still between actions costs
a video codec almost nothing and the result plays everywhere with nothing
installed. It is 4:2:0, so coloured subpixel fringes on small text soften; this
is a movie for watching a run back, not for reading a file listing out of. The
frames themselves are still there for that.

Pacing. Each frame is held for the time until the next one actually arrived,
clamped at both ends. Without a ceiling the lunch break plays in real time;
without a floor the click pairs flash past unseen. The clamps are the whole
editorial policy, and they are flags because the right values are a matter of
taste.

Words. Frames alone are a machine being operated by nobody. frames.jsonl also
carries what the run was asked to do and what the model was thinking, so those
go in a panel beside the screen — beside rather than under, because reasoning
is paragraphs and a strip under a 1280-wide screen holds about four lines. The
panel is drawn here and never into the saved PNGs: those are the record of what
the guest actually displayed, and captions burnt into them would make them
useless as evidence and freeze one layout choice forever.

Needs PyAV and Pillow (see tools/requirements.txt):
    pip install -r tools/requirements.txt
"""

import argparse
import json
import re
import sys
import time
from fractions import Fraction
from pathlib import Path

try:
    import av
    from PIL import Image, ImageDraw, ImageFont
except ImportError as problem:
    sys.exit(
        f"This needs PyAV and Pillow ({problem}).\n"
        "    pip install -r tools/requirements.txt\n"
        "PyAV carries its own FFmpeg, so nothing else has to be installed."
    )

# Milliseconds, so a frame's timestamp is the number in frames.jsonl with the
# run's start subtracted — nothing to convert when reading a movie against its
# index.
TIME_BASE = Fraction(1, 1000)

# The panel. Dark, because it sits next to a bright desktop for minutes at a
# time and a white column beside it is punishing to watch.
PANEL_BG = (18, 18, 20)
PANEL_RULE = (58, 58, 64)
LABEL_FG = (128, 128, 138)      # the small capitals naming each section
ACTION_FG = (240, 190, 120)     # what it just did, in the accent colour
AIM_FG = (235, 120, 120)        # the same red as the dot in the frame

# The four kinds of writing the index records, and how each is shown.
#
# They are different acts and the panel should not flatten them. A prompt is an
# instruction from a person and stands for the whole turn, so it is the
# brightest thing here and set in bold. A reply is the model's answer, and gets
# the accent colour because it is the one thing that concludes something.
# Reasoning is the model talking to itself: dimmest, and italic where a face is
# available, because it is not addressed to anyone. Prose said in passing sits
# between the two.
STYLES = {
    "prompt": {"title": "TASK",     "fill": (235, 235, 240), "face": "bold",    "lines": 8},
    "reply":  {"title": "REPLY",    "fill": (150, 210, 160), "face": "regular", "lines": 20},
    "say":    {"title": "SAYS",     "fill": (214, 218, 228), "face": "regular", "lines": 20},
    "think":  {"title": "THINKING", "fill": (150, 156, 172), "face": "italic",  "lines": 22},
}

FONT_CANDIDATES = [
    # Windows, Linux, macOS. First set whose regular face exists wins; the
    # bitmap default is the last resort and looks it.
    ("C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/segoeuib.ttf",
     "C:/Windows/Fonts/segoeuii.ttf"),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
     "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
     "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf"),
    ("/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
     "/usr/share/fonts/TTF/DejaVuSans-Oblique.ttf"),
    ("/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Helvetica.ttc",
     "/System/Library/Fonts/Helvetica.ttc"),
]


def load_fonts(size):
    """regular, bold, italic and a smaller face for the section labels.

    A missing bold or italic falls back to the regular face rather than to
    another family: the styles here carry meaning, but a panel set in two
    unrelated typefaces is worse than one that repeats itself.
    """
    for regular, bold, italic in FONT_CANDIDATES:
        if Path(regular).exists():
            try:
                def face(path):
                    return ImageFont.truetype(path if Path(path).exists() else regular, size)
                return {
                    "regular": ImageFont.truetype(regular, size),
                    "bold": face(bold),
                    "italic": face(italic),
                    "label": ImageFont.truetype(regular, max(11, int(size * 0.72))),
                }
            except OSError:
                continue
    default = ImageFont.load_default()
    return {"regular": default, "bold": default, "italic": default, "label": default}


def read_index(path):
    """The frames, and the words that were spoken around them."""
    frames, sessions, words = [], [], []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue          # a half-written last line from a killed run
            event = record.get("event")
            if event == "start":
                sessions.append(record)
            elif event in ("prompt", "reply", "say", "think"):
                words.append(record)
            elif "seq" in record:
                frames.append(record)
    frames.sort(key=lambda f: f["seq"])
    words.sort(key=lambda w: w["epoch_ms"])
    return frames, sessions, words


def attach_words(frames, words):
    """For each frame, the prompt it is working under and the last thing said.

    Both are 'the most recent one at or before this frame', which is what a
    viewer would infer anyway: the task stays up for the whole turn, and the
    commentary is whatever the model last said on the way to this picture.

    The commentary keeps its kind, because reasoning, prose and a final reply
    are different acts and the panel shows them differently.

    A reply is the exception to "at or before". It is written after the turn's
    last action, so under that rule no frame would ever carry one and the REPLY
    style would be unreachable — which is exactly what the first run with text
    showed. But a reply describes the state the turn ended in, and that state is
    the picture in the last frame, so it belongs there: any reply arriving
    before the next frame is shown on this one.
    """
    attached = []
    prompt = None
    commentary = None                  # (kind, text)
    position = 0
    for index, frame in enumerate(frames):
        while position < len(words) and words[position]["epoch_ms"] <= frame["epoch_ms"]:
            word = words[position]
            if word["event"] == "prompt":
                prompt = word["text"]
                commentary = None      # a new task; the old thinking is stale
            else:
                commentary = (word["event"], word["text"])
            position += 1

        # Look ahead for the turn's conclusion, but only for a reply: the
        # reasoning that comes after this frame belongs to the next action, not
        # to this picture.
        horizon = frames[index + 1]["epoch_ms"] if index + 1 < len(frames) else float("inf")
        for word in words[position:]:
            if word["epoch_ms"] >= horizon:
                break
            if word["event"] == "reply":
                commentary = ("reply", word["text"])
                break

        attached.append((prompt, commentary))
    return attached


def plan_durations(frames, args):
    """How long each frame stays on screen, in milliseconds."""
    holds = []
    for index, frame in enumerate(frames):
        if index < len(frames) - 1:
            gap = frames[index + 1]["epoch_ms"] - frame["epoch_ms"]
        else:
            gap = args.hold_ms
        held = max(args.min_ms, min(args.max_ms, gap))

        # The one frame that is not a record of elapsed time. An aim frame
        # exists to be looked at, and the real gap to the click it precedes is
        # a few hundred milliseconds — not long enough to find a red dot on a
        # screen you have never seen before.
        if frame.get("phase") == "before":
            held = max(held, args.aim_ms)
        holds.append(held)
    return holds


# The model writes for a terminal, and terminals get markdown: **Next**, *this*,
# `that`. The panel has real bold and italic faces and uses them to say what
# kind of writing a passage is, so the asterisks would be both noise and a
# second, contradictory emphasis system. Stripped rather than rendered — this is
# a caption, not a document, and a bold word inside an italic reasoning block
# would undo the one distinction the styling is making.
MARKDOWN = re.compile(r"\*\*(.+?)\*\*|\*(.+?)\*|`(.+?)`|__(.+?)__", re.DOTALL)


def plain(text):
    if not text:
        return text
    return MARKDOWN.sub(lambda m: next(g for g in m.groups() if g is not None), text)


def wrap(text, font, width, draw):
    """Greedy wrap, measured in the font rather than counted in characters."""
    lines = []
    for paragraph in text.splitlines():
        if not paragraph.strip():
            lines.append("")
            continue
        line = ""
        for word in paragraph.split():
            candidate = f"{line} {word}".strip()
            if draw.textlength(candidate, font=font) <= width or not line:
                line = candidate
            else:
                lines.append(line)
                line = word
        lines.append(line)
    return lines


class Panel:
    """Draws the left-hand column: the task, the thinking, and the action."""

    def __init__(self, width, height, fonts, started_ms):
        self.width, self.height = width, height
        self.fonts = fonts
        self.body = fonts["regular"]
        self.small = fonts["label"]
        self.started_ms = started_ms
        self.pad = max(14, width // 24)

    def _section(self, draw, y, kind, text):
        """One block of writing, styled by what kind of writing it is."""
        if not text:
            return y
        style = STYLES[kind]
        font = self.fonts[style["face"]]

        draw.text((self.pad, y), style["title"], font=self.small, fill=LABEL_FG)
        y += int(self.small.size * 1.9)

        lines = wrap(plain(text), font, self.width - 2 * self.pad, draw)
        clipped = len(lines) > style["lines"]
        for line in lines[:style["lines"]]:
            draw.text((self.pad, y), line, font=font, fill=style["fill"])
            y += int(font.size * 1.42)
        if clipped:
            draw.text((self.pad, y), "…", font=font, fill=LABEL_FG)
            y += int(font.size * 1.42)
        return y + int(font.size * 0.9)

    def render(self, frame, prompt, commentary):
        image = Image.new("RGB", (self.width, self.height), PANEL_BG)
        draw = ImageDraw.Draw(image)
        draw.line([(self.width - 1, 0), (self.width - 1, self.height)], fill=PANEL_RULE)

        elapsed = (frame["epoch_ms"] - self.started_ms) / 1000
        clock = f"{int(elapsed // 3600)}:{int(elapsed // 60) % 60:02d}:{int(elapsed) % 60:02d}"
        draw.text((self.pad, self.pad), f"#{frame['seq']}", font=self.small, fill=LABEL_FG)
        draw.text((self.width - self.pad - draw.textlength(clock, font=self.small), self.pad),
                  clock, font=self.small, fill=LABEL_FG)

        y = self.pad + int(self.small.size * 3.0)
        y = self._section(draw, y, "prompt", prompt)
        if commentary:
            kind, text = commentary
            y = self._section(draw, y, kind, text)

        # The action goes at the foot rather than in sequence: it is a caption
        # for the picture beside it, and it is the one line that changes on
        # every single frame.
        label = frame.get("label", "")
        colour = AIM_FG if frame.get("phase") == "before" else ACTION_FG
        lines = wrap(label, self.body, self.width - 2 * self.pad, draw)[:3]
        y = self.height - self.pad - int(self.body.size * 1.42) * len(lines)
        draw.line([(self.pad, y - self.pad), (self.width - self.pad, y - self.pad)],
                  fill=PANEL_RULE)
        for line in lines:
            draw.text((self.pad, y), line, font=self.body, fill=colour)
            y += int(self.body.size * 1.42)
        return image


def compose(shot, panel_image, screen_size, panel_side):
    """Screen and panel on one canvas, the screen letterboxed into its half.

    A zoom is a different shape from the screen, so it is scaled to fit and
    centred rather than stretched: it is the model looking closely at what it is
    about to click, and a distorted one would misrepresent where it looked.
    """
    screen_w, screen_h = screen_size
    panel_w = panel_image.width if panel_image else 0
    canvas = Image.new("RGB", (panel_w + screen_w, screen_h), (0, 0, 0))

    if shot.size != screen_size:
        scale = min(screen_w / shot.width, screen_h / shot.height)
        shot = shot.resize((max(1, int(shot.width * scale)), max(1, int(shot.height * scale))),
                           Image.LANCZOS)

    screen_x = panel_w if panel_side == "left" else 0
    canvas.paste(shot, (screen_x + (screen_w - shot.width) // 2,
                        (screen_h - shot.height) // 2))
    if panel_image:
        canvas.paste(panel_image, (0 if panel_side == "left" else screen_w, 0))
    return canvas


def main():
    parser = argparse.ArgumentParser(
        description="Assemble a --screenshots directory into an mp4.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("directory", nargs="?", default=".",
                        help="a directory written by tapto-vnc --screenshots")
    parser.add_argument("-o", "--out", default="movie.mp4",
                        help="output file, relative to the directory")
    parser.add_argument("--crf", type=int, default=23,
                        help="quality: lower is better and bigger, 18 is near-transparent")
    parser.add_argument("--preset", default="slow",
                        help="x264 speed/size tradeoff: ultrafast..veryslow")
    parser.add_argument("--width", type=int, default=0,
                        help="scale the whole canvas to this width, 0 for native")
    parser.add_argument("--panel", choices=("left", "right", "off"), default="left",
                        help="where the task and the model's thinking are drawn")
    parser.add_argument("--panel-width", type=int, default=0,
                        help="panel width in pixels, 0 to size it from the screen")
    parser.add_argument("--font-size", type=int, default=0,
                        help="panel body text size, 0 to size it from the panel")
    parser.add_argument("--min-ms", type=int, default=400,
                        help="shortest a frame may be held")
    parser.add_argument("--max-ms", type=int, default=2500,
                        help="longest a frame may be held; this is what makes idle watchable")
    parser.add_argument("--aim-ms", type=int, default=900,
                        help="minimum hold on an 'about to click' frame")
    parser.add_argument("--hold-ms", type=int, default=3000,
                        help="how long the final frame stays up")
    parser.add_argument("--read-cps", type=int, default=22,
                        help="reading speed in characters per second, for the beat that shows"
                             " new text against the screen it was written about. 0 disables"
                             " the split")
    parser.add_argument("--read-min-ms", type=int, default=1400,
                        help="shortest reading beat")
    parser.add_argument("--read-max-ms", type=int, default=7000,
                        help="longest reading beat, however much was written")
    parser.add_argument("--fps", type=int, default=12,
                        help="constant frame rate; each still is repeated to fill its time")
    parser.add_argument("--vfr", action="store_true",
                        help="one sample per still with a long duration instead. Smaller, and"
                             " some players show black until several samples have arrived")
    parser.add_argument("--zooms", choices=("fit", "skip"), default="fit",
                        help="what to do with zoom frames, which are not screen-sized")
    parser.add_argument("--list", action="store_true",
                        help="print the cut — every beat, when it starts and why — and stop."
                             " Pacing is guesswork until you can see it laid out")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    directory = Path(args.directory).resolve()
    index = directory / "frames.jsonl"
    if not index.exists():
        sys.exit(f"no frames.jsonl in {directory}\n"
                 "Run tapto-vnc with --screenshots <dir> first.")

    frames, sessions, words = read_index(index)
    if not frames:
        sys.exit("frames.jsonl lists no frames")

    # The commonest size rather than the first frame's: a run that was resized
    # part way through should still be a movie of whatever it spent its time at.
    sizes = {}
    for frame in frames:
        key = (frame["width"], frame["height"])
        sizes[key] = sizes.get(key, 0) + 1
    screen_w, screen_h = max(sizes, key=sizes.get)

    panel_w = 0
    if args.panel != "off":
        panel_w = args.panel_width or max(320, int(screen_w * 0.32))
    font_size = args.font_size or max(15, int(panel_w / 24)) if panel_w else 15
    fonts = load_fonts(font_size)

    canvas_w, canvas_h = screen_w + panel_w, screen_h
    if args.width:
        scale = args.width / canvas_w
        out_w, out_h = args.width, round(canvas_h * scale)
    else:
        out_w, out_h = canvas_w, canvas_h
    # 4:2:0 stores chroma at half resolution in each axis, so both have to be
    # even or the encoder has nowhere to put the last row.
    out_w -= out_w % 2
    out_h -= out_h % 2

    holds = plan_durations(frames, args)
    attached = attach_words(frames, words)

    chosen = []
    for frame, held, (prompt, commentary) in zip(frames, holds, attached):
        path = directory / frame["file"]
        if not path.exists():
            continue
        if args.zooms == "skip" and (frame["width"], frame["height"]) != (screen_w, screen_h):
            continue
        chosen.append((frame, path, held, prompt, commentary))
    if not chosen:
        sys.exit("no usable frames")

    # One change per beat.
    #
    # A frame that arrives with new words in the panel changes two things at
    # once, and a viewer can attend to one: read the panel and the screen has
    # already moved on, watch the screen and the reasoning went past unread.
    # Holding the frame longer does not help, because it holds both.
    #
    # So the beat is split. First the new text against the screen as it still
    # was — nothing moves, and this is the beat that is read. Then the screen
    # changes with the text standing still — nothing new to read, and this is
    # the beat that is watched. The reading beat repeats the previous
    # screenshot, which is why it is nearly free to encode: only the panel
    # differs from the frame before it.
    #
    # Its length comes from how much there is to read rather than from the
    # clock, since it is the one thing in this movie that is not a record of
    # elapsed time.
    def reading_ms(text):
        if not args.read_cps or not text:
            return 0
        return max(args.read_min_ms,
                   min(args.read_max_ms, int(1000 * len(text) / args.read_cps)))

    beats = []                 # (path, frame, held, prompt, commentary)
    previous_path = None
    previous_text = (None, None)
    for frame, path, held, prompt, commentary in chosen:
        panel_text = (prompt, commentary[1] if commentary else None)
        if args.read_cps and previous_path and panel_text != previous_text:
            # Only what is actually new has to be read: a fresh task is read in
            # full, but when the task is unchanged the reading beat is paced by
            # the new commentary alone.
            fresh = (commentary[1] if commentary else "") if prompt == previous_text[0] \
                else "".join(part for part in panel_text if part)
            pause = reading_ms(fresh)
            if pause:
                beats.append((previous_path, frame, pause, prompt, commentary))
        beats.append((path, frame, held, prompt, commentary))
        previous_path, previous_text = path, panel_text

    if args.list:
        at = 0
        for path, frame, held, prompt, commentary in beats:
            reading = path != directory / frame["file"]
            kind = "read " if reading else "watch"
            said = ""
            if commentary:
                said = f"  [{commentary[0]}] {plain(commentary[1])[:60].replace(chr(10), ' ')}"
            print(f"{at / 1000:7.1f}s {kind} {held / 1000:4.1f}s  "
                  f"#{frame['seq']:<4} {frame['label'][:40]:<40}{said}")
            at += held
        print(f"\n{len(beats)} beats, {at / 60000:.1f} min")
        return

    panel = Panel(panel_w, screen_h, fonts, frames[0]["epoch_ms"]) if panel_w else None

    out_path = directory / args.out
    container = av.open(str(out_path), mode="w", options={"movflags": "+faststart"})
    stream = container.add_stream("libx264", rate=Fraction(args.fps, 1))
    stream.width, stream.height = out_w, out_h
    stream.pix_fmt = "yuv420p"
    stream.codec_context.time_base = TIME_BASE
    # tune=stillimage is x264's setting for exactly this material: long stretches
    # of an unchanging desktop, where the default psychovisual tuning spends bits
    # on grain that is not there.
    #
    # bf=0 disables B-frames, and that one is not about size. A codec that
    # reorders frames reports a delay, and the muxer compensates by writing an
    # edit list that skips it. Measured in frames that delay is invisible; here
    # a frame is often 2.5 seconds, so a two-frame delay became a five-second
    # edit at the head of the file and players opened the movie on black. No
    # reordering, no delay, no edit list.
    stream.options = {
        "crf": str(args.crf),
        "preset": args.preset,
        "tune": "stillimage",
        "bf": "0",
    }

    pts = 0
    written = 0
    fitted = 0
    samples = 0
    started = time.monotonic()
    canvas = None

    # A still that stays on screen for two and a half seconds can be one sample
    # two and a half seconds long, or thirty samples of a twelfth of a second
    # each. The file is smaller the first way and it is what this did at first,
    # but players disagree about when to present a sample whose successor has
    # not arrived: two of them opened the movie on black until several samples
    # were in — four seconds on one run, fourteen on another, both exactly the
    # sum of the first few durations.
    #
    # So the stills are repeated instead. x264 encodes a frame identical to its
    # predecessor as almost nothing, so the cost is nowhere near the thirty-fold
    # the sample count suggests — measured at about two thirds larger on a real
    # run, 1.37MB against 0.83MB. That is the price of the movie playing
    # everywhere; --vfr buys the bytes back for a file you know your own player
    # will handle.
    step = 1000 // args.fps

    def emit(image, at, hold_ms):
        nonlocal samples
        frame = av.VideoFrame.from_image(image)
        repeats = 1 if args.vfr else max(1, round(hold_ms / step))
        for repeat in range(repeats):
            frame.pts = at + (repeat * step if not args.vfr else 0)
            frame.time_base = TIME_BASE
            for packet in stream.encode(frame):
                container.mux(packet)
            samples += 1
        return at + (hold_ms if args.vfr else repeats * step)

    try:
        for path, frame, held, prompt, commentary in beats:
            with Image.open(path) as opened:
                shot = opened.convert("RGB")
            if shot.size != (screen_w, screen_h):
                fitted += 1

            panel_image = panel.render(frame, prompt, commentary) if panel else None
            canvas = compose(shot, panel_image, (screen_w, screen_h), args.panel)
            if (canvas.width, canvas.height) != (out_w, out_h):
                canvas = canvas.resize((out_w, out_h), Image.LANCZOS)

            pts = emit(canvas, pts, held)
            written += 1
            if not args.quiet and written % 25 == 0:
                print(f"  {written}/{len(beats)} beats")

        # A sample's duration is implied by the next sample's timestamp, so the
        # last one has none: without this the movie cuts the moment the final
        # picture appears. Holding it gives the ending something to sit on.
        if canvas is not None:
            pts = emit(canvas, pts, args.hold_ms)

        for packet in stream.encode(None):
            container.mux(packet)
    finally:
        container.close()

    if not args.quiet:
        real_ms = frames[-1]["epoch_ms"] - frames[0]["epoch_ms"]
        print()
        print(f"wrote {out_path}")
        reading = len(beats) - len(chosen)
        print(f"  frames    {len(chosen)} of {len(frames)} ({fitted} fitted from another size)")
        print(f"  beats     {written}, of which {reading} are reading beats"
              + ("" if args.vfr else f"; {samples} samples at {args.fps} fps"))
        print(f"  plays in  {pts / 60000:.1f} min, from {real_ms / 60000:.1f} min of real time")
        print(f"  size      {out_path.stat().st_size / 1e6:.1f} MB at {out_w}x{out_h},"
              f" crf {args.crf}, encoded in {time.monotonic() - started:.0f}s")
        if panel:
            spoken = sum(1 for w in words if w["event"] in ("think", "say", "reply"))
            print(f"  panel     {len(set(w['text'] for w in words if w['event'] == 'prompt'))}"
                  f" prompt(s), {spoken} passage(s) of the model talking")
            if not words:
                print("            (nothing — this run predates the index carrying text)")
        if len(sessions) > 1:
            print(f"  note      {len(sessions)} sessions in this directory;"
                  " the gaps between them are clamped like any other")


if __name__ == "__main__":
    main()
