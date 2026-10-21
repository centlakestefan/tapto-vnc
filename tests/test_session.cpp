// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// The saved conversation behind --resume and /resume. A save must read back
// as it was written, including history the provider shaped and text that is
// not valid UTF-8; a file that is damaged, foreign or from another layout
// version must be refused with a reason rather than loaded half-way, and one
// saved for another machine is not this machine's. Ported with the store from
// tapto-code's test.

#include "tapto/session.h"

#include "tapto/paths.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using tapto::SavedSession;

namespace {

int g_checks = 0;
int g_failures = 0;

void check_eq(int line, const char* what, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n"
              << "  expected:\n" << expected << "\n"
              << "  actual:\n" << actual << "\n";
}

void check_true(int line, const char* what, bool cond) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ") " << what << "\n";
}

#define CHECK_EQ(actual, expected) check_eq(__LINE__, #actual, (actual), (expected))
#define CHECK_TRUE(cond) check_true(__LINE__, #cond, (cond))

const char* kDir = "tapto-session-tests";

void write_raw(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << bytes;
}

} // namespace

int main() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
    // The directory is created by the save itself, as it is under ~/.tapto.
    const std::string target = "vnc:192.0.2.10:0";
    const fs::path file = fs::path(kDir) / "project" / "session.json";

    // --- nothing saved -----------------------------------------------------
    {
        std::string err;
        CHECK_TRUE(!tapto::load_session(file, target, err));
        CHECK_EQ(err, "");
    }

    // --- round trip --------------------------------------------------------
    {
        SavedSession s;
        s.target = target;
        s.provider = "qwen36";
        s.dialect = "openai";
        s.model = "Qwen3-VL-30B";
        s.folders.push_back("C:/proj/libfoo");
        s.folders.push_back("C:/proj/libbar");
        s.pending_prompt = "fix the build\nplease";
        s.history = json::array({
            {{"role", "user"}, {"content", "hello \xE2\x80\x94 world"}},
            {{"role", "assistant"}, {"content", "hi"},
             {"tool_calls", json::array({{{"id", "c1"}, {"type", "function"}}})}},
        });
        CHECK_EQ(tapto::save_session(file, s), "");
        CHECK_TRUE(!fs::exists(fs::path(file).concat(".tmp")));

        std::string err;
        auto back = tapto::load_session(file, target, err);
        CHECK_EQ(err, "");
        CHECK_TRUE(back.has_value());
        if (back) {
            CHECK_EQ(back->target, target);
            CHECK_EQ(back->provider, "qwen36");
            CHECK_EQ(back->dialect, "openai");
            CHECK_EQ(back->model, "Qwen3-VL-30B");
            CHECK_EQ(back->pending_prompt, "fix the build\nplease");
            CHECK_EQ(back->history.dump(), s.history.dump());
            CHECK_TRUE(back->folders.size() == 2);
            if (back->folders.size() == 2) {
                CHECK_EQ(back->folders[1], "C:/proj/libbar");
            }
            CHECK_EQ(std::to_string(back->saved_at.size()), "16"); // YYYY-MM-DD HH:MM
        }
    }

    // --- a second save replaces the first; no pending prompt is none --------
    {
        SavedSession s;
        s.target = target;
        s.dialect = "claude";
        s.history = json::array({{{"role", "user"}, {"content", "second"}}});
        CHECK_EQ(tapto::save_session(file, s), "");
        std::string err;
        auto back = tapto::load_session(file, target, err);
        CHECK_TRUE(back.has_value());
        if (back) {
            CHECK_EQ(back->dialect, "claude");
            CHECK_EQ(back->pending_prompt, "");
            CHECK_TRUE(back->folders.empty());
            CHECK_EQ(back->history.dump(), s.history.dump());
        }
    }

    // --- bytes that are not UTF-8 are replaced, not a failed save ------------
    {
        SavedSession s;
        s.target = target;
        s.dialect = "claude";
        s.history = json::array({{{"role", "user"}, {"content", std::string("bad \xFF byte")}}});
        CHECK_EQ(tapto::save_session(file, s), "");
        std::string err;
        auto back = tapto::load_session(file, target, err);
        CHECK_TRUE(back.has_value());
        if (back) {
            CHECK_EQ(back->history[0]["content"].get<std::string>(), "bad \xEF\xBF\xBD byte");
        }
    }

    // --- another machine's conversation is none, not an error ----------------
    {
        std::string err;
        CHECK_TRUE(!tapto::load_session(file, "vm:vcenter/build-01", err));
        CHECK_EQ(err, "");
    }

    // --- refused files ------------------------------------------------------
    {
        std::string err;
        write_raw(file, "{\"version\": 1, \"history\": [");
        CHECK_TRUE(!tapto::load_session(file, target, err));
        CHECK_TRUE(err.find("is not a saved session") != std::string::npos);

        write_raw(file, "{\"version\": 99, \"history\": []}");
        CHECK_TRUE(!tapto::load_session(file, target, err));
        CHECK_TRUE(err.find("different version") != std::string::npos);

        write_raw(file, "{\"version\": 1, \"history\": {}}");
        CHECK_TRUE(!tapto::load_session(file, target, err));
        CHECK_TRUE(err.find("holds no conversation") != std::string::npos);

        write_raw(file, "{\"version\": 1, \"provider\": 7, \"history\": []}");
        CHECK_TRUE(!tapto::load_session(file, target, err));
        CHECK_TRUE(err.find("is not a saved session") != std::string::npos);
    }

    // --- it lives beside the local config, not in the project ----------------
    // One file per machine, named after it in characters any file system takes.
    CHECK_EQ(tapto::session_path(target).parent_path().string(),
             tapto::config_path(tapto::Level::Local).parent_path().string());
    CHECK_EQ(tapto::session_path(target).filename().string(), "session-vnc_192.0.2.10_0.json");
    CHECK_EQ(tapto::session_path("vm:vc.example.com/Build VM 01").filename().string(),
             "session-vm_vc.example.com_Build_VM_01.json");

    // --- screenshots are dropped from what is saved, in every dialect --------
    {
        const json history = json::parse(R"([
            {"role":"user","content":"click OK"},
            {"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":[
                {"type":"text","text":"clicked"},
                {"type":"image","source":{"type":"base64","media_type":"image/png","data":"AA=="}}]}]},
            {"role":"user","content":[{"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]},
            {"role":"user","parts":[{"inline_data":{"mime_type":"image/png","data":"AA=="}}]}
        ])");
        const std::string saved = tapto::without_screenshots(history).dump();
        CHECK_TRUE(saved.find("AA==") == std::string::npos);
        CHECK_TRUE(saved.find("clicked") != std::string::npos);
        CHECK_TRUE(saved.find("click OK") != std::string::npos);
        CHECK_TRUE(saved.find("screenshot left out") != std::string::npos);
        CHECK_EQ(tapto::without_screenshots(json::object()).dump(), "{}");

        // The openai dialect's screenshot turn, image and all, and one that
        // libtapto already reduced to its own placeholder while the run went
        // on: neither is a prompt once saved, and the library's reason for
        // the gap is not the one that applies.
        const json openai = json::parse(R"([
            {"role":"user","content":"click OK"},
            {"role":"assistant","content":"Clicked."},
            {"role":"user","content":[{"type":"text","text":"Captured the screen:"},
                {"type":"text","text":"[earlier image omitted to stay within the request size limit; request a fresh one if you need to see it again]"}]},
            {"role":"user","content":[{"type":"text","text":"Captured the screen:"},
                {"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]},
            {"role":"assistant","content":"Done."}
        ])");
        const json stripped = tapto::without_screenshots(openai);
        CHECK_TRUE(stripped.dump().find("request size limit") == std::string::npos);
        std::string steps;
        for (const auto& e : tapto::transcript(stripped)) steps += e.text + "|";
        CHECK_EQ(steps, "click OK|Clicked.\n\nDone.|");
    }

    // --- the transcript /resume shows, from each dialect's history -----------
    // The prompts and the model's text, in order; the model's texts around
    // its tool calls joined; tool turns, images and thinking left out; the
    // /compact seed a marker of its own.
    {
        using Who = tapto::TranscriptEntry::Who;
        auto render = [](const std::vector<tapto::TranscriptEntry>& t) {
            std::string out;
            for (const auto& e : t) {
                out += e.who == Who::User ? "U:" : e.who == Who::Model ? "M:" : "S:";
                out += e.text + "|";
            }
            return out;
        };
        const std::string seed =
            "(The conversation prior to this point has been summarized for context):\nwe did X";

        json claude = json::parse(R"([
            {"role":"user","content":"fix the build"},
            {"role":"assistant","content":[
                {"type":"thinking","thinking":"hmm","signature":"s"},
                {"type":"text","text":"Looking at it."},
                {"type":"tool_use","id":"t1","name":"run_command","input":{}}]},
            {"role":"user","content":[
                {"type":"tool_result","tool_use_id":"t1","content":"ok"}]},
            {"role":"assistant","content":[{"type":"text","text":"Fixed.\n\n"}]},
            {"role":"user","content":[{"type":"text","text":"thanks","cache_control":{"type":"ephemeral"}}]}
        ])");
        claude.insert(claude.begin(), json{{"role", "user"}, {"content", seed}});
        CHECK_EQ(render(tapto::transcript(claude)),
                 "S:|U:fix the build|M:Looking at it.\n\nFixed.|U:thanks|");

        const json openai = json::parse(R"([
            {"role":"system","content":"be brief"},
            {"role":"user","content":"show the logo"},
            {"role":"assistant","content":null,"reasoning_content":"think",
             "tool_calls":[{"id":"c1","type":"function","function":{"name":"view","arguments":"{}"}}]},
            {"role":"tool","tool_call_id":"c1","content":"done"},
            {"role":"user","content":[{"type":"text","text":"logo.png:"},
                {"type":"image_url","image_url":{"url":"data:image/png;base64,AA=="}}]},
            {"role":"assistant","content":"It is blue."}
        ])");
        CHECK_EQ(render(tapto::transcript(openai)), "U:show the logo|M:It is blue.|");

        const json gemini = json::parse(R"([
            {"role":"user","parts":[{"text":"list files"}]},
            {"role":"model","parts":[{"text":"pondering","thought":true},
                {"functionCall":{"name":"find_files","args":{}}}]},
            {"role":"user","parts":[{"functionResponse":{"name":"find_files","response":{"content":"a.cpp"}}}]},
            {"role":"model","parts":[{"text":"There is "},{"text":"a.cpp."}]}
        ])");
        CHECK_EQ(render(tapto::transcript(gemini)), "U:list files|M:There is a.cpp.|");

        CHECK_EQ(render(tapto::transcript(json::object())), "");
        CHECK_EQ(render(tapto::transcript(json::parse(R"([1,"x",{"role":"user"}])"))), "");
    }

    fs::remove_all(kDir, ec);

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
