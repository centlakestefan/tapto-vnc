#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Centlake Software AB
"""Turn a --screenshots directory into a movie of the run.

The frames tapto-vnc writes are lossless PNGs of a desktop, 44MB for a
four-hour session, and they arrive at wildly uneven times: half a second
between a click and its result, a quarter of an hour while the model thinks,
and occasionally two hours because somebody went to lunch with the prompt open.
Three things follow from that, and they are what this script is.

Encoding. H.264 in mp4, because a desktop that sits still between actions costs
a video codec almost nothing -- the same 44MB comes out around 3MB -- and
because the result plays everywhere without anything installed. It is 4:2:0, so
coloured subpixel fringes on small text soften; this is a movie for watching a
run back, not for reading a file listing out of. Use the frames themselves for
that, they are still there.

Pacing. Each frame is held for the time until the next one actually arrived,
clamped at both ends. Without a ceiling the lunch break plays in real time;
without a floor the click pairs flash past unseen. The clamps are the whole
editorial policy and they are flags, because the right values are a matter of
taste and change with what you are looking for.

Fitting. A zoom is 1014x319 and the screen is whatever the guest is, so zooms
are scaled to fit and centred on black rather than stretched. They are worth
keeping: a zoom is the model looking closely at the thing it is about to click,
and dropping it removes the reason for the click that follows.

Needs PyAV (see tools/requirements.txt):
    pip install -r tools/requirements.txt
"""

import argparse
import json
import sys
from fractions import Fraction
from pathlib import Path

try:
    import av
except ImportError:
    sys.exit(
        "This needs PyAV.\n"
        "    pip install -r tools/requirements.txt\n"
        "PyAV carries its own FFmpeg, so nothing else has to be installed."
    )

# Milliseconds, so a frame's timestamp is the number in frames.jsonl with the
# run's start subtracted -- nothing to convert when reading a movie against its
# index.
TIME_BASE = Fraction(1, 1000)


def read_index(path):
    """The frames and the session markers, in the order they were written."""
    frames, sessions = [], []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            if record.get("event") == "start":
                sessions.append(record)
            elif "seq" in record:
                frames.append(record)
    frames.sort(key=lambda f: f["seq"])
    return frames, sessions


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
        # a few hundred milliseconds -- not long enough to find a red dot on a
        # screen you have never seen before.
        if frame.get("phase") == "before":
            held = max(held, args.aim_ms)
        holds.append(held)
    return holds


def decode_png(path):
    with av.open(str(path)) as container:
        for frame in container.decode(video=0):
            return frame
    raise ValueError(f"no image in {path}")


class Fitter:
    """Scales and letterboxes a frame of any size onto the movie's canvas.

    One filter graph per input size, built on demand: a graph's source is
    configured for the size it was made with, and a run has two sizes in it —
    the screen and the zoom — so this is a cache of two.
    """

    def __init__(self, width, height):
        self.width, self.height = width, height
        self.graphs = {}

    def _graph(self, frame):
        key = (frame.width, frame.height, frame.format.name)
        if key not in self.graphs:
            graph = av.filter.Graph()
            source = graph.add_buffer(
                width=frame.width,
                height=frame.height,
                format=frame.format.name,
                time_base=TIME_BASE,
            )
            scale = graph.add(
                "scale",
                f"{self.width}:{self.height}:force_original_aspect_ratio=decrease",
            )
            pad = graph.add(
                "pad", f"{self.width}:{self.height}:(ow-iw)/2:(oh-ih)/2:black"
            )
            sink = graph.add("buffersink")
            source.link_to(scale)
            scale.link_to(pad)
            pad.link_to(sink)
            graph.configure()
            self.graphs[key] = graph
        return self.graphs[key]

    def fit(self, frame):
        graph = self._graph(frame)
        graph.push(frame)
        return graph.pull()


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
                        help="scale the movie to this width, 0 for the guest's own size")
    parser.add_argument("--min-ms", type=int, default=400,
                        help="shortest a frame may be held")
    parser.add_argument("--max-ms", type=int, default=2500,
                        help="longest a frame may be held; this is what makes idle watchable")
    parser.add_argument("--aim-ms", type=int, default=900,
                        help="minimum hold on an 'about to click' frame")
    parser.add_argument("--hold-ms", type=int, default=3000,
                        help="how long the final frame stays up")
    parser.add_argument("--zooms", choices=("pad", "skip"), default="pad",
                        help="what to do with zoom frames, which are not screen-sized")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    directory = Path(args.directory).resolve()
    index = directory / "frames.jsonl"
    if not index.exists():
        sys.exit(f"no frames.jsonl in {directory}\n"
                 "Run tapto-vnc with --screenshots <dir> first.")

    frames, sessions = read_index(index)
    if not frames:
        sys.exit("frames.jsonl lists no frames")

    # The commonest size rather than the first frame's: a run that was resized
    # part way through should still be a movie of whatever it spent its time at.
    sizes = {}
    for frame in frames:
        sizes[(frame["width"], frame["height"])] = sizes.get((frame["width"], frame["height"]), 0) + 1
    screen_w, screen_h = max(sizes, key=sizes.get)

    if args.width:
        out_w = args.width
        out_h = round(screen_h * args.width / screen_w)
    else:
        out_w, out_h = screen_w, screen_h
    # 4:2:0 stores chroma at half resolution in each axis, so both have to be
    # even or the encoder has nowhere to put the last row.
    out_w -= out_w % 2
    out_h -= out_h % 2

    holds = plan_durations(frames, args)

    chosen = []
    for frame, held in zip(frames, holds):
        path = directory / frame["file"]
        if not path.exists():
            continue
        if args.zooms == "skip" and (frame["width"], frame["height"]) != (screen_w, screen_h):
            continue
        chosen.append((frame, path, held))
    if not chosen:
        sys.exit("no usable frames")

    out_path = directory / args.out
    container = av.open(str(out_path), mode="w", options={"movflags": "+faststart"})
    stream = container.add_stream("libx264", rate=Fraction(30, 1))
    stream.width, stream.height = out_w, out_h
    stream.pix_fmt = "yuv420p"
    stream.codec_context.time_base = TIME_BASE
    # tune=stillimage is x264's setting for exactly this material: long stretches
    # of an unchanging desktop, where the default psychovisual tuning spends bits
    # on grain that is not there.
    stream.options = {"crf": str(args.crf), "preset": args.preset, "tune": "stillimage"}

    fitter = Fitter(out_w, out_h)
    pts = 0
    padded = 0
    written = 0
    try:
        for frame, path, held in chosen:
            image = decode_png(path)
            if (image.width, image.height) != (out_w, out_h):
                padded += 1
            fitted = fitter.fit(image)
            fitted.pts = pts
            fitted.time_base = TIME_BASE
            for packet in stream.encode(fitted):
                container.mux(packet)
            pts += held
            written += 1
            if not args.quiet and written % 25 == 0:
                print(f"  {written}/{len(chosen)} frames")

        # A frame's duration is implied by the next frame's timestamp, so the
        # last one has none: without this the movie cuts the moment the final
        # picture appears. Repeating it gives the ending something to sit on,
        # and an identical frame costs the encoder almost nothing.
        if written:
            fitted.pts = pts
            for packet in stream.encode(fitted):
                container.mux(packet)
            pts += args.hold_ms

        for packet in stream.encode(None):
            container.mux(packet)
    finally:
        container.close()

    # The same cut list for anyone with a system ffmpeg:
    #   ffmpeg -f concat -safe 0 -i frames.concat -vsync vfr -pix_fmt yuv420p out.mp4
    # The last entry is repeated because the concat demuxer ignores the final
    # duration, for the same reason the encoder above needed a repeat.
    concat = directory / "frames.concat"
    with concat.open("w", encoding="utf-8") as handle:
        for frame, _, held in chosen:
            handle.write(f"file '{frame['file']}'\nduration {held / 1000:.3f}\n")
        handle.write(f"file '{chosen[-1][0]['file']}'\n")

    if not args.quiet:
        real_ms = frames[-1]["epoch_ms"] - frames[0]["epoch_ms"]
        print()
        print(f"wrote {out_path}")
        print(f"  frames    {written} of {len(frames)} ({padded} fitted from another size)")
        print(f"  plays in  {pts / 60000:.1f} min, from {real_ms / 60000:.1f} min of real time")
        print(f"  size      {out_path.stat().st_size / 1e6:.1f} MB"
              f" at {out_w}x{out_h}, crf {args.crf}")
        if len(sessions) > 1:
            print(f"  note      {len(sessions)} sessions in this directory;"
                  " the gaps between them are clamped like any other")


if __name__ == "__main__":
    main()
