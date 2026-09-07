// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

// ---------------------------------------------------------------------------
// The user-facing output the library needs, and nothing more.
//
// The provider clients report progress through these seven functions. What
// they mean is up to the program: tapto-code drives a terminal status line,
// tapto-word forwards them to a Word task pane over SSE, tapto-vnc styles them
// for a console. Each program links its own ui.cpp supplying these bodies;
// libtapto declares them and supplies none.
//
// A program that wants more (a banner, a help screen, config listings) puts
// those in its own header. Do not add to this one unless a library source
// calls it.
//
// Thread-safety: the backends call these from the thread running chat(). A
// program that runs several chats at once must make its implementation safe
// for that; the library makes no promise here.
// ---------------------------------------------------------------------------

namespace tapto::ui {

// --- Progress / status line ------------------------------------------------
//
// The status line is the transient "what is happening right now" row. It is
// replaced in place as the model works and never becomes part of the permanent
// transcript unless commit_status() says so.

// Show or update the status line. `iteration` and `max_iterations` are the
// current tool-loop counters; pass 0/0 for the initial "Thinking..." phase
// before any tools have run.
//
//   "Thinking..."
//   "[1/50] Thinking..."
//   "[3/50] Edit src/foo.cpp"
void set_status(const std::string& text, int iteration, int max_iterations);

// Commit the current status line to the transcript as a permanent line, then
// clear the status state. Called after a tool finishes so its label stays
// visible while "Thinking..." takes the next line.
void commit_status();

// Erase the status line. Called once at the end of a chat turn before the
// final reply is shown.
void end_status();

// --- Permanent output ------------------------------------------------------

// The model's intermediate chain-of-thought or prose accompanying a tool call.
// `is_reasoning` selects a dimmed style for thinking blocks vs. normal prose.
// Respects `print_cot`: when false the call is a no-op.
void emit_intermediate(const std::string& text, bool is_reasoning, bool print_cot);

// A plain line of output.
void print_line(const std::string& text);

// An error, and a warning. Library code uses these for things the user must
// see and act on (a provider block that names no dialect), never for progress.
void print_error(const std::string& text);
void print_warning(const std::string& text);

} // namespace tapto::ui
