// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/fstools.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "tapto/context.h"
#include "tapto/encoding.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace tapto {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

bool wildcard_match(const std::string& pattern, const std::string& text) {
    // Iterative with backtracking so it stays O(n*m) without recursion.
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool is_noise_dir(const std::string& name) {
    return name == ".git" || name == "build" || name == "node_modules" ||
           name == ".tapto" || name == ".vs" || name == ".vscode" ||
           name == "__pycache__" || name == ".idea" ||
           (name.size() > 6 && name.compare(0, 6, "build-") == 0);
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    return lines;
}

std::vector<std::string> content_lines(const std::string& content) {
    if (content.empty()) return {};
    auto lines = split_lines(content);
    if (content.back() == '\n' && !lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

bool looks_binary(const std::string& content) {
    const size_t probe = std::min<size_t>(content.size(), 8192);
    return content.find('\0') < probe;
}

std::string cap_output(std::string text, std::size_t max_bytes) {
    if (text.size() > max_bytes) {
        text = text.substr(0, max_bytes) + "\n... [output truncated at " +
               std::to_string(max_bytes) + " bytes]";
    }
    return text;
}

std::vector<LineMatch> find_matching_lines(const fs::path& path,
                                           const std::string& query,
                                           std::size_t max_lines) {
    std::vector<LineMatch> out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;

    // Binary probe: a NUL in the first 8 KiB marks the file as binary (the
    // same first-8KiB window looks_binary uses, but without holding the file).
    char probe[8192];
    in.read(probe, sizeof(probe));
    const std::streamsize got = in.gcount();
    for (std::streamsize i = 0; i < got; ++i)
        if (static_cast<unsigned char>(probe[i]) == 0) return out; // binary
    in.clear();
    in.seekg(0, std::ios::beg);

    int lineno = 0;
    std::string line;
    for (;;) {
        if (out.size() >= max_lines) break;             // enough matches already
        if (!std::getline(in, line)) {
            if (!in.eof() && in.bad()) break;            // real error: stop
            if (line.empty()) break;                    // clean EOF, nothing left
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ++lineno;                                    // final line, no newline
            if (line.find(query) != std::string::npos && out.size() < max_lines)
                out.push_back({lineno, line});
            break;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ++lineno;
        if (line.find(query) != std::string::npos && out.size() < max_lines)
            out.push_back({lineno, line});
    }
    return out;
}

// ---------------------------------------------------------------------------
// FolderSet
// ---------------------------------------------------------------------------

namespace {

// Windows paths compare case-insensitively, and weakly_canonical keeps the
// case it was given, so "c:/Proj" and "C:/proj" name one directory and must
// count as such. Elsewhere the filesystem is case-sensitive and so is this.
bool same_component(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const std::string x = a.string(), y = b.string();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(x[i])) !=
            std::tolower(static_cast<unsigned char>(y[i])))
            return false;
    }
    return true;
#else
    return a == b;
#endif
}

// True when `path` is `root` or lies beneath it, component by component.
bool is_within(const fs::path& root, const fs::path& path) {
    auto r = root.begin(), p = path.begin();
    for (; r != root.end(); ++r, ++p) {
        if (r->empty()) continue; // a trailing separator yields an empty component
        if (p == path.end() || !same_component(*r, *p)) return false;
    }
    return true;
}

fs::path canonical_or_normal(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

std::string label_for(const fs::path& root, const std::vector<Folder>& taken) {
    std::string base = root.filename().string();
    if (base.empty()) base = root.root_name().string(); // "C:" for a drive root
    if (base.empty()) base = "folder";
    std::string label = base;
    for (int n = 2;; ++n) {
        bool clash = false;
        for (const auto& f : taken) {
            if (f.label == label) { clash = true; break; }
        }
        if (!clash) return label;
        label = base + "-" + std::to_string(n);
    }
}

std::string granted_list(const std::vector<Folder>& folders) {
    if (folders.empty()) return "No folders are granted.";
    std::string out = "Granted folders:";
    for (const auto& f : folders) out += "\n  " + f.label + "  ->  " + f.root.generic_string();
    return out;
}

} // namespace

const Folder* FolderSet::find(const std::string& path_or_label) const {
    for (const auto& f : m_folders) {
        if (f.label == path_or_label) return &f;
    }
    const fs::path wanted = canonical_or_normal(fs::path(path_or_label));
    for (const auto& f : m_folders) {
        if (is_within(f.root, wanted) && is_within(wanted, f.root)) return &f;
    }
    return nullptr;
}

const Folder* FolderSet::owner_of(const fs::path& canonical) const {
    for (const auto& f : m_folders) {
        if (is_within(f.root, canonical)) return &f;
    }
    return nullptr;
}

std::string FolderSet::add(const std::string& path, std::string* label_out, bool writable) {
    if (path.empty()) return "ERROR: no folder given.";
    std::error_code ec;
    const fs::path given(path);
    if (!fs::exists(given, ec)) return "ERROR: '" + path + "' does not exist.";
    if (!fs::is_directory(given, ec)) return "ERROR: '" + path + "' is not a directory.";

    const fs::path root = canonical_or_normal(fs::absolute(given, ec));
    // The same directory, or a subfolder of one already granted: covered
    // already, so report the folder that covers it and add nothing.
    if (const Folder* have = owner_of(root)) {
        if (label_out) *label_out = have->label;
        return "";
    }

    Folder f;
    f.root = root;
    f.label = label_for(root, m_folders);
    f.writable = writable;
    if (label_out) *label_out = f.label;
    m_folders.push_back(std::move(f));
    return "";
}

bool FolderSet::set_writable(const std::string& path_or_label, bool writable) {
    const Folder* f = find(path_or_label);
    if (!f) return false;
    m_folders[static_cast<size_t>(f - m_folders.data())].writable = writable;
    return true;
}

bool FolderSet::remove(const std::string& path_or_label) {
    const Folder* f = find(path_or_label);
    if (!f) return false;
    m_folders.erase(m_folders.begin() + (f - m_folders.data()));
    return true;
}

bool FolderSet::resolve(const std::string& input,
                        fs::path& out,
                        std::string& error,
                        const Folder** folder) const {
    if (m_folders.empty()) {
        error = "ERROR: no folders are granted. The user grants one with /add-folder <path>.";
        return false;
    }
    if (input.empty()) {
        error = "ERROR: no path given. " + granted_list(m_folders);
        return false;
    }

    fs::path candidate(input);
    fs::path abs;
    if (candidate.is_absolute()) {
        abs = candidate;
    } else {
        // "<label>" or "<label>/rest": the first component names a folder.
        const std::string head = candidate.begin()->string();
        const Folder* by_label = nullptr;
        for (const auto& f : m_folders) {
            if (f.label == head) { by_label = &f; break; }
        }
        if (by_label) {
            fs::path rest;
            for (auto it = std::next(candidate.begin()); it != candidate.end(); ++it) rest /= *it;
            abs = by_label->root / rest;
        } else if (m_folders.size() == 1) {
            abs = m_folders.front().root / candidate;
        } else {
            error = "ERROR: '" + input + "' does not start with a granted folder's label, and " +
                    "several are granted, so it is ambiguous. Prefix it with one of these labels. " +
                    granted_list(m_folders);
            return false;
        }
    }

    const fs::path resolved = canonical_or_normal(abs);
    const Folder* owner = owner_of(resolved);
    if (!owner) {
        error = "ERROR: '" + input + "' is outside every granted folder. Only what is under " +
                "these can be read: " + granted_list(m_folders);
        return false;
    }
    out = resolved;
    if (folder) *folder = owner;
    return true;
}

std::string FolderSet::display(const fs::path& path) const {
    const fs::path canonical = canonical_or_normal(path);
    const Folder* owner = owner_of(canonical);
    if (!owner) return path.generic_string();
    fs::path rel;
    auto r = owner->root.begin(), p = canonical.begin();
    for (; r != owner->root.end(); ++r) {
        if (r->empty()) continue;
        ++p;
    }
    for (; p != canonical.end(); ++p) rel /= *p;
    const std::string tail = rel.generic_string();
    return tail.empty() ? owner->label : owner->label + "/" + tail;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

namespace {

json object_schema(json properties, std::vector<std::string> required) {
    return json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)}};
}

std::string str_arg(const json& in, const char* key, const std::string& fallback = "") {
    if (in.is_object() && in.contains(key) && in[key].is_string()) return in[key].get<std::string>();
    return fallback;
}

int int_arg(const json& in, const char* key, int fallback) {
    if (!in.is_object() || !in.contains(key)) return fallback;
    const json& v = in[key];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_string()) {
        try { return std::stoi(v.get<std::string>()); } catch (...) {}
    }
    return fallback;
}

// The folder a listing or search starts in: the `folder` argument when given,
// the only granted folder when there is one, otherwise an error.
bool pick_folder(const FolderSet& folders, const json& in, fs::path& base,
                 const Folder*& owner, std::string& error) {
    const std::string arg = str_arg(in, "folder");
    if (arg.empty()) {
        if (folders.folders().size() == 1) {
            owner = &folders.folders().front();
            base = owner->root;
            return true;
        }
        error = "ERROR: `folder` is required when more than one folder is granted. " +
                granted_list(folders.folders());
        return false;
    }
    return folders.resolve(arg, base, error, &owner);
}

std::string tool_list_folders(const FolderSet& folders) {
    if (folders.empty()) {
        return "No folders are granted. The user can grant one by typing "
               "/add-folder <path> in the chat.";
    }
    std::string out = "Read-only access to these folders (refer to files as <label>/<relative path>):";
    for (const auto& f : folders.folders()) {
        out += "\n  " + f.label + "  ->  " + f.root.generic_string();
    }
    return out;
}

std::string tool_list_files(const FolderSet& folders, const json& in) {
    fs::path base;
    const Folder* owner = nullptr;
    std::string err;
    if (!pick_folder(folders, in, base, owner, err)) return err;

    const std::string pattern = str_arg(in, "pattern", "*");
    const int wanted = std::clamp(int_arg(in, "max", 200), 1, 1000);

    std::error_code ec;
    if (!fs::is_directory(base, ec)) {
        return "ERROR: '" + folders.display(base) + "' is not a directory. Use read_file for a file.";
    }

    std::vector<std::string> rows;
    size_t total = 0;
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        // Never follow symlinks: they could point outside the granted root.
        if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec)) {
            if (is_noise_dir(p.filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (!wildcard_match(pattern, p.filename().string())) continue;
        ++total;
        if (static_cast<int>(rows.size()) >= wanted) continue; // keep counting
        std::error_code sz_ec;
        const auto size = fs::file_size(p, sz_ec);
        rows.push_back(folders.display(p) + (sz_ec ? "" : "  (" + std::to_string(size) + " bytes)"));
    }

    if (!total) {
        return "No files matching '" + pattern + "' under " + folders.display(base) +
               " (build output, .git and node_modules are skipped).";
    }
    std::ostringstream out;
    out << total << " file" << (total == 1 ? "" : "s") << " under " << folders.display(base);
    if (pattern != "*") out << " matching '" << pattern << "'";
    if (static_cast<int>(rows.size()) < static_cast<int>(total)) {
        out << " (showing the first " << rows.size() << "; narrow with `pattern` or raise `max`)";
    }
    out << ":\n";
    for (const auto& r : rows) out << r << "\n";
    return cap_output(out.str());
}

std::string tool_read_file(const FolderSet& folders, const json& in) {
    const std::string path = str_arg(in, "path");
    fs::path file;
    std::string err;
    if (!folders.resolve(path, file, err)) return err;

    std::error_code ec;
    if (fs::is_directory(file, ec)) {
        return "ERROR: '" + folders.display(file) + "' is a directory. Use list_files to see what is in it.";
    }
    if (!fs::is_regular_file(file, ec)) return "ERROR: '" + folders.display(file) + "' does not exist.";

    std::string content;
    if (!read_file(file, content)) return "ERROR: '" + folders.display(file) + "' could not be read.";
    if (looks_binary(content)) {
        return folders.display(file) + " is a binary file (" + std::to_string(content.size()) +
               " bytes); it has no text to show.";
    }

    const auto lines = content_lines(content);
    int start = int_arg(in, "start_line", 1);
    int end = int_arg(in, "end_line", -1);
    if (end < 0 || end > static_cast<int>(lines.size())) end = static_cast<int>(lines.size());
    if (start < 1) start = 1;
    if (lines.empty()) return folders.display(file) + " is empty.";
    if (start > static_cast<int>(lines.size())) {
        return "ERROR: start_line " + std::to_string(start) + " is past the end; " +
               folders.display(file) + " has " + std::to_string(lines.size()) + " lines.";
    }
    if (end < start) return "ERROR: end_line is before start_line.";

    // Numbered like tapto-code's `view`, so a model used to one reads the other.
    constexpr size_t kMaxBytes = 64000;
    std::ostringstream out;
    size_t emitted = 0;
    int last = start - 1;
    for (int i = start - 1; i < end; ++i) {
        const std::string row = std::to_string(i + 1) + "|" + lines[i] + "\n";
        if (emitted + row.size() > kMaxBytes) break;
        out << row;
        emitted += row.size();
        last = i + 1;
    }
    std::string result = folders.display(file) + " (" + std::to_string(lines.size()) + " lines";
    if (start != 1 || last != static_cast<int>(lines.size())) {
        result += ", showing " + std::to_string(start) + "-" + std::to_string(last);
    }
    result += ")\n" + out.str();
    if (last < end) {
        result += "... [truncated at " + std::to_string(kMaxBytes) +
                  " bytes; call again with start_line " + std::to_string(last + 1) + "]\n";
    }
    return sanitizeUtf8(result);
}

std::string tool_search_files(const FolderSet& folders, const json& in) {
    const std::string query = str_arg(in, "query");
    if (query.empty()) return "ERROR: `query` must be a non-empty string.";

    fs::path base;
    const Folder* owner = nullptr;
    std::string err;
    if (!pick_folder(folders, in, base, owner, err)) return err;
    const std::string pattern = str_arg(in, "pattern", "*");

    constexpr size_t kMaxFiles = 100;
    constexpr size_t kMaxLinesPerFile = 20;

    struct Match {
        std::string path;
        std::vector<std::pair<int, std::string>> lines;
    };
    std::vector<Match> results;
    bool more = false;

    std::error_code ec;
    fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec)) {
            if (is_noise_dir(p.filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (!wildcard_match(pattern, p.filename().string())) continue;

        // Stream (find_matching_lines) so files larger than the old 5 MiB cap
        // are still searched; the helper bails out on binaries and unreadable
        // files, so no whole-file read or size check is needed here.
        auto lines = find_matching_lines(p, query, kMaxLinesPerFile);
        if (lines.empty()) continue;

        if (results.size() >= kMaxFiles) { more = true; break; }
        Match m;
        m.path = folders.display(p);
        for (const auto& lm : lines) m.lines.emplace_back(lm.line, std::move(lm.text));
        results.push_back(std::move(m));
    }

    if (results.empty()) {
        return "No files under " + folders.display(base) + " matching '" + pattern +
               "' contain '" + query + "'.";
    }
    std::ostringstream out;
    out << "Found '" << query << "' in " << results.size() << " file" << (results.size() == 1 ? "" : "s")
        << (more ? " (more exist; narrow the search)" : "") << ":\n\n";
    for (const auto& m : results) {
        out << m.path << "\n";
        for (const auto& line : m.lines) out << "  " << line.first << ": " << line.second << "\n";
        out << "\n";
    }
    return sanitizeUtf8(cap_output(out.str()));
}

const json kStr = {{"type", "string"}};
const json kInt = {{"type", "integer"}};

} // namespace

std::vector<ToolSpec> folder_tools(const FolderSet& folders) {
    const FolderSet* set = &folders;
    std::vector<ToolSpec> tools;

    {
        ToolSpec t;
        t.name = "list_folders";
        t.description =
            "List the folders the user has granted you read-only access to, with the "
            "label to refer to each one by. Files are addressed as <label>/<relative "
            "path>. You cannot create, edit or delete anything in them.";
        t.parameters = object_schema(json::object(), {});
        t.executor = [set](Context&, const json&) { return tool_list_folders(*set); };
        t.display = [](const json&) { return std::string("List folders"); };
        tools.push_back(std::move(t));
    }
    {
        ToolSpec t;
        t.name = "list_files";
        t.description =
            "List the files under a granted folder, recursively, with sizes. `folder` "
            "is a label from list_folders or a path inside one (optional when only one "
            "folder is granted). `pattern` is a glob on the file name -- '*.cpp', "
            "'README*' -- and `max` caps the listing (default 200). Build output, .git "
            "and node_modules are skipped. Start here to learn a project's shape "
            "before reading files.";
        t.parameters = object_schema({{"folder", kStr}, {"pattern", kStr}, {"max", kInt}}, {});
        t.executor = [set](Context&, const json& in) { return tool_list_files(*set, in); };
        t.display = [](const json& in) {
            const std::string p = str_arg(in, "pattern", "*");
            return "List " + str_arg(in, "folder", "files") + (p == "*" ? "" : " " + p);
        };
        tools.push_back(std::move(t));
    }
    {
        ToolSpec t;
        t.name = "read_file";
        t.description =
            "Read a text file from a granted folder, with line numbers. `path` is "
            "<label>/<relative path> as list_files shows it. Optional `start_line` "
            "and `end_line` (1-based, inclusive) read a slice; a long file is "
            "truncated with a note saying where to continue. Binary files are "
            "described, not dumped.";
        t.parameters = object_schema({{"path", kStr}, {"start_line", kInt}, {"end_line", kInt}}, {"path"});
        t.executor = [set](Context&, const json& in) { return tool_read_file(*set, in); };
        t.display = [](const json& in) {
            std::string label = "Read " + str_arg(in, "path", "?");
            if (in.is_object() && in.contains("start_line")) {
                label += ":" + std::to_string(int_arg(in, "start_line", 1)) + "-" +
                         (int_arg(in, "end_line", -1) < 0 ? "EOF" : std::to_string(int_arg(in, "end_line", -1)));
            }
            return label;
        };
        tools.push_back(std::move(t));
    }
    {
        ToolSpec t;
        t.name = "search_files";
        t.description =
            "Search the text files under a granted folder for a literal string "
            "(case-sensitive), reporting each matching file with its matching lines "
            "and line numbers. `folder` as for list_files; `pattern` narrows by file "
            "name. Use it to find where something is defined or mentioned before "
            "reading the file.";
        t.parameters = object_schema({{"query", kStr}, {"folder", kStr}, {"pattern", kStr}}, {"query"});
        t.executor = [set](Context&, const json& in) { return tool_search_files(*set, in); };
        t.display = [](const json& in) { return "Search \"" + str_arg(in, "query", "?") + "\""; };
        tools.push_back(std::move(t));
    }
    return tools;
}

std::string folder_prompt(const FolderSet& folders) {
    if (folders.empty()) return "";
    bool any_writable = false;
    for (const auto& f : folders.folders()) any_writable = any_writable || f.writable;

    // The all-read-only wording is the original one and is unchanged: a
    // program that never grants write access gets the same prompt as before.
    if (!any_writable) {
        std::string out =
            "The user has granted you read-only access to these folders on their machine, "
            "so you can read a software project and write about it:\n";
        for (const auto& f : folders.folders()) {
            out += "  " + f.label + "  (" + f.root.generic_string() + ")\n";
        }
        out += "Use list_files to see a folder's shape, search_files to find where something "
               "lives, and read_file to read it, addressing files as <label>/<relative path>. "
               "You cannot modify these files, and nothing outside these folders is readable; "
               "if you need another folder, ask the user to grant it with /add-folder.";
        return out;
    }

    std::string out =
        "The user has granted you access to these folders on their machine, beyond "
        "your usual working directory:\n";
    for (const auto& f : folders.folders()) {
        out += "  " + f.label + "  (" + f.root.generic_string() + ")" +
               (f.writable ? "  -- read and write" : "  -- read-only") + "\n";
    }
    out += "Use list_files to see a folder's shape, search_files to find where something "
           "lives, and read_file to read it, addressing files as <label>/<relative path>. "
           "Under a folder marked read and write you may also create and edit files with "
           "your file-editing tool, giving the path either as <label>/<relative path> or as "
           "the absolute path shown above. A read-only folder cannot be modified, and "
           "nothing outside these folders is reachable; if you need another folder, ask "
           "the user to grant it with /add-folder.";
    return out;
}

} // namespace tapto
