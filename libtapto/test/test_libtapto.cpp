// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

// Unit tests for the parts of libtapto that need no provider on the other end:
// the config store, secret references, UTF-8 sanitising, tool images, the
// /compact trimmer and the tool display hook. Plain assertions; ctest runs
// the binary.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include "tapto/aibackend.h"
#include "tapto/base64.h"
#include "tapto/certs.h"
#include "tapto/config.h"
#include "tapto/context.h"
#include "tapto/encoding.h"
#include "tapto/fstools.h"
#include "tapto/policy.h"
#include "tapto/provider.h"
#include "tapto/secret.h"
#include "tapto/tool_image.h"
#include "tapto/tool_registry.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond \
                      << "\n";                                                   \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        const auto _a = (a);                                                     \
        const auto _b = (b);                                                     \
        if (!(_a == _b)) {                                                       \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: "    \
                      << #a << " == " << #b << "  (" << _a << " vs " << _b       \
                      << ")\n";                                                  \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

fs::path scratch_file(const char* name) {
    return fs::temp_directory_path() / ("libtapto-test-" + std::string(name));
}

// --- config -----------------------------------------------------------------

void test_config_roundtrip() {
    const fs::path path = scratch_file("config");
    fs::remove(path);

    tapto::Config missing = tapto::Config::load(path);
    CHECK(missing.entries().empty());

    tapto::Config cfg;
    cfg.set("provider", "qwen36");
    cfg.set("qwen36-provider-type", "openai");
    cfg.set("qwen36-provider-type", "openai"); // idempotent
    cfg.set("qwen36-model", "Qwen3-VL-30B");
    CHECK_EQ(cfg.entries().size(), std::size_t(3));
    cfg.save(path);

    tapto::Config back = tapto::Config::load(path);
    CHECK_EQ(back.get("provider").value_or(""), std::string("qwen36"));
    CHECK_EQ(back.get("qwen36-model").value_or(""), std::string("Qwen3-VL-30B"));
    CHECK(!back.get("nope").has_value());
    CHECK(back.unset("qwen36-model"));
    CHECK(!back.unset("qwen36-model"));
    CHECK_EQ(back.entries().size(), std::size_t(2));

    fs::remove(path);
}

// A file edited by hand survives load -> set/unset -> save: comments, blank
// lines and order stay, only the touched key's line changes, and the bytes
// are LF on every platform (CRLF in is normalised, and never written back).
void test_config_preserves_file() {
    const fs::path path = scratch_file("config-preserve");
    {
        std::ofstream out(path, std::ios::binary);
        out << "# tapto-code\r\n# my comment\r\n\r\nprovider = claude\r\nprint-cot = true\r\n";
    }
    tapto::Config cfg = tapto::Config::load(path);
    cfg.set("print-cot", "false");
    cfg.set("model", "claude-sonnet-5");
    CHECK(cfg.unset("provider"));
    cfg.save(path);

    std::string bytes;
    {
        std::ifstream in(path, std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    CHECK_EQ(bytes, std::string("# tapto-code\n# my comment\n\nprint-cot = false\nmodel = claude-sonnet-5\n"));

    fs::remove(path);
}

// --- secret -----------------------------------------------------------------

void test_secret_literal_and_env() {
    tapto::Secret literal = tapto::resolve_secret("sk-plain");
    CHECK(literal.ok());
    CHECK_EQ(literal.value, std::string("sk-plain"));
    CHECK(literal.error.empty());

#ifdef _WIN32
    _putenv_s("LIBTAPTO_TEST_KEY", "from-env");
#else
    setenv("LIBTAPTO_TEST_KEY", "from-env", 1);
#endif
    tapto::Secret env = tapto::resolve_secret("env:LIBTAPTO_TEST_KEY");
    CHECK(env.ok());
    CHECK_EQ(env.value, std::string("from-env"));

    tapto::Secret unset = tapto::resolve_secret("env:LIBTAPTO_TEST_KEY_THAT_IS_NOT_SET");
    CHECK(!unset.ok());
    CHECK(!unset.error.empty());
}

// --- encoding ---------------------------------------------------------------

void test_sanitize_utf8() {
    CHECK_EQ(tapto::sanitizeUtf8("plain ascii"), std::string("plain ascii"));
    CHECK_EQ(tapto::sanitizeUtf8("h\xC3\xA4r"), std::string("h\xC3\xA4r")); // "här"

    // A lone continuation byte becomes U+FFFD and the rest survives.
    const std::string bad = "a\x80z";
    const std::string clean = tapto::sanitizeUtf8(bad);
    CHECK_EQ(clean, std::string("a\xEF\xBF\xBDz"));

    // The result always serialises.
    bool threw = false;
    try {
        (void)json(tapto::sanitizeUtf8("\xFF\xFE")).dump();
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
}

// --- base64 -----------------------------------------------------------------

void test_base64() {
    CHECK_EQ(tapto::base64Encode({}), std::string(""));
    CHECK_EQ(tapto::base64Encode({'f'}), std::string("Zg=="));
    CHECK_EQ(tapto::base64Encode({'f', 'o'}), std::string("Zm8="));
    CHECK_EQ(tapto::base64Encode({'f', 'o', 'o'}), std::string("Zm9v"));
    CHECK_EQ(tapto::base64Encode({'f', 'o', 'o', 'b', 'a', 'r'}), std::string("Zm9vYmFy"));
}

// --- tool images ------------------------------------------------------------

void test_tool_image_context() {
    Context ctx;
    tapto::ToolImage out;
    CHECK(!tapto::takeToolImage(ctx, out));

    tapto::ToolImage in;
    in.png = {1, 2, 3};
    in.width = 4;
    in.height = 5;
    in.label = "shot";
    tapto::putToolImage(ctx, in);
    CHECK(tapto::takeToolImage(ctx, out));
    CHECK_EQ(out.png.size(), std::size_t(3));
    CHECK_EQ(out.label, std::string("shot"));
    // Taken once: a stale image must never attach to a later result.
    CHECK(!tapto::takeToolImage(ctx, out));

    // An empty image is "no image".
    tapto::putToolImage(ctx, tapto::ToolImage{});
    CHECK(!tapto::takeToolImage(ctx, out));
}

json claude_image_msg() {
    return json{{"role", "user"},
                {"content", json::array({
                     json{{"type", "tool_result"}, {"tool_use_id", "x"},
                          {"content", json::array({
                               json{{"type", "text"}, {"text", "ok"}},
                               json{{"type", "image"},
                                    {"source", {{"type", "base64"}, {"media_type", "image/png"}, {"data", "AAAA"}}}}})}}})}};
}

void test_prune_history_images() {
    // No images: nothing to prune, nothing to cache around.
    json plain = json::array({json{{"role", "user"}, {"content", "hi"}},
                              json{{"role", "assistant"}, {"content", "hello"}}});
    CHECK_EQ(tapto::pruneHistoryImages(plain, 3), std::size_t(0));
    CHECK_EQ(tapto::firstLiveImageMessage(plain), plain.size());

    // Four images, keep two: the oldest two become placeholders, and the
    // first live image is then at index 2.
    json history = json::array({claude_image_msg(), claude_image_msg(),
                                claude_image_msg(), claude_image_msg()});
    CHECK_EQ(tapto::firstLiveImageMessage(history), std::size_t(0));
    CHECK_EQ(tapto::pruneHistoryImages(history, 2), std::size_t(2));
    CHECK_EQ(tapto::firstLiveImageMessage(history), std::size_t(2));
    CHECK_EQ(history[0]["content"][0]["content"][1]["type"].get<std::string>(), std::string("text"));
    CHECK_EQ(history[3]["content"][0]["content"][1]["type"].get<std::string>(), std::string("image"));

    // Pruning again changes nothing.
    CHECK_EQ(tapto::pruneHistoryImages(history, 2), std::size_t(0));

    // Gemini shape gets the Gemini placeholder.
    json gemini = json::array({json{{"role", "user"},
                                    {"parts", json::array({json{{"inline_data", {{"mime_type", "image/png"}, {"data", "AAAA"}}}}})}}});
    CHECK_EQ(tapto::pruneHistoryImages(gemini, 0), std::size_t(1));
    CHECK(gemini[0]["parts"][0].contains("text"));
    CHECK(!gemini[0]["parts"][0].contains("inline_data"));
}

// --- tool display hook ------------------------------------------------------

void test_tool_display_name() {
    ToolSpec plain;
    plain.name = "plain";

    ToolSpec labelled;
    labelled.name = "edit";
    labelled.display = [](const json& in) { return "Edit " + in.value("path", std::string("?")); };

    ToolSpec broken;
    broken.name = "broken";
    broken.display = [](const json&) -> std::string { throw std::runtime_error("boom"); };

    std::vector<ToolSpec> tools{plain, labelled, broken};
    CHECK_EQ(getToolDisplayName(tools, "plain", json::object()), std::string("plain"));
    CHECK_EQ(getToolDisplayName(tools, "edit", json{{"path", "a.cpp"}}), std::string("Edit a.cpp"));
    CHECK_EQ(getToolDisplayName(tools, "broken", json::object()), std::string("broken"));
    CHECK_EQ(getToolDisplayName(tools, "unknown", json::object()), std::string("unknown"));
}

// --- /compact input trimming ------------------------------------------------
//
// buildTrimmedHistoryForSummary() is header-only on AiBackend, so a backend
// whose only job is to hand back a canned history drives it without a
// provider, key or network. Each of the three history shapes (OpenAI
// role:"tool", Claude tool_result blocks, Gemini functionResponse parts) must
// have its oversized results shrunk and everything else -- the small results,
// the user and assistant text, and above all the tool *calls* -- left intact.
// It must also be a pure function of the history.

class FakeBackend : public AiBackend {
public:
    json m_hist;

    void setSystemPrompt(const std::string&) override {}
    const std::string& getSystemPrompt() const override { return m_sp; }
    void setModel(const std::string&) override {}
    void setHost(const std::string&) override {}
    void setApiKeyRef(const std::string&) override {}
    void setThinkingBudget(std::optional<int>) override {}
    std::string chat(Context&, const std::string&) override { return ""; }
    void start() override { m_hist = json::array(); }
    bool hasHistory() const override { return !m_hist.empty(); }
    void loadHistory(const json& h) override { m_hist = h; }
    json getHistory() const override { return m_hist; }
    std::size_t lastInputTokens() const override { return 0; }
    void beginWithSummary(const std::string&) override { m_hist = json::array(); }

private:
    std::string m_sp;
};

void test_compact_trimmer() {
    FakeBackend be;
    // Well over and well under the 2000-character limit.
    const std::string big(5000, 'a');
    const std::string small = "ok";
    const std::string big_ph = "[omitted tool output (5000 chars)]";

    // OpenAI: oversized tool result shrunk, small kept, the call intact.
    {
        json hist = json::array();
        hist.push_back({{"role", "user"}, {"content", "read the main file"}});
        hist.push_back({{"role", "assistant"}, {"content", ""},
                        {"tool_calls", json::array({json{
                            {"id", "c1"},
                            {"function", {{"name", "str_replace_based_edit_tool"},
                                          {"arguments", "{\"path\":\"src/main.cpp\"}"}}}}})}});
        hist.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", big}});
        hist.push_back({{"role", "tool"}, {"tool_call_id", "c2"}, {"content", small}});
        hist.push_back({{"role", "assistant"}, {"content", "done, it was fine"}});
        be.loadHistory(hist);

        json t = be.buildTrimmedHistoryForSummary();
        CHECK_EQ(t.size(), std::size_t(5));
        CHECK_EQ(t[2]["content"].get<std::string>(), big_ph);
        CHECK_EQ(t[3]["content"].get<std::string>(), small);
        CHECK_EQ(t[1]["tool_calls"][0]["function"]["name"].get<std::string>(),
                 std::string("str_replace_based_edit_tool"));
        CHECK_EQ(t[1]["tool_calls"][0]["function"]["arguments"].get<std::string>(),
                 std::string("{\"path\":\"src/main.cpp\"}"));
        CHECK_EQ(t[4]["content"].get<std::string>(), std::string("done, it was fine"));
    }

    // Claude: tool_result blocks shrunk; the tool_use block preserved.
    {
        json hist = json::array();
        hist.push_back({{"role", "user"}, {"content", "look at the file"}});
        hist.push_back({{"role", "assistant"},
                        {"content", json::array({
                            json{{"type", "text"}, {"text", "reading now"}},
                            json{{"type", "tool_use"}, {"id", "u1"},
                                 {"name", "str_replace_based_edit_tool"},
                                 {"input", {{"path", "src/main.cpp"}}}}})}});
        hist.push_back({{"role", "user"},
                        {"content", json::array({
                            json{{"type", "tool_result"}, {"tool_use_id", "u1"}, {"content", big}},
                            json{{"type", "tool_result"}, {"tool_use_id", "u2"}, {"content", small}}})}});
        hist.push_back({{"role", "assistant"},
                        {"content", json::array({json{{"type", "text"}, {"text", "ok"}}})}});
        be.loadHistory(hist);

        json t = be.buildTrimmedHistoryForSummary();
        CHECK_EQ(t.size(), std::size_t(4));
        CHECK_EQ(t[2]["content"][0]["content"].get<std::string>(), big_ph);
        CHECK_EQ(t[2]["content"][1]["content"].get<std::string>(), small);
        CHECK_EQ(t[1]["content"][1]["type"].get<std::string>(), std::string("tool_use"));
        CHECK_EQ(t[1]["content"][1]["input"]["path"].get<std::string>(), std::string("src/main.cpp"));
    }

    // Gemini: functionResponse shrunk; functionCall preserved.
    {
        json hist = json::array();
        hist.push_back({{"role", "user"}, {"parts", json::array({json{{"text", "read it"}}})}});
        hist.push_back({{"role", "model"},
                        {"parts", json::array({json{{"functionCall", {
                            {"name", "str_replace_based_edit_tool"},
                            {"args", {{"path", "src/main.cpp"}}}}}}})}});
        hist.push_back({{"role", "user"},
                        {"parts", json::array({
                            json{{"functionResponse", {{"name", "str_replace_based_edit_tool"},
                                                       {"response", {{"content", big}}}}}},
                            json{{"functionResponse", {{"name", "run_command"},
                                                       {"response", {{"content", small}}}}}}})}});
        hist.push_back({{"role", "model"}, {"parts", json::array({json{{"text", "done"}}})}});
        be.loadHistory(hist);

        json t = be.buildTrimmedHistoryForSummary();
        CHECK_EQ(t.size(), std::size_t(4));
        CHECK_EQ(t[2]["parts"][0]["functionResponse"]["response"]["content"].get<std::string>(), big_ph);
        CHECK_EQ(t[2]["parts"][1]["functionResponse"]["response"]["content"].get<std::string>(), small);
        CHECK_EQ(t[1]["parts"][0]["functionCall"]["args"]["path"].get<std::string>(),
                 std::string("src/main.cpp"));
    }

    // Pure: the live history is never mutated.
    {
        json hist = json::array();
        hist.push_back({{"role", "tool"}, {"tool_call_id", "c1"}, {"content", big}});
        be.loadHistory(hist);
        const json before = be.getHistory();
        json t = be.buildTrimmedHistoryForSummary();
        CHECK(be.getHistory() == before);
        CHECK_EQ(t[0]["content"].get<std::string>(), big_ph);
    }

    // No-op: nothing oversized maps back to an identical copy.
    {
        json hist = json::array();
        hist.push_back({{"role", "user"}, {"content", "hi"}});
        hist.push_back({{"role", "assistant"}, {"content", "hello there"}});
        be.loadHistory(hist);
        CHECK(be.buildTrimmedHistoryForSummary() == be.getHistory());
    }
}

// --- policy -----------------------------------------------------------------

std::optional<std::string> entry(const std::vector<tapto::Config::Entry>& entries, const std::string& key) {
    for (const auto& e : entries) {
        if (e.first == key) return e.second;
    }
    return std::nullopt;
}

void test_policy_file() {
    fs::path path = scratch_file("policy");
    {
        std::ofstream out(path, std::ios::binary);
        out << "# managed\n"
               "provider = work\n"
               "work-provider-type = openai\n"
               "work-provider-url = https://gateway.example/v1\n"
               "work-api-key =\n"                 // empty: not a policy, dropped
               "allowed-providers = work, review\n"
               "provider = work2\n";              // last wins, as in the store
    }
    auto entries = tapto::policy_entries_from_file(path);
    fs::remove(path);

    CHECK_EQ(entry(entries, "provider").value_or(""), std::string("work2"));
    CHECK_EQ(entry(entries, "work-provider-type").value_or(""), std::string("openai"));
    CHECK(!entry(entries, "work-api-key").has_value());
    CHECK_EQ(entry(entries, "allowed-providers").value_or(""), std::string("work, review"));
    // A missing file is no policy.
    CHECK(tapto::policy_entries_from_file(scratch_file("no-such-policy")).empty());
    CHECK(tapto::policy_entries_from_file(fs::path()).empty());
}

void test_policy_provider_rules() {
    using E = std::vector<tapto::Config::Entry>;

    // No policy: everything goes.
    CHECK_EQ(tapto::provider_policy_refusal("anything", E{}), std::string());

    // An allow-list names the only acceptable providers.
    E allow = {{"allowed-providers", "work, review"}};
    CHECK_EQ(tapto::provider_policy_refusal("work", allow), std::string());
    CHECK_EQ(tapto::provider_policy_refusal("review", allow), std::string());
    std::string why = tapto::provider_policy_refusal("claude", allow);
    CHECK(why.find("'claude'") != std::string::npos);
    CHECK(why.find("work") != std::string::npos);

    // allow-user-providers = 0: only what the policy configures.
    E confined = {{"provider", "work"},
                  {"work-provider-type", "openai"},
                  {"review-provider-type", "claude"},
                  {"allow-user-providers", "0"}};
    CHECK_EQ(tapto::provider_policy_refusal("work", confined), std::string());
    CHECK_EQ(tapto::provider_policy_refusal("review", confined), std::string());
    why = tapto::provider_policy_refusal("personal", confined);
    CHECK(why.find("'personal'") != std::string::npos);
    CHECK(why.find("review") != std::string::npos);
    // A bare dialect is a provider too, and is refused unless the policy names it.
    CHECK(!tapto::provider_policy_refusal("claude", confined).empty());
    E confined_default = {{"provider", "claude"}, {"allow-user-providers", "false"}};
    CHECK_EQ(tapto::provider_policy_refusal("claude", confined_default), std::string());

    // allow-user-providers = 1 is the default behaviour, spelled out.
    E open = {{"provider", "work"}, {"allow-user-providers", "1"}};
    CHECK_EQ(tapto::provider_policy_refusal("personal", open), std::string());

    // The allow-list wins over the confinement when both are set.
    E both = {{"allowed-providers", "personal"}, {"allow-user-providers", "0"}};
    CHECK_EQ(tapto::provider_policy_refusal("personal", both), std::string());

    // A confined user may not point a permitted block elsewhere: the URL must
    // be policy's (or the vendor default), whichever key does the confining.
    using tapto::Level;
    CHECK_EQ(tapto::provider_url_policy_refusal("work-provider-url", Level::Local, E{}), std::string());
    CHECK_EQ(tapto::provider_url_policy_refusal("work-provider-url", Level::Local, open), std::string());
    CHECK_EQ(tapto::provider_url_policy_refusal("work-provider-url", Level::Policy, confined), std::string());
    why = tapto::provider_url_policy_refusal("work-provider-url", Level::Local, confined);
    CHECK(why.find("'work-provider-url'") != std::string::npos);
    CHECK(why.find("local") != std::string::npos);
    CHECK(!tapto::provider_url_policy_refusal("provider-url", Level::Global, allow).empty());
    // An empty allow-list confines nothing, as in provider_policy_refusal.
    CHECK_EQ(tapto::provider_url_policy_refusal("provider-url", Level::Global, E{{"allowed-providers", " , "}}),
             std::string());
}

void test_policy_command_rules() {
    using E = std::vector<tapto::Config::Entry>;
    // Unset, or anything but a false: the user's own commands count.
    CHECK(tapto::policy_allows_user_commands(E{}));
    CHECK(tapto::policy_allows_user_commands(E{{"allow-user-commands", "1"}}));
    CHECK(tapto::policy_allows_user_commands(E{{"allow-user-commands", "yes"}}));
    // The DWORD a policy writes, and the words a file may use.
    CHECK(!tapto::policy_allows_user_commands(E{{"allow-user-commands", "0"}}));
    CHECK(!tapto::policy_allows_user_commands(E{{"allow-user-commands", "false"}}));
    CHECK(!tapto::policy_allows_user_commands(E{{"allow-user-commands", "No"}}));
}

#ifdef _WIN32
// The registry reader, against a scratch key under HKCU\SOFTWARE (which a
// standard user may write; HKCU\SOFTWARE\Policies itself may not be).
void test_policy_registry() {
    const std::wstring subkey = L"SOFTWARE\\Centlake\\libtapto-test-policy";
    RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str()); // a previous run that died

    // Absent key: no policy, no error.
    CHECK(tapto::policy_entries_from_registry(HKEY_CURRENT_USER, subkey).empty());

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        std::cerr << "test_policy_registry: cannot create scratch key, skipping\n";
        return;
    }
    auto set_sz = [&](HKEY k, const wchar_t* name, const wchar_t* value) {
        RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                       static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    };
    set_sz(key, L"provider", L"work");
    set_sz(key, L"work-provider-type", L"openai");
    set_sz(key, L"work-api-key", L"wincred:tapto/work");
    set_sz(key, L"unset-by-policy", L"");
    // The marker the Group Policy client leaves in a key it manages.
    set_sz(key, L"**delvals.", L" ");
    DWORD zero = 0, tokens = 32000;
    RegSetValueExW(key, L"allow-user-providers", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
    RegSetValueExW(key, L"allow-user-commands", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
    RegSetValueExW(key, L"max-output-tokens", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&tokens), sizeof(tokens));
    const wchar_t multi[] = L"work\0review\0\0";
    RegSetValueExW(key, L"allowed-providers", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(multi), sizeof(multi));
    // An ADMX list element: a subkey with values named 1, 2, ... (10 sorts
    // after 2 numerically, not before it), plus the client's marker, which
    // must not turn the names non-numeric.
    HKEY list = nullptr;
    RegCreateKeyExW(key, L"extra-list", 0, nullptr, 0, KEY_WRITE, nullptr, &list, nullptr);
    set_sz(list, L"10", L"ten");
    set_sz(list, L"2", L"two");
    set_sz(list, L"1", L"one");
    set_sz(list, L"**delvals.", L" ");
    RegCloseKey(list);
    // The "settings" subkey: the template's free-form list, whose values are
    // config keys of the parent.
    HKEY settings = nullptr;
    RegCreateKeyExW(key, L"Settings", 0, nullptr, 0, KEY_WRITE, nullptr, &settings, nullptr);
    set_sz(settings, L"trace-file", L"C:\\logs\\tapto.log");
    set_sz(settings, L"review-provider-type", L"claude");
    set_sz(settings, L"**delvals.", L" ");
    RegCloseKey(settings);
    // The "commands" subkey: the organization's command allow-list, not a key.
    HKEY commands = nullptr;
    RegCreateKeyExW(key, L"commands", 0, nullptr, 0, KEY_WRITE, nullptr, &commands, nullptr);
    set_sz(commands, L"build", L"cmake --build build --config Debug");
    set_sz(commands, L"test", L"ctest --test-dir build");
    set_sz(commands, L"**delvals.", L" ");
    RegCloseKey(commands);
    RegCloseKey(key);

    auto entries = tapto::policy_entries_from_registry(HKEY_CURRENT_USER, subkey);
    auto cmds = tapto::policy_commands_from_registry(HKEY_CURRENT_USER, subkey);
    // A key without the subkey: no commands, no error.
    auto no_cmds = tapto::policy_commands_from_registry(HKEY_CURRENT_USER, subkey + L"\\extra-list");
    RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());

    CHECK_EQ(entry(entries, "provider").value_or(""), std::string("work"));
    CHECK_EQ(entry(entries, "work-provider-type").value_or(""), std::string("openai"));
    CHECK_EQ(entry(entries, "work-api-key").value_or(""), std::string("wincred:tapto/work"));
    CHECK(!entry(entries, "unset-by-policy").has_value());
    CHECK_EQ(entry(entries, "allow-user-providers").value_or(""), std::string("0"));
    CHECK_EQ(entry(entries, "max-output-tokens").value_or(""), std::string("32000"));
    CHECK_EQ(entry(entries, "allowed-providers").value_or(""), std::string("work, review"));
    CHECK_EQ(entry(entries, "extra-list").value_or(""), std::string("one, two, ten"));
    CHECK(!entry(entries, "**delvals.").has_value());
    // Flattened from the settings subkey; the subkeys themselves are not keys.
    CHECK_EQ(entry(entries, "trace-file").value_or(""), std::string("C:\\logs\\tapto.log"));
    CHECK_EQ(entry(entries, "review-provider-type").value_or(""), std::string("claude"));
    CHECK(!entry(entries, "Settings").has_value());
    CHECK(!entry(entries, "settings").has_value());
    CHECK(!entry(entries, "commands").has_value());

    CHECK_EQ(cmds.size(), size_t{2});
    CHECK_EQ(entry(cmds, "build").value_or(""), std::string("cmake --build build --config Debug"));
    CHECK_EQ(entry(cmds, "test").value_or(""), std::string("ctest --test-dir build"));
    CHECK(no_cmds.empty());

    // And the rules read what the registry produced.
    CHECK_EQ(tapto::provider_policy_refusal("review", entries), std::string());
    CHECK(!tapto::provider_policy_refusal("claude", entries).empty());
    CHECK(!tapto::policy_allows_user_commands(entries));
}
#endif

// --- provider helpers -------------------------------------------------------
//
// Only the pure one. default_provider_name(), provider_dialect() and
// resolve_api_key() read the user's real config store, which a unit test has
// no business touching.

void test_api_key_env_var() {
    CHECK_EQ(std::string(tapto::api_key_env_var("claude")), std::string("ANTHROPIC_API_KEY"));
    CHECK_EQ(std::string(tapto::api_key_env_var("openai")), std::string("OPENAI_API_KEY"));
    CHECK_EQ(std::string(tapto::api_key_env_var("gemini")), std::string("GEMINI_API_KEY"));
    CHECK(tapto::api_key_env_var("qwen36") != nullptr);
    CHECK_EQ(std::string(tapto::api_key_env_var("qwen36")), std::string());
}

void test_tool_definition_formats() {
    ToolSpec spec;
    spec.name = "t";
    spec.description = "d";
    spec.parameters = json{{"type", "object"}};

    CHECK(tool_definition_to_json(spec, ToolFormat::Claude).contains("input_schema"));
    CHECK(tool_definition_to_json(spec, ToolFormat::OpenAI).contains("parameters"));
    CHECK(tool_definition_to_json(spec, ToolFormat::Gemini).contains("parameters"));
    // MCP: the Claude shape with the schema key in camelCase.
    const json mcp = tool_definition_to_json(spec, ToolFormat::Mcp);
    CHECK(mcp.contains("inputSchema"));
    CHECK(!mcp.contains("input_schema"));
    CHECK_EQ(mcp.value("name", std::string()), std::string("t"));

    spec.claude_builtin_type = "text_editor_20250728";
    const json builtin = tool_definition_to_json(spec, ToolFormat::Claude);
    CHECK_EQ(builtin.value("type", std::string()), std::string("text_editor_20250728"));
    CHECK(!builtin.contains("input_schema"));
    // Other dialects still get the explicit schema.
    CHECK(tool_definition_to_json(spec, ToolFormat::OpenAI).contains("parameters"));
}

// --- folders ----------------------------------------------------------------

struct Tree {
    fs::path root;
    explicit Tree(const char* name) : root(scratch_file(name)) {
        fs::remove_all(root);
        fs::create_directories(root / "proj" / "src");
        fs::create_directories(root / "proj" / "build");
        fs::create_directories(root / "proj" / ".git");
        fs::create_directories(root / "other");
        write(root / "proj" / "README.md", "# proj\n\nHello world.\n");
        write(root / "proj" / "src" / "main.cpp", "int main() {\n  return 0; // needle\n}\n");
        write(root / "proj" / "src" / "blob.bin", std::string("PNG\0\0\0junk", 10));
        write(root / "proj" / "build" / "out.o", "needle in build output\n");
        write(root / "other" / "secret.txt", "not for the model\n");
    }
    ~Tree() { std::error_code ec; fs::remove_all(root, ec); }
    static void write(const fs::path& p, const std::string& content) {
        std::ofstream out(p, std::ios::binary);
        out << content;
    }
};

std::string run(const std::vector<ToolSpec>& tools, const char* name, json in) {
    Context ctx;
    for (const auto& t : tools)
        if (t.name == name) return t.executor(ctx, in);
    return "no such tool";
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

void test_folder_set_grants() {
    Tree t("folders-grant");
    tapto::FolderSet set;
    CHECK(set.empty());

    std::string label;
    CHECK_EQ(set.add((t.root / "proj").string(), &label), std::string(""));
    CHECK_EQ(label, std::string("proj"));
    CHECK_EQ(set.folders().size(), std::size_t(1));

    // Granting again, or a subfolder, adds nothing and names the cover.
    CHECK_EQ(set.add((t.root / "proj" / "src").string(), &label), std::string(""));
    CHECK_EQ(label, std::string("proj"));
    CHECK_EQ(set.folders().size(), std::size_t(1));

    // Bad grants are refused with a reason.
    CHECK(starts_with(set.add((t.root / "nope").string()), "ERROR:"));
    CHECK(starts_with(set.add((t.root / "proj" / "README.md").string()), "ERROR:"));
    CHECK(starts_with(set.add(""), "ERROR:"));

    // A second folder with the same last component gets a distinct label.
    fs::create_directories(t.root / "other" / "proj");
    CHECK_EQ(set.add((t.root / "other" / "proj").string(), &label), std::string(""));
    CHECK_EQ(label, std::string("proj-2"));

    // Read-only by default; the mode is a grant the program's editor consults.
    CHECK(set.get("proj") != nullptr);
    CHECK(!set.get("proj")->writable);
    CHECK(set.set_writable("proj", true));
    CHECK(set.get("proj")->writable);
    CHECK(!set.set_writable("nope", true));
    // Re-granting leaves the mode alone; a fresh grant can ask for it.
    CHECK_EQ(set.add((t.root / "proj").string(), &label, false), std::string(""));
    CHECK(set.get("proj")->writable);
    fs::create_directories(t.root / "rw");
    CHECK_EQ(set.add((t.root / "rw").string(), &label, true), std::string(""));
    CHECK(set.get("rw")->writable);
    CHECK(tapto::folder_prompt(set).find("read and write") != std::string::npos);
    CHECK(set.set_writable("rw", false));
    CHECK(set.set_writable("proj", false));
    CHECK(tapto::folder_prompt(set).find("read and write") == std::string::npos);
    CHECK(set.remove("rw"));

    // Remove by label and by path.
    CHECK(set.remove("proj-2"));
    CHECK(!set.remove("proj-2"));
    CHECK(set.remove((t.root / "proj").string()));
    CHECK(set.empty());
}

void test_folder_set_resolve() {
    Tree t("folders-resolve");
    tapto::FolderSet set;
    fs::path out;
    std::string err;

    // Nothing granted: every path is refused, and the message says how to fix it.
    CHECK(!set.resolve("anything", out, err));
    CHECK(contains(err, "/add-folder"));

    set.add((t.root / "proj").string());

    // Label-relative, bare label, absolute, and root-relative (one folder).
    CHECK(set.resolve("proj/src/main.cpp", out, err));
    CHECK(fs::equivalent(out, t.root / "proj" / "src" / "main.cpp"));
    CHECK(set.resolve("proj", out, err));
    CHECK(fs::equivalent(out, t.root / "proj"));
    CHECK(set.resolve((t.root / "proj" / "README.md").string(), out, err));
    CHECK(set.resolve("src/main.cpp", out, err));
    CHECK(fs::equivalent(out, t.root / "proj" / "src" / "main.cpp"));

    // Escapes: dot-dot, an absolute path elsewhere, a sibling folder.
    CHECK(!set.resolve("proj/../other/secret.txt", out, err));
    CHECK(contains(err, "outside"));
    CHECK(!set.resolve((t.root / "other" / "secret.txt").string(), out, err));
    CHECK(!set.resolve("proj/src/../../other/secret.txt", out, err));

    // A prefix that merely starts with the root's name is not inside it.
    fs::create_directories(t.root / "project-evil");
    CHECK(!set.resolve((t.root / "project-evil").string(), out, err));

    // display() gives the model-facing form back.
    CHECK_EQ(set.display(t.root / "proj" / "src" / "main.cpp"), std::string("proj/src/main.cpp"));
    CHECK_EQ(set.display(t.root / "proj"), std::string("proj"));

    // With two folders a bare relative path is ambiguous and says so.
    set.add((t.root / "other").string());
    CHECK(!set.resolve("src/main.cpp", out, err));
    CHECK(contains(err, "ambiguous"));
    CHECK(set.resolve("other/secret.txt", out, err));
}

void test_folder_tools() {
    Tree t("folders-tools");
    tapto::FolderSet set;
    auto tools = tapto::folder_tools(set);
    CHECK_EQ(tools.size(), std::size_t(4));

    // Before any grant every tool explains itself rather than failing oddly.
    CHECK(contains(run(tools, "list_folders", json::object()), "No folders"));
    CHECK(starts_with(run(tools, "read_file", json{{"path", "x"}}), "ERROR:"));

    set.add((t.root / "proj").string());
    CHECK(contains(run(tools, "list_folders", json::object()), "proj"));

    // list_files skips build/ and .git/, shows sizes, honours the glob.
    const std::string listing = run(tools, "list_files", json::object());
    CHECK(contains(listing, "proj/README.md"));
    CHECK(contains(listing, "proj/src/main.cpp"));
    CHECK(!contains(listing, "out.o"));
    CHECK(contains(listing, "bytes"));
    const std::string cpp_only = run(tools, "list_files", json{{"pattern", "*.cpp"}});
    CHECK(contains(cpp_only, "main.cpp"));
    CHECK(!contains(cpp_only, "README"));
    CHECK(contains(run(tools, "list_files", json{{"pattern", "*.zzz"}}), "No files"));

    // read_file: numbered lines, slices, binary detection, directory refusal.
    const std::string whole = run(tools, "read_file", json{{"path", "proj/src/main.cpp"}});
    CHECK(contains(whole, "1|int main() {"));
    CHECK(contains(whole, "3|}"));
    CHECK(contains(whole, "(3 lines)"));
    const std::string slice = run(tools, "read_file", json{{"path", "proj/src/main.cpp"}, {"start_line", 2}, {"end_line", 2}});
    CHECK(contains(slice, "2|  return 0;"));
    CHECK(!contains(slice, "1|int"));
    CHECK(contains(slice, "showing 2-2"));
    CHECK(contains(run(tools, "read_file", json{{"path", "proj/src/blob.bin"}}), "binary"));
    CHECK(starts_with(run(tools, "read_file", json{{"path", "proj/src"}}), "ERROR:"));
    CHECK(starts_with(run(tools, "read_file", json{{"path", "proj/nope.txt"}}), "ERROR:"));
    CHECK(starts_with(run(tools, "read_file", json{{"path", "proj/../other/secret.txt"}}), "ERROR:"));
    CHECK(starts_with(run(tools, "read_file", json{{"path", "proj/src/main.cpp"}, {"start_line", 9}}), "ERROR:"));

    // search_files: finds the needle in src, not in build; binary skipped.
    const std::string found = run(tools, "search_files", json{{"query", "needle"}});
    CHECK(contains(found, "proj/src/main.cpp"));
    CHECK(contains(found, "2: "));
    CHECK(!contains(found, "out.o"));
    CHECK(contains(run(tools, "search_files", json{{"query", "absent-string"}}), "No files"));
    CHECK(starts_with(run(tools, "search_files", json{{"query", ""}}), "ERROR:"));

    // The prompt paragraph names the folder; empty when nothing is granted.
    CHECK(contains(tapto::folder_prompt(set), "proj"));
    set.clear();
    CHECK(tapto::folder_prompt(set).empty());
    // Tools built earlier see the cleared set.
    CHECK(contains(run(tools, "list_folders", json::object()), "No folders"));
}

void test_fs_helpers() {
    CHECK(tapto::wildcard_match("*.cpp", "main.cpp"));
    CHECK(!tapto::wildcard_match("*.cpp", "main.h"));
    CHECK(tapto::wildcard_match("ma?n.*", "main.cpp"));
    CHECK(tapto::wildcard_match("*", ""));
    CHECK(tapto::is_noise_dir(".git"));
    CHECK(tapto::is_noise_dir("build-ide"));
    CHECK(!tapto::is_noise_dir("builder"));
    CHECK_EQ(tapto::content_lines("a\nb\n").size(), std::size_t(2));
    CHECK_EQ(tapto::content_lines("a\r\nb").size(), std::size_t(2));
    CHECK_EQ(tapto::content_lines("").size(), std::size_t(0));
    CHECK(tapto::looks_binary(std::string("ab\0cd", 5)));
    CHECK(!tapto::looks_binary("plain"));
    CHECK(contains(tapto::cap_output(std::string(100, 'x'), 10), "truncated"));
    CHECK_EQ(tapto::cap_output("short", 10), std::string("short"));
}

// --- certificates ------------------------------------------------------------

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void test_certificates() {
    const fs::path dir = scratch_file("certs");
    fs::remove_all(dir);

    // First call: everything is created.
    tapto::CertResult a = tapto::ensure_certificates(dir);
    CHECK(a.ok);
    if (!a.ok) { std::cerr << a.error << "\n"; return; }
    CHECK(a.ca_created);
    CHECK(a.leaf_created);
    CHECK(fs::exists(a.paths.ca_file));
    CHECK(fs::exists(a.paths.ca_key_file));
    CHECK(fs::exists(a.paths.cert_file));
    CHECK(fs::exists(a.paths.key_file));
    CHECK(a.leaf_days_left >= 89 && a.leaf_days_left <= 90);
    const auto ca_days = tapto::cert_days_left(a.paths.ca_file);
    CHECK(ca_days && *ca_days >= 3649 && *ca_days <= 3650);

    // PEM, and the leaf chains to the CA.
    CHECK(slurp(a.paths.cert_file).rfind("-----BEGIN CERTIFICATE-----", 0) == 0);
    CHECK(slurp(a.paths.key_file).find("PRIVATE KEY") != std::string::npos);
    CHECK(tapto::cert_signed_by(a.paths.cert_file, a.paths.ca_file));
    CHECK(!tapto::cert_signed_by(a.paths.ca_file, a.paths.cert_file));
    CHECK_EQ(tapto::cert_fingerprint(a.paths.ca_file).size(), std::size_t(40));
    CHECK(tapto::cert_fingerprint(a.paths.ca_file) != tapto::cert_fingerprint(a.paths.cert_file));

    // Second call: nothing changes.
    const std::string leaf_before = slurp(a.paths.cert_file);
    tapto::CertResult b = tapto::ensure_certificates(dir);
    CHECK(b.ok);
    CHECK(!b.ca_created);
    CHECK(!b.leaf_created);
    CHECK_EQ(slurp(b.paths.cert_file), leaf_before);

    // A leaf with fewer days left than the renewal threshold is renewed; the
    // CA is left alone and the new leaf still chains to it. A threshold above
    // the leaf's own lifetime forces one, which is how a short leaf gets made.
    tapto::CertResult c = tapto::ensure_certificates(dir, /*leaf_days=*/5, 3650, /*renew_before=*/100);
    CHECK(c.ok);
    CHECK(!c.ca_created);
    CHECK(c.leaf_created);
    CHECK(c.leaf_days_left <= 5);
    tapto::CertResult d = tapto::ensure_certificates(dir, 90, 3650, 30);
    CHECK(d.leaf_created); // the 5-day leaf is under the 30-day threshold
    CHECK(!d.ca_created);
    CHECK(tapto::cert_signed_by(d.paths.cert_file, d.paths.ca_file));

    // A CA replaced underneath the leaf makes the leaf invalid, so it is re-issued.
    fs::remove(a.paths.ca_file);
    fs::remove(a.paths.ca_key_file);
    tapto::CertResult e = tapto::ensure_certificates(dir);
    CHECK(e.ok);
    CHECK(e.ca_created);
    CHECK(e.leaf_created);
    CHECK(tapto::cert_signed_by(e.paths.cert_file, e.paths.ca_file));

    // Inspection of garbage is a clean "no".
    const fs::path junk = dir / "junk.crt";
    std::ofstream(junk) << "not a certificate";
    CHECK(!tapto::cert_days_left(junk).has_value());
    CHECK(!tapto::cert_signed_by(junk, e.paths.ca_file));
    CHECK(tapto::cert_fingerprint(junk).empty());
    CHECK(!tapto::cert_days_left(dir / "missing.crt").has_value());

    // The trust query must not throw for an untrusted CA (it is not installed
    // by any test); installation itself is a dialog and is checked by hand.
    CHECK(!tapto::ca_is_trusted(e.paths.ca_file) || true);
    CHECK(!tapto::manual_trust_command(e.paths.ca_file).empty());

    fs::remove_all(dir);
}

} // namespace

int main() {
    test_config_roundtrip();
    test_config_preserves_file();
    test_secret_literal_and_env();
    test_sanitize_utf8();
    test_base64();
    test_tool_image_context();
    test_prune_history_images();
    test_tool_display_name();
    test_compact_trimmer();
    test_api_key_env_var();
    test_policy_file();
    test_policy_provider_rules();
    test_policy_command_rules();
#ifdef _WIN32
    test_policy_registry();
#endif
    test_tool_definition_formats();
    test_fs_helpers();
    test_folder_set_grants();
    test_folder_set_resolve();
    test_folder_tools();
    test_certificates();

    if (g_failures) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "libtapto: all checks passed\n";
    return 0;
}
