// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tapto {

// The conversation as it stood after the last completed turn, kept so a run
// ended by accident (Ctrl-C, a closed window, a crash) can be picked up again
// with --resume or /resume. Ported from tapto-code's, which keeps one per
// folder; this one keeps one per folder *and machine*, since a conversation
// about one remote screen is no guide to another.
//
// The history is the provider's own wire format, which differs per dialect,
// so it can only be handed back to a client of the same dialect. The system
// prompt and tools are not stored: they are rebuilt from config as usual, and
// the granted folders are stored so the tools that depend on them come back.
struct SavedSession {
    std::string target;       // the machine, as session_path() was given it
    std::string provider;     // provider name, as chosen with --provider or config
    std::string dialect;      // claude | openai | gemini: what the history is shaped for
    std::string model;
    std::string saved_at;     // local time, "YYYY-MM-DD HH:MM"; filled in by save_session
    std::vector<std::string> folders; // /add-folder grants, the working directory not included
    // A prompt that was sent but whose turn never finished. The history does
    // not contain it, so it is shown on resume for the user to send again.
    std::string pending_prompt;
    nlohmann::json history = nlohmann::json::array();
};

// Where the working directory's session with `target` is kept: beside its
// local config, under ~/.tapto/projects/<encoded-cwd>/, never in the folder
// itself. `target` names the machine ("vnc:host:display", "vm:vcenter/name");
// it is part of the file name, made safe for one, and stored in the file so
// two targets that make the same name are still told apart on load.
std::filesystem::path session_path(const std::string& target);

// Write `s` to `path`, replacing what was there. The file is written beside
// the target and renamed over it, so a crash mid-write leaves the previous
// session intact. Returns an empty string on success, else what went wrong.
std::string save_session(const std::filesystem::path& path, SavedSession s);

// Read a saved session with `target`. nullopt when there is none, including a
// file another target saved; a file that exists but cannot be used sets
// `error` and also returns nullopt.
std::optional<SavedSession> load_session(const std::filesystem::path& path,
                                         const std::string& target, std::string& error);

// `history` with every screenshot in it replaced by a short placeholder, in
// all three dialects' shapes. What a conversation is saved as: a screenshot
// shows a screen as it was, and by the time the conversation is resumed that
// screen may have moved on, so the model has to look again rather than act on
// the last one it saw. It also keeps the file small.
nlohmann::json without_screenshots(nlohmann::json history);

// One step of a conversation as the user saw it on the terminal, for /resume
// to show where the conversation stood.
struct TranscriptEntry {
    enum class Who {
        User,    // a prompt the user typed
        Model,   // what the model wrote back (its text between tool calls too)
        Summary, // the /compact summary the conversation starts from; no text
    };
    Who who = Who::User;
    std::string text;
};

// The conversation in `history`, any of the three dialects' shapes, as the
// user saw it: their prompts and the model's text, in order. Tool calls, tool
// results, images and the model's thinking are left out, and a user turn that
// carries any of them is a tool's turn, not a prompt; so is one holding a
// screenshot that without_screenshots() took out. The model's texts
// between two prompts are joined into one entry.
std::vector<TranscriptEntry> transcript(const nlohmann::json& history);

} // namespace tapto
