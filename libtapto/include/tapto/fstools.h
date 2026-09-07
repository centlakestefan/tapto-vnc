// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tapto/tool_registry.h"

// ---------------------------------------------------------------------------
// Read-only folder access.
//
// The user grants the model a folder -- `/add-folder C:\proj\tapto-code` --
// and the model can list, read and search what is under it. Nothing more: no
// create, no edit, no shell. The use case is a model writing documentation
// about a software project into a document open beside it.
//
// This generalises the sandbox in tapto-code's tools.cpp, which is pinned to
// one root (the working directory), to N roots granted at runtime. It is in
// the library so tapto-code can offer the same slash commands without a second
// copy of the path rules.
//
// Paths the model supplies are resolved against the granted roots and refused
// otherwise. A refusal is a tool result naming the roots that ARE granted, so
// the model learns the boundary rather than guessing at another path.
//
// Threading: a FolderSet is mutated by slash commands and read by tool
// executors. Both run on the chat thread of one session, so there is no lock
// here; a program that shares one set across threads must add its own.
// ---------------------------------------------------------------------------

namespace tapto {

struct Folder {
    std::string label;          // how the model names it: the last path component, made unique
    std::filesystem::path root; // canonical absolute path
    // Whether the program's own editing tool may write under this root. The
    // tools in this file never write, whatever the flag says: it is a grant
    // the program's editor consults (tapto-code's does), and a program with
    // no editor simply never sets it. Off by default.
    bool writable = false;
};

class FolderSet {
public:
    // Grant a directory. Returns an empty string on success and fills
    // `label_out` (when given) with the label the model will see; otherwise an
    // "ERROR: ..." sentence. Granting a folder already granted is a no-op
    // success that leaves its mode alone (see set_writable), and granting a
    // subfolder of a granted root is allowed but pointless, so it is reported
    // as such rather than refused.
    std::string add(const std::string& path, std::string* label_out = nullptr,
                    bool writable = false);

    // Change a granted folder's mode. False if nothing matched.
    bool set_writable(const std::string& path_or_label, bool writable);

    // The granted folder a label or path names, or null.
    const Folder* get(const std::string& path_or_label) const { return find(path_or_label); }

    // Revoke by label or by path. False if nothing matched.
    bool remove(const std::string& path_or_label);

    const std::vector<Folder>& folders() const { return m_folders; }
    bool empty() const { return m_folders.empty(); }
    void clear() { m_folders.clear(); }

    // Resolve a model-supplied path to a real one inside a granted root.
    //
    //   "tapto-code/src/main.cpp"   label-relative
    //   "tapto-code"                the root itself
    //   "C:/proj/tapto-code/src"    absolute, must fall under a root
    //   "src/main.cpp"              relative to the only root, when there is one
    //
    // `..` and symlinks are resolved before the check, so neither escapes.
    // Returns true and fills `out` (and `folder`, when given) on success;
    // otherwise sets `error` to an "ERROR: ..." sentence and returns false.
    bool resolve(const std::string& input,
                 std::filesystem::path& out,
                 std::string& error,
                 const Folder** folder = nullptr) const;

    // The path as the model should see it: "<label>/<relative>", forward
    // slashes. A path under no root comes back unchanged.
    std::string display(const std::filesystem::path& path) const;

private:
    std::vector<Folder> m_folders;
    const Folder* find(const std::string& path_or_label) const;
    const Folder* owner_of(const std::filesystem::path& canonical) const;
};

// The read-only tools over a folder set: list_folders, list_files, read_file,
// search_files. `folders` must outlive the returned specs; the executors hold
// a pointer to it, so a set that grows later is seen by tools built earlier.
std::vector<ToolSpec> folder_tools(const FolderSet& folders);

// A system-prompt paragraph naming the granted roots and the rules, or an empty
// string when nothing is granted. Programs append it to their own prompt when
// they hand out folder_tools(), so the model knows both the tools and why.
std::string folder_prompt(const FolderSet& folders);

// --- Shared helpers ---------------------------------------------------------
// Used by the tools above and available to a program's own filesystem tools,
// so the glob, the noise-directory list and the read cap agree everywhere.

// Glob match supporting '*' (any run) and '?' (single char).
bool wildcard_match(const std::string& pattern, const std::string& text);

// Directories a source-tree walk skips: .git, build, node_modules and the like.
bool is_noise_dir(const std::string& name);

// Whole-file read, byte-exact. False if the file cannot be opened.
bool read_file(const std::filesystem::path& path, std::string& out);

// Split on '\n', dropping a trailing '\r' from each line. The final segment
// is kept even when empty, so a file ending in '\n' yields one empty line at
// the end; content_lines() drops it.
std::vector<std::string> split_lines(const std::string& text);
std::vector<std::string> content_lines(const std::string& content);

// True when the first few KB hold a NUL byte, which no text encoding this
// program reads produces.
bool looks_binary(const std::string& content);

// Truncate at `max_bytes` with a marker, so one huge result cannot fill the
// context window on its own.
std::string cap_output(std::string text, std::size_t max_bytes = 64000);

} // namespace tapto
