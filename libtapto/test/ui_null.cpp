// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

// The library's provider clients call tapto::ui and every program supplies the
// bodies. A test binary is a program too; this one shows nothing, except
// errors and warnings, which a failing test wants to see.

#include "tapto/ui.h"

#include <iostream>

namespace tapto::ui {

void set_status(const std::string&, int, int) {}
void commit_status() {}
void end_status() {}
void emit_intermediate(const std::string&, bool, bool) {}
void print_line(const std::string& text) { std::cout << text << "\n"; }
void print_error(const std::string& text) { std::cerr << "error: " << text << "\n"; }
void print_warning(const std::string& text) { std::cerr << "warning: " << text << "\n"; }

} // namespace tapto::ui
