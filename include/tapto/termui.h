// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

#include "tapto/ui.h"

// tapto-vnc's own terminal output, over and above tapto/ui.h.
//
// libtapto declares the seven functions its provider clients call (status
// line, intermediate text, plain/error/warning lines) and each program
// supplies the bodies. What this program prints beyond those -- the final
// reply, and the colour switches -- is declared here and implemented in the
// same ui.cpp, so the ANSI handling stays in one file. tapto-code and
// tapto-word do the same with a header of their own.
namespace tapto::ui {

// The model's final reply, plain.
void print_reply(const std::string& text);

// Whether to emit ANSI styling; defaults to on for a terminal.
void set_use_color(bool enabled);

enum class ColorMode {
    Auto,    // style only when stdout is a terminal that will render it
    Always,  // style regardless -- for piping into something that renders ANSI
    Never,   // never style
};

// On Windows, Auto also has to *enable* virtual-terminal processing on the
// console: being a terminal is not sufficient there, and without that mode the
// escapes are printed literally as "[2m". If it cannot be enabled, Auto falls
// back to plain text. NO_COLOR in the environment forces plain text under Auto.
void set_color_mode(ColorMode mode);

}  // namespace tapto::ui
