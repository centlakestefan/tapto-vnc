// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

// Terminal output for the agent loop.
//
// This is a deliberately small subset of tapto-code's UI module — only the
// functions the backend calls — so the provider clients can be reused
// unchanged without dragging in a full TUI. The signatures match tapto-code's,
// so its richer implementation can be dropped in later without touching call
// sites.
namespace tapto::ui {

// Show or replace the single-line status the loop keeps on screen while the
// model works. `iteration`/`max` are the tool-loop counters; pass 0 for the
// initial thinking phase.
void set_status(const std::string& text, int iteration, int max_iterations);

// Promote the current status line to a permanent transcript line.
void commit_status();

// Erase the status line. Call once before printing a final reply.
void end_status();

// Print prose or reasoning the model emitted alongside a tool call. A no-op
// when `print_cot` is false.
void emit_intermediate(const std::string& text, bool is_reasoning, bool print_cot);

// Permanent output.
void print_reply(const std::string& text);
void print_line(const std::string& text);
void print_error(const std::string& text);
void print_warning(const std::string& text);

// Whether to emit ANSI styling; defaults to on for a terminal.
void set_use_color(bool enabled);

enum class ColorMode {
    Auto,    // style only when stdout is a terminal that will render it
    Always,  // style regardless — for piping into something that renders ANSI
    Never,   // never style
};

// On Windows, Auto also has to *enable* virtual-terminal processing on the
// console: being a terminal is not sufficient there, and without that mode the
// escapes are printed literally as "[2m". If it cannot be enabled, Auto falls
// back to plain text. NO_COLOR in the environment forces plain text under Auto.
void set_color_mode(ColorMode mode);

}  // namespace tapto::ui
