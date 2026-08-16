// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tapto {

// The record of a run: the frames a tool saved, and the words that went with
// them.
//
// Frames alone are a slideshow of a machine being operated by nobody. What
// makes a run legible afterwards is knowing what it was asked to do and what
// it was thinking while it did it, and those live in the same file for the
// same reason the frames' sizes and timings do: a later pass — a movie, a
// transcript, a contact sheet — needs them on one timeline, and cannot
// reconstruct them from the pictures.
//
// The text is written where it happens rather than being reassembled from the
// terminal afterwards, so a --quiet run records exactly as much as a noisy one.
namespace frames {

// Where frames and frames.jsonl are written. Empty — the default — disables
// all of it, which is what a run without --screenshots wants.
//
// The directory owns the numbering and the clock, not this process: a session
// restarted against a directory continues its sequence and its timeline. Set
// this once, before anything is saved.
void set_directory(const std::string& dir);
const std::string& directory();
inline bool enabled() { return !directory().empty(); }

// What a saved frame is, beyond its pixels: which of the two sizes it is, where
// it sits in an action's before/after pair, and the point a click was aimed at.
struct Meta {
    const char* kind  = "full";     // "full" or "zoom" — differing pixel sizes
    const char* phase = "";         // "before" / "after" for an action's pair
    int width = 0, height = 0;      // of the image, margins included
    bool has_aim = false;
    int aim_x = 0, aim_y = 0;
};

// Writes a PNG into the directory, numbered and named after its label, and
// appends its line to frames.jsonl. A no-op when no directory is set.
//
// Never throws: saving is a diagnostic convenience, and a full disk must not
// end a run that is otherwise working.
void save(const std::vector<uint8_t>& png, const std::string& label, const Meta& meta = {});

// Appends a line of text to the index — a prompt, a reply, the model's
// reasoning — stamped with the moment it happened.
//
// `kind` is one of "prompt", "reply", "say" or "think". They are kept apart
// rather than merged into one "text" because they are read differently: a
// prompt is an instruction that stands for a whole turn, a reply is an answer,
// and reasoning is the model talking to itself on the way there.
void record(const char* kind, const std::string& text);

}  // namespace frames
}  // namespace tapto
