// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>

namespace tapto {

// Config scopes, ordered lowest to highest precedence.
enum class Level { System, Global, Local };

// Human-readable name ("system" / "global" / "local").
const char* level_name(Level level);

// Resolve the config file path for a scope.
//
// System and Global are fixed per-machine / per-user. Local (project) scope is
// stored centrally per working directory under the user's home
// (~/.tapto/projects/<encoded-cwd>/config), NOT inside the project folder —
// so a cloned repo can't ship config/commands and nothing is written into the
// project tree. No upward search.
std::filesystem::path config_path(Level level);

// Same resolution as config_path, but for the allow-listed commands store
// (".tapto/commands").
std::filesystem::path commands_path(Level level);

} // namespace tapto
