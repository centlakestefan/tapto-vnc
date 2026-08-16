// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/ui.h"

#include "tapto/frame_index.h"

#include <iostream>

#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define TAPTO_ISATTY _isatty
#define TAPTO_FILENO _fileno
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <unistd.h>
#define TAPTO_ISATTY isatty
#define TAPTO_FILENO fileno
#endif

namespace tapto::ui {
namespace {

// In-place rewriting depends on "\r" moving the cursor, which only happens on
// a terminal. Redirected to a file or a pipe, every intermediate status would
// otherwise be written out in full and the transcript becomes unreadable.
bool isTerminal() {
    static const bool kTerminal = TAPTO_ISATTY(TAPTO_FILENO(stdout)) != 0;
    return kTerminal;
}

// Whether escape sequences will actually be *rendered*.
//
// Being a terminal is not enough on Windows: the classic console only
// interprets ANSI once ENABLE_VIRTUAL_TERMINAL_PROCESSING is turned on, and
// without it the escapes are printed literally as "[2m". So ask for the mode
// and report whether we got it; if not, the caller falls back to plain text
// rather than spraying control codes at the user.
bool ansiRendersHere() {
    static const bool kOk = [] {
#ifdef _WIN32
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode)) return false;   // not a real console
        if (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) return true;
        return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        return true;
#endif
    }();
    return kOk;
}

ColorMode   g_colorMode = ColorMode::Auto;
bool        g_useColor = true;
std::string g_status;          // plain text — measures the visible width
std::string g_statusRendered;  // same line with escapes, for actual output
size_t      g_statusWidth = 0; // characters to overwrite when erasing

// Same codes tapto-code uses, so the two tools look alike.
//
// Note kDim on its own is not enough: the classic Windows console host does
// not implement SGR 2 (faint), so text styled only with it comes out looking
// completely unstyled. Anything that should be visibly distinct therefore
// carries a real colour as well.
constexpr const char* kReset  = "\x1b[0m";
constexpr const char* kDim    = "\x1b[2m";
constexpr const char* kCyan   = "\x1b[36m";
constexpr const char* kYellow = "\x1b[33m";
constexpr const char* kRed    = "\x1b[31m";

bool useAnsi() {
    if (g_colorMode == ColorMode::Never)  return false;
    if (g_colorMode == ColorMode::Always) return true;   // caller knows better
    if (!g_useColor || !isTerminal()) return false;
    // Widely honoured convention: any value means "no colour".
    if (std::getenv("NO_COLOR") != nullptr) return false;
    return ansiRendersHere();
}

std::string styled(const char* code, const std::string& text) {
    return useAnsi() ? std::string(code) + text + kReset : text;
}

std::string dim(const std::string& text) { return styled(kDim, text); }

// "[3/60] " — coloured so the progress counter stands out from the tool name
// even on a console that drops faint.
std::string counter(int iteration, int max_iterations) {
    if (iteration <= 0) return {};
    const std::string text =
        "[" + std::to_string(iteration) + "/" + std::to_string(max_iterations) + "] ";
    return styled(kCyan, text);
}

// The status line lives on the current row and is rewritten in place, so it
// must be blanked before any permanent output lands on the same row.
void eraseStatus() {
    if (g_statusWidth == 0) return;
    std::cout << '\r' << std::string(g_statusWidth, ' ') << '\r';
    g_statusWidth = 0;
    g_status.clear();
}

}  // namespace

void set_use_color(bool enabled) { g_useColor = enabled; }
void set_color_mode(ColorMode mode) { g_colorMode = mode; }

void set_status(const std::string& text, int iteration, int max_iterations) {
    std::string prefix;
    if (iteration > 0) {
        prefix = "[" + std::to_string(iteration) + "/" + std::to_string(max_iterations) + "] ";
    }
    // Width is measured on the plain text: escape sequences occupy no columns,
    // so counting them would over-pad and leave the line ragged.
    g_status = prefix + text;
    g_statusRendered = counter(iteration, max_iterations) + dim(text);

    // Off a terminal, hold the text back until commit_status() makes it
    // permanent; transient states are noise in a captured log.
    if (!isTerminal()) return;

    // Pad to the previous width so leftovers from a longer line don't survive.
    const size_t previous = g_statusWidth;
    std::cout << '\r' << g_statusRendered;
    if (previous > g_status.size()) {
        std::cout << std::string(previous - g_status.size(), ' ');
    }
    std::cout << std::flush;
    g_statusWidth = g_status.size();
}

void commit_status() {
    if (g_status.empty()) return;
    const std::string rendered = g_statusRendered;
    eraseStatus();
    g_status.clear();
    g_statusRendered.clear();
    std::cout << "  " << rendered << "\n" << std::flush;
}

void end_status() {
    eraseStatus();
    std::cout << std::flush;
}

void emit_intermediate(const std::string& text, bool is_reasoning, bool print_cot) {
    // Recorded before the print_cot test, deliberately: --quiet is about what
    // this terminal shows, not about what the run was. A quiet run's frames
    // should still be able to say what the model was thinking while it worked.
    frames::record(is_reasoning ? "think" : "say", text);

    if (!print_cot || text.empty()) return;
    eraseStatus();
    std::cout << (is_reasoning ? dim(text) : text) << "\n" << std::flush;
}

void print_reply(const std::string& text) {
    eraseStatus();
    std::cout << text << "\n" << std::flush;
}

void print_line(const std::string& text) {
    eraseStatus();
    std::cout << text << "\n" << std::flush;
}

void print_error(const std::string& text) {
    eraseStatus();
    std::cerr << styled(kRed, "error: " + text) << "\n" << std::flush;
}

void print_warning(const std::string& text) {
    eraseStatus();
    std::cerr << styled(kYellow, "warning: " + text) << "\n" << std::flush;
}

}  // namespace tapto::ui
