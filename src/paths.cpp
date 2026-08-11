// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/paths.h"

#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;

namespace tapto {

namespace {

fs::path env_path(const char* name) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // std::getenv is the portable, intended call here
#endif
    const char* value = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return value ? fs::path(value) : fs::path();
}

fs::path system_store_path(const std::string& filename) {
#ifdef _WIN32
    fs::path base = env_path("PROGRAMDATA");
    if (base.empty()) base = "C:\\ProgramData";
    return base / "tapto" / filename;
#else
    return fs::path("/etc/tapto") / filename;
#endif
}

fs::path home_dir() {
#ifdef _WIN32
    fs::path home = env_path("USERPROFILE");
    if (home.empty()) home = env_path("HOMEDRIVE") / env_path("HOMEPATH");
#else
    fs::path home = env_path("HOME");
#endif
    return home;
}

fs::path global_store_path(const std::string& filename) {
    return home_dir() / ".tapto" / filename;
}

// Encode an absolute directory path into a single filename-safe key.
std::string project_key(const fs::path& dir) {
    std::string key;
    for (char c : dir.string()) {
        key += (c == '\\' || c == '/' || c == ':') ? '_' : c;
    }
    size_t start = key.find_first_not_of('_');
    key = (start == std::string::npos) ? std::string("root") : key.substr(start);
    return key;
}

// The project (local) scope is stored centrally per working directory under the
// user's home, NOT inside the project folder. This means a cloned repo can't
// ship config/commands to the agent, and nothing is ever written into the
// project tree (so no .gitignore entry is needed). Keyed by the canonical
// absolute path of the current directory.
fs::path local_path(const std::string& filename) {
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (ec) cwd = ".";
    fs::path canon = fs::weakly_canonical(cwd, ec);
    if (ec) canon = cwd;
    return home_dir() / ".tapto" / "projects" / project_key(canon) / filename;
}

// Resolve the path to a named store file ("config", "commands", ...) for a scope.
fs::path store_path(Level level, const std::string& filename) {
    switch (level) {
        case Level::System: return system_store_path(filename);
        case Level::Global: return global_store_path(filename);
        case Level::Local:  return local_path(filename);
    }
    return fs::path();
}

} // namespace

const char* level_name(Level level) {
    switch (level) {
        case Level::System: return "system";
        case Level::Global: return "global";
        case Level::Local:  return "local";
    }
    return "unknown";
}

fs::path config_path(Level level) {
    return store_path(level, "config");
}

fs::path commands_path(Level level) {
    return store_path(level, "commands");
}

} // namespace tapto
