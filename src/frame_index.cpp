// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/frame_index.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "tapto/version.h"

using nlohmann::json;

namespace tapto {
namespace frames {
namespace {

std::string g_directory;

// Milliseconds since the epoch. Absolute rather than relative to this process,
// so a session stopped and restarted against the same directory extends one
// timeline: a per-process clock restarting at zero would not merely lose the
// gap, it would assert that the second session began the instant the first
// ended.
long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

// Turns "2x left-clicked at (400,300)" into "2x-left-clicked-at-400-300" so it
// can serve as a filename on any platform.
std::string slugify(const std::string& text) {
    std::string out;
    bool lastWasDash = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            lastWasDash = false;
        } else if (!lastWasDash && !out.empty()) {
            out.push_back('-');
            lastWasDash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 60) out.resize(60);
    return out.empty() ? "screenshot" : out;
}

// The highest number any file in `dir` already starts with, or 0 for a
// directory with none — which is where this process's numbering carries on
// from.
//
// Reads the directory rather than frames.jsonl, because the number's first job
// is to not overwrite a file that is already there, and the files are the ones
// that know. An index deleted by hand, or a directory filled by an older build,
// still numbers correctly.
int highestSequenceIn(const std::filesystem::path& dir) {
    int highest = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        size_t digits = 0;
        while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[digits]))) {
            ++digits;
        }
        if (digits == 0) continue;
        try {
            highest = std::max(highest, std::stoi(name.substr(0, digits)));
        } catch (const std::exception&) {
            // A name beginning with more digits than an int holds is not one of
            // ours; ignore it rather than letting it decide the numbering.
        }
    }
    return highest;
}

// Appends one JSON object as a line. Everything written here goes through this,
// so the file is always one-object-per-line and a run that dies half way still
// leaves an index that parses to the point it stopped.
void append(const json& entry) {
    std::ofstream index(std::filesystem::path(g_directory) / "frames.jsonl", std::ios::app);
    index << entry.dump() << "\n";
}

// The number the next frame gets, and — once per process, before the first of
// them — a start record.
//
// The start record is where an assembling pass finds the seam between two
// sessions: the gap there is somebody at a keyboard, not the guest sitting
// still. It names the build for the same reason the first line of a trace does:
// the frames outlive the binary that made them.
int nextSequence() {
    static int sequence = -1;
    if (sequence < 0) {
        sequence = highestSequenceIn(g_directory);
        append(json{
            {"event", "start"},
            {"epoch_ms", nowMs()},
            {"continues_from", sequence},
            {"version", TAPTO_VNC_VERSION},
            {"commit", TAPTO_VNC_COMMIT},
        });
    }
    return ++sequence;
}

}  // namespace

void set_directory(const std::string& dir) { g_directory = dir; }
const std::string& directory() { return g_directory; }

void save(const std::vector<uint8_t>& png, const std::string& label, const Meta& meta) {
    if (g_directory.empty() || png.empty()) return;

    const long long stamp = nowMs();
    try {
        std::filesystem::create_directories(g_directory);
        const int sequence = nextSequence();

        std::ostringstream name;
        name << std::setfill('0') << std::setw(4) << sequence << "-" << slugify(label) << ".png";
        const std::filesystem::path path = std::filesystem::path(g_directory) / name.str();

        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
        file.close();

        json entry{
            {"seq", sequence},
            {"file", name.str()},
            {"epoch_ms", stamp},
            {"kind", meta.kind},
            {"label", label},
            {"width", meta.width},
            {"height", meta.height},
        };
        if (meta.phase && *meta.phase) entry["phase"] = meta.phase;
        if (meta.has_aim) entry["aim"] = json{{"x", meta.aim_x}, {"y", meta.aim_y}};
        append(entry);
    } catch (const std::exception&) {
        // Saving is a diagnostic convenience; never fail a tool call over it.
    }
}

void record(const char* kind, const std::string& text) {
    if (g_directory.empty() || text.empty()) return;
    try {
        std::filesystem::create_directories(g_directory);
        append(json{
            {"event", kind},
            {"epoch_ms", nowMs()},
            {"text", text},
        });
    } catch (const std::exception&) {
        // As above: what the run is actually doing matters more than the record
        // of it.
    }
}

}  // namespace frames
}  // namespace tapto
