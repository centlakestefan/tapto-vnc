// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// tapto-vnc: lets a model drive a remote screen over VNC.

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#endif

#include "tapto/aibackend.h"
#include "tapto/aiconfig.h"
#include "tapto/claude.h"
#include "tapto/computer_tools.h"
#include "tapto/config.h"
#include "tapto/context.h"
#include "tapto/gemini.h"
#include "tapto/input_map.h"
#include "tapto/log.h"
#include "tapto/openai.h"
#include "tapto/paths.h"
#include "tapto/ui.h"
#include "tapto/vmware_console.h"
#include "tapto/vnc_session.h"

namespace {

#ifdef _WIN32
// Windows hands a native program its arguments in the system ANSI code page,
// regardless of what the console code page is set to. A task written in
// Swedish therefore arrives as cp1252 bytes, which are not valid UTF-8, and
// nlohmann::json refuses to serialise them: the run dies with
// "invalid UTF-8 byte at index N" before a single request is sent.
//
// The real command line is kept by Windows in UTF-16 and is lossless, so take
// it from there and convert it ourselves. Everything downstream — the config
// store, the JSON bodies, the trace log — is UTF-8 throughout.
std::string toUtf8(const wchar_t* text) {
    if (!text) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

// Returns the command line as UTF-8, or an empty vector if Windows declines to
// parse it — in which case the caller keeps the original argv rather than
// losing the arguments altogether.
std::vector<std::string> utf8Arguments() {
    int wideCount = 0;
    wchar_t** wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideCount);
    if (!wideArgv) return {};

    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(wideCount));
    for (int i = 0; i < wideCount; ++i) out.push_back(toUtf8(wideArgv[i]));
    LocalFree(wideArgv);
    return out;
}
#endif

// Per-dialect defaults. The model matters most: driving a screen needs vision
// and solid tool use, so each default is that vendor's current general model
// rather than its cheapest. These apply to a provider block that names a
// dialect and nothing else, which is what makes `--provider claude` work
// against an empty config.
struct ProviderDefaults {
    const char* host;
    const char* model;
    const char* apiKeyEnv;
};

ProviderDefaults defaultsFor(const std::string& dialect) {
    if (dialect == "openai") {
        return {"https://api.openai.com", "gpt-4o", "OPENAI_API_KEY"};
    }
    if (dialect == "gemini") {
        return {"https://generativelanguage.googleapis.com", "gemini-2.0-flash", "GEMINI_API_KEY"};
    }
    return {"https://api.anthropic.com", "claude-opus-5", "ANTHROPIC_API_KEY"};
}

// The tapto-code config store (~/.tapto/config and friends), so an existing
// tapto-code setup works here without re-entering the API key. Scopes are
// merged with git-style precedence: local overrides global overrides system.
class Settings {
public:
    Settings() {
        for (tapto::Level level : {tapto::Level::System, tapto::Level::Global, tapto::Level::Local}) {
            try {
                m_scopes.push_back(tapto::Config::load(tapto::config_path(level)));
            } catch (const std::exception&) {
                // An unreadable scope must not stop the run; the remaining
                // scopes and the environment can still supply what we need.
                m_scopes.emplace_back();
            }
        }
    }

    std::optional<std::string> get(const std::string& key) const {
        for (auto it = m_scopes.rbegin(); it != m_scopes.rend(); ++it) {
            if (auto value = it->get(key)) {
                if (!value->empty()) return value;
            }
        }
        return std::nullopt;
    }

    std::string valueOr(const std::string& key, const std::string& fallback) const {
        if (auto value = get(key)) return *value;
        return fallback;
    }

    int intOr(const std::string& key, int fallback) const {
        if (auto value = get(key)) {
            try { return std::stoi(*value); } catch (...) {}
        }
        return fallback;
    }

    // Every provider block declared in the store, found by its
    // `<name>-provider-type` key. Only used to name the alternatives when
    // someone asks for a provider that is not configured — a list of what does
    // exist is worth more than a list of what is allowed.
    std::vector<std::string> providerNames() const {
        static const std::string kSuffix = "-provider-type";
        std::vector<std::string> names;
        for (const auto& scope : m_scopes) {
            for (const auto& entry : scope.entries()) {
                const std::string& key = entry.first;
                if (key.size() <= kSuffix.size()) continue;
                if (key.compare(key.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) continue;
                std::string name = key.substr(0, key.size() - kSuffix.size());
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    names.push_back(std::move(name));
                }
            }
        }
        return names;
    }

private:
    std::vector<tapto::Config> m_scopes;
};

constexpr const char* kDefaultModel = "claude-opus-5";
constexpr const char* kAnthropicHost = "https://api.anthropic.com";

// Parked: deliberately not part of kSystemPrompt right now.
//
// Driving a wizard from the keyboard is markedly more reliable than clicking
// its buttons — Return activates the highlighted default button, which takes
// coordinates out of the step altogether, and it cannot hit the neighbouring
// button by mistake the way a click can. It is switched off while we are
// measuring how accurately a model can click, precisely because it hands the
// model a way around the thing under test.
//
// To re-enable, paste this line back into kSystemPrompt's "How to work" list.
// It cannot be commented out in place: kSystemPrompt is a raw string literal,
// so anything inside it is prompt text, not source.
[[maybe_unused]] const char* kKeyboardNavGuidance =
    "- When a dialog, wizard or installer is in front of you, drive it from the keyboard "
    "rather than by clicking. It is far more reliable than aiming at small buttons that "
    "look alike, and it cannot select the neighbouring one by mistake. Return presses the "
    "dialog's default button — the one drawn with a highlighted border, usually Next, "
    "Install or Finish. Escape cancels. Tab moves between controls and Space ticks the "
    "focused checkbox. Alt plus the underlined letter in a label activates that control "
    "where one is shown. Check the screenshot afterwards as usual: pressing Return when "
    "the wrong control has focus does something else.";

// Written for a model that can only perceive the machine through screenshots.
// The instructions that matter are the ones covering that gap: it cannot see
// between actions, and its idea of the screen goes stale the moment it acts.
const char* kSystemPrompt = R"(You are operating a computer through its screen, keyboard and mouse over a VNC connection.

You cannot see the machine except through screenshots. Every action tool returns a fresh screenshot showing the result of what you just did, so you always have current evidence to work from.

How to work:
- Take a screenshot first if you don't already have a current one. Never act on an assumption about what is on screen.
- Look at what the screenshot actually shows before deciding. Read window titles, button labels and field contents rather than relying on where things usually are.
- There is one coordinate system: actual pixels of the whole screen, (0,0) at the top left. Every tool uses it. Never give a normalised 0-1000 or 0-1 fraction.
- A full screenshot has to show the whole screen at once, so small or tightly packed things — rows in a file list, items in a menu, buttons along the bottom of a dialog — are hard to tell apart and easy to misjudge. Do not estimate their position from the full screenshot. Use vnc_zoom on the area around them instead: it returns that area enlarged, with a numbered ruler along the top and left edge and a grid drawn from it, all labelled in screen coordinates.
- Read the target's position off that grid rather than judging it. Find which gridlines bracket the thing you want, read their numbers from the rulers, and give that position to vnc_click. This is reading, not estimating — if you find yourself guessing a coordinate, zoom in closer instead, because a smaller region means a finer grid.
- If a click lands in the wrong place, do not adjust it by guesswork and do not repeat it. Zoom in tighter on the target and read its coordinates again.
- Zooming is not only about aiming, it is also how you make sure you have the right thing. Neighbouring entries in a list often have names that differ only slightly — the same word with a version number, a suffix or a different extension — and adjacent rows sit about twenty pixels apart, so opening the wrong one is easy and looks like success afterwards. Zoom in close enough to read the whole row rather than a name alone: the other columns, such as type and size, are usually what tells two similar entries apart. Confirm you have the right one before you open it.
- Aim at the largest part of whatever you are clicking, not its smallest visible detail. A big target absorbs a small error; a small one does not.
- This matters most for checkboxes and radio buttons. The little square or circle is only a few pixels across, but the text label beside it belongs to the same control and clicking the text toggles it just the same — and the label is usually many times wider. So to tick "I accept the terms in the License Agreement", click the middle of that sentence rather than trying to hit the box. The same goes for a row in a list: click its name, which is wide, rather than its icon, which is not.
- Afterwards, look at the screenshot and confirm the box actually changed state. A label is not always wired to its control, so if the click did nothing, zoom in and aim at the box itself.
- Click a text field before typing into it. Use vnc_type for text, vnc_key for named keys and shortcuts.
- Take particular care with the buttons along the bottom of a wizard. Back, Next, Install and Cancel sit side by side, are the same size and shape, and differ only by their label — and clicking Back undoes a step you have already completed. Zoom in close enough to read the label on the button you are about to hit, confirm it is the one you want, and afterwards check that the page moved forward rather than back.
- After an action that starts something slow — launching an application, loading a page, an installer step — use vnc_wait and look again rather than assuming it finished.
- If an action did not do what you expected, say so and look at the screen again. Do not repeat the same click hoping for a different result.
- Small text is drawn with subpixel antialiasing, so individual letters often contain strongly coloured pixels — orange, blue or purple fringes — even where the text is plain black. That is how the remote machine renders fonts, not a property of the document and not a fault. Read the words; do not report per-word colour differences in body text, and do not investigate them further — zoom is for working out where something is, not for studying how letters are drawn.

Be careful with actions that are hard to undo: deleting files, overwriting data, confirming destructive dialogs, changing system settings. If you are about to do something irreversible that the user did not clearly ask for, stop and ask them first.

Never guess a password, PIN or other credential. If the machine asks you to sign in and you have not been told what to enter, say so and stop rather than trying something plausible. Repeated wrong attempts can lock an account, and a guess that happens to work is not something the user authorised. The same goes for any other secret you were not given.

Your private reasoning is not kept between steps — only your replies, the tool results and the most recent screenshots are. So when you finish something that matters, say so in your reply rather than only thinking it: "the installer has finished and I clicked Finish" is a note to yourself as much as to the user, and it is the only part of that thought you will still have later.

When the task you were given is complete, say so and stop. Do not start it over. Before repeating any substantial action — running an installer, creating an account, sending something — check whether you have already done it: look at your earlier replies and at what is on screen. Doing an installation twice is not harmless, and a screen that looks like the starting point is not evidence that nothing has happened.

Tell the user what you observe and what you are doing, in plain language. When a task is done, say what the final state of the screen is.)";

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [connection options] [agent options] [task]\n\n"
        << "Direct VNC:\n"
        << "  --host <host>       VNC server host (default: localhost)\n"
        << "  --display <n>       Display number; port is 5900+n\n"
        << "  --password <pw>     VNC password\n\n"
        << "VMware console (WebMKS) — naming a VM selects this mode:\n"
        << "  --vm <name>         VM name (exact, case-sensitive)\n"
        << "  --vcenter <host>    Overrides config key vcenter-host\n"
        << "  --user <name>       Overrides $TAPTO_VCENTER_USER / vcenter-user\n"
        << "  --insecure          Skip TLS verification (or vcenter-insecure=true)\n"
        << "  --resolution <WxH>  Set the guest's screen size before connecting,\n"
        << "                      e.g. 1280x1024 (config key resolution). Needs\n"
        << "                      VMware Tools; the guest takes ~10s to apply it.\n"
        << "                      Use this when a VM opened in the vSphere web\n"
        << "                      client is stuck at the browser's viewport size.\n"
        << "  Password from $TAPTO_VCENTER_PASSWORD or config key vcenter-password.\n\n"
        << "Agent:\n"
        << "  --provider <name>   Which configured provider to use (config key\n"
        << "                      provider). A name is anything you like — qwen36,\n"
        << "                      gemma4 — defined by <name>-provider-type in the\n"
        << "                      config; claude, openai and gemini work with no\n"
        << "                      config at all.\n"
        << "  --provider-url <u>  API base URL; point at a local OpenAI-compatible\n"
        << "                      server, e.g. http://host:8080 (config provider-url)\n"
        << "  --model <id>        Model (default: " << kDefaultModel << ")\n"
        << "  --effort <level>    low|medium|high|xhigh|max (default: high)\n"
        << "  --thinking <mode>   on|off|default (config key thinking). Asks the\n"
        << "                      model to reason before answering. On an\n"
        << "                      OpenAI-compatible server this is a per-request\n"
        << "                      switch, so a model that stays silent by default\n"
        << "                      needs it regardless of how the server was\n"
        << "                      started. Costs tokens and latency per step.\n"
        << "                      Gemini 3 has no true off: 'off' asks for the\n"
        << "                      least thinking it allows.\n"
        << "  --max-steps <n>     Tool-call limit per reply (default: 60)\n"
        << "  --trace <path>      Append request/response diagnostics here\n"
        << "  --quiet             Don't print the model's intermediate reasoning\n"
        << "  --screenshots <dir> Save every screenshot the agent takes into <dir>\n"
        << "                      (config key screenshot-dir)\n"
        << "  --color <mode>      auto|always|never (config key color). Auto styles\n"
        << "                      only when the terminal will render ANSI.\n"
        << "  --require-zoom <m>  on|off (config key require-zoom, default off).\n"
        << "                      Refuses a click unless vnc_zoom has been called\n"
        << "                      since the last action AND the point lies inside\n"
        << "                      that region. Stops a model guessing positions\n"
        << "                      from a full screenshot; costs one extra step per\n"
        << "                      click. Worth it for local models.\n"
        << "  --coord-span <n>    Model reports coordinates on a 0..n grid instead\n"
        << "                      of pixels (0 = pixels; config coordinate-span)\n"
        << "  --layout <name>     Keyboard layout of the REMOTE machine\n"
        << "                      (config key keyboard-layout; default us)\n"
        << "  --altcode <mode>    on|off|auto — Alt+numpad fallback for characters\n"
        << "                      the layout cannot reach. Windows guests only;\n"
        << "                      needs NumLock on. (config key altcode-fallback)\n"
        << "  --type-test <text>  Type <text> into whatever has focus, save\n"
        << "                      layout-test.png, and exit. No model involved —\n"
        << "                      use it to check a layout against a real guest.\n\n"
        << "A provider is a named block in the config store, so several backends —\n"
        << "including two local servers speaking the same API — coexist in one file:\n\n"
        << "  qwen36-provider-type = openai        gemma4-provider-type = openai\n"
        << "  qwen36-provider-url  = http://box:8000   gemma4-provider-url = http://box:8081\n"
        << "  qwen36-model         = Qwen3.6-27B   gemma4-model         = gemma-4-31b\n"
        << "  qwen36-api-key       = local         gemma4-api-key       = local\n\n"
        << "Then: --provider qwen36, or set 'provider = qwen36' as the default.\n"
        << "-provider-type names the API shape to speak, not the model. The unscoped\n"
        << "model, provider-url and api-key apply only to the default provider,\n"
        << "so a local endpoint's URL is never sent to a hosted one, or its key to\n"
        << "a local one.\n\n"
        << "Settings resolve in this order: CLI flag, environment, then the shared\n"
        << "tapto config store (~/.tapto/config, same one tapto-code uses), then a\n"
        << "default. The API key comes from $ANTHROPIC_API_KEY or the store's\n"
        << "api-key; model, effort, provider-url, max-output-tokens,\n"
        << "max-tool-iterations, print-cot and trace-file are read from it too.\n\n"
        << "Any remaining arguments are sent as the first task. With none,\n"
        << "tapto-vnc starts an interactive prompt. Type /exit to quit.\n";
}

const char* option(int argc, char** argv, int& i, const char* flag) {
    if (std::string(argv[i]) != flag) return nullptr;
    if (i + 1 >= argc) {
        std::cerr << "ERROR: " << flag << " requires a value\n";
        std::exit(2);
    }
    return argv[++i];
}

// Parses "1280x1024". Rejects anything with trailing rubbish rather than
// silently taking a prefix, so a typo is reported instead of acted on.
bool parseResolution(const std::string& text, int& width, int& height) {
    const size_t sep = text.find_first_of("xX");
    if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size()) return false;
    try {
        size_t usedWidth = 0, usedHeight = 0;
        width  = std::stoi(text.substr(0, sep), &usedWidth);
        height = std::stoi(text.substr(sep + 1), &usedHeight);
        if (usedWidth != sep || usedHeight != text.size() - sep - 1) return false;
    } catch (const std::exception&) {
        return false;
    }
    return width >= 640 && width <= 7680 && height >= 480 && height <= 4320;
}

std::string fromEnv(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Kept alive for the whole of main(), since argv is repointed at it.
    std::vector<std::string> wideArgs = utf8Arguments();
    std::vector<char*> wideArgv;
    if (!wideArgs.empty()) {
        for (std::string& arg : wideArgs) wideArgv.push_back(arg.data());
        wideArgv.push_back(nullptr);
        argc = static_cast<int>(wideArgs.size());
        argv = wideArgv.data();
    }
    // So a reply containing non-ASCII prints as text rather than mojibake.
    SetConsoleOutputCP(CP_UTF8);
#endif

    tapto::VncOptions vnc;
    tapto::VCenterCredentials vcenter;
    // Left empty so config can supply them; CLI flags win when present.
    std::string vmName, model, effort, trace, task, provider, layout, typeTest, altcode, color,
                screenshotDir, providerUrl, coordSpan, resolution, thinking, requireZoom;
    int maxSteps = 0;
    int resolutionWidth = 0, resolutionHeight = 0;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        if (arg == "--dump-tools") {
            // Print the tool definitions exactly as they go on the wire, so a
            // schema problem can be seen without spending an API call.
            nlohmann::json out = nlohmann::json::array();
            for (const auto& tool : tapto::makeComputerTools()) {
                out.push_back(tool_definition_to_json(tool, ToolFormat::Claude));
            }
            std::cout << out.dump(2) << "\n";
            return 0;
        }
        if (arg == "--insecure") { vcenter.insecure = true; continue; }
        if (arg == "--quiet")    { quiet = true; continue; }
        if (const char* v = option(argc, argv, i, "--host"))      { vnc.host = v; continue; }
        if (const char* v = option(argc, argv, i, "--display"))   { vnc.display = static_cast<uint16_t>(std::atoi(v)); continue; }
        if (const char* v = option(argc, argv, i, "--password"))  { vnc.password = v; continue; }
        if (const char* v = option(argc, argv, i, "--vcenter"))   { vcenter.host = v; continue; }
        if (const char* v = option(argc, argv, i, "--vm"))        { vmName = v; continue; }
        if (const char* v = option(argc, argv, i, "--user"))      { vcenter.username = v; continue; }
        if (const char* v = option(argc, argv, i, "--resolution")) { resolution = v; continue; }
        if (const char* v = option(argc, argv, i, "--screenshots")) { screenshotDir = v; continue; }
        if (const char* v = option(argc, argv, i, "--color"))     { color = v; continue; }
        if (const char* v = option(argc, argv, i, "--altcode"))   { altcode = v; continue; }
        if (const char* v = option(argc, argv, i, "--coord-span")) { coordSpan = v; continue; }
        if (const char* v = option(argc, argv, i, "--layout"))    { layout = v; continue; }
        if (const char* v = option(argc, argv, i, "--type-test")) { typeTest = v; continue; }
        if (const char* v = option(argc, argv, i, "--provider-url")) { providerUrl = v; continue; }
        if (const char* v = option(argc, argv, i, "--provider"))  { provider = v; continue; }
        if (const char* v = option(argc, argv, i, "--model"))     { model = v; continue; }
        if (const char* v = option(argc, argv, i, "--effort"))    { effort = v; continue; }
        if (const char* v = option(argc, argv, i, "--thinking"))  { thinking = v; continue; }
        if (const char* v = option(argc, argv, i, "--require-zoom")) { requireZoom = v; continue; }
        if (const char* v = option(argc, argv, i, "--max-steps")) { maxSteps = std::atoi(v); continue; }
        if (const char* v = option(argc, argv, i, "--trace"))     { trace = v; continue; }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "ERROR: unknown argument '" << arg << "'\n\n";
            usage(argv[0]);
            return 2;
        }
        // Everything else is the task text.
        if (!task.empty()) task += " ";
        task += arg;
    }

    // Resolution order for every setting: CLI flag, then environment, then the
    // config store, then a built-in default.
    const Settings settings;

    // A provider has a *name* and a *dialect*, and they are not the same thing.
    //
    // The name selects a block of config keys and is free-form: qwen36, gemma4,
    // work-claude. The dialect is one of the three request shapes this program
    // can speak, named by that block's `-provider-type`. Two local servers can
    // therefore both be OpenAI-compatible and still be told apart, which they
    // could not when the name *was* the dialect and one store held at most one
    // configuration per vendor.
    //
    // `provider-type` doubles as the legacy default: a store that says
    // `provider-type = claude` names the block "claude", whose dialect is
    // "claude" because that is also a dialect name. Nothing needs rewriting.
    const std::string defaultProvider =
        settings.valueOr("provider", settings.valueOr("provider-type", "claude"));
    if (provider.empty()) provider = defaultProvider;

    const bool isDialectName =
        provider == "claude" || provider == "openai" || provider == "gemini";
    const std::string dialect =
        settings.valueOr(provider + "-provider-type", isDialectName ? provider : "");
    if (dialect.empty()) {
        std::cerr << "ERROR: unknown provider '" << provider << "'.";
        const std::vector<std::string> names = settings.providerNames();
        if (!names.empty()) {
            std::cerr << " Configured:";
            for (const auto& name : names) std::cerr << " " << name;
            std::cerr << ".";
        }
        std::cerr << "\nName a provider by adding '" << provider
                  << "-provider-type = claude|openai|gemini' to the tapto config store, "
                     "or use claude, openai or gemini directly.\n";
        return 2;
    }
    if (dialect != "claude" && dialect != "openai" && dialect != "gemini") {
        std::cerr << "ERROR: '" << provider << "-provider-type' is '" << dialect
                  << "'; expected claude, openai or gemini. That key names the API "
                     "shape to speak, not the model.\n";
        return 2;
    }
    const ProviderDefaults defaults = defaultsFor(dialect);

    // The block's own key first. It is the most specific thing the user wrote
    // and it is the only one that can be right when two blocks share a dialect
    // — and, more sharply, an environment variable winning here would send a
    // real vendor key to whatever `<name>-provider-url` points at, which for a
    // local server means writing it into somebody's log.
    std::string apiKey = settings.valueOr(provider + "-api-key", "");
    if (apiKey.empty()) apiKey = fromEnv(defaults.apiKeyEnv);
    if (apiKey.empty() && provider == defaultProvider) {
        // The unscoped api-key belongs to the default provider only. Otherwise
        // one vendor's key would be handed to another.
        apiKey = settings.valueOr("api-key", "");
    }
    if (apiKey.empty()) {
        std::cerr << "ERROR: no API key for provider '" << provider << "'. Add '"
                  << provider << "-api-key' to the tapto config store, or set "
                  << defaults.apiKeyEnv;
        if (provider != defaultProvider) {
            std::cerr << " (the unscoped api-key belongs to '" << defaultProvider
                      << "', so it is not used here)";
        }
        std::cerr << ".\nA local server that checks no key still needs one; any "
                     "non-empty value will do.\n";
        return 2;
    }

    // Only Claude accepts output_config.effort; sending it elsewhere is an
    // unknown field.
    if (dialect != "claude") effort.clear();

    if (screenshotDir.empty()) screenshotDir = settings.valueOr("screenshot-dir", "");
    if (color.empty()) color = settings.valueOr("color", "auto");
    if (color == "auto")        tapto::ui::set_color_mode(tapto::ui::ColorMode::Auto);
    else if (color == "always") tapto::ui::set_color_mode(tapto::ui::ColorMode::Always);
    else if (color == "never")  tapto::ui::set_color_mode(tapto::ui::ColorMode::Never);
    else {
        std::cerr << "ERROR: --color expects auto, always or never\n";
        return 2;
    }

    // Some vision models report positions on a normalised grid rather than in
    // pixels; rescale them here rather than letting every click miss.
    if (coordSpan.empty()) coordSpan = settings.valueOr("coordinate-span", "0");
    try { tapto::setCoordinateSpan(std::stoi(coordSpan)); }
    catch (const std::exception&) { std::cerr << "ERROR: --coord-span expects a number\n"; return 2; }

    // Keyboard layout of the *remote* machine, not this one.
    if (layout.empty()) layout = settings.valueOr("keyboard-layout", "us");
    if (altcode.empty()) altcode = settings.valueOr("altcode-fallback", "auto");
    if (altcode != "on" && altcode != "off" && altcode != "auto") {
        std::cerr << "ERROR: --altcode expects on, off or auto\n";
        return 2;
    }
    if (!tapto::input::setRemoteLayout(layout)) {
        std::cerr << "ERROR: unknown keyboard layout '" << layout << "'. Known layouts:";
        for (const std::string& name : tapto::input::availableLayouts()) {
            std::cerr << " " << name;
        }
        std::cerr << "\n";
        return 2;
    }
    // "auto" enables the fallback only where it is safe and useful: a non-US
    // layout strands some characters, and Alt codes are a Windows mechanism.
    // The guest OS is not knowable from here, so "auto" assumes Windows —
    // which is what a VMware console usually is — and "off" is the escape
    // hatch for a Linux guest, where Alt+digit is a desktop shortcut.
    tapto::input::setAltCodeFallback(altcode == "on" ||
                                     (altcode == "auto" && layout != "us"));

    if (requireZoom.empty()) requireZoom = settings.valueOr("require-zoom", "off");
    if (requireZoom != "on" && requireZoom != "off") {
        std::cerr << "ERROR: --require-zoom expects on or off\n";
        return 2;
    }
    tapto::setRequireZoom(requireZoom == "on");

    if (thinking.empty()) thinking = settings.valueOr("thinking", "default");
    if (thinking != "on" && thinking != "off" && thinking != "default") {
        std::cerr << "ERROR: --thinking expects on, off or default\n";
        return 2;
    }

    if (model.empty())    model    = settings.valueOr(provider + "-model", "");
    if (model.empty() && provider == defaultProvider) model = settings.valueOr("model", "");
    if (model.empty())    model    = defaults.model;
    if (effort.empty())   effort   = settings.valueOr("effort", "high");
    if (trace.empty())    trace    = settings.valueOr("trace-file", "");
    if (maxSteps <= 0)    maxSteps = settings.intOr("max-tool-iterations", 60);
    const int maxOutputTokens = settings.intOr("max-output-tokens", 16000);
    if (!quiet && settings.valueOr("print-cot", "true") == "false") quiet = true;

    // A local OpenAI-compatible server (llama.cpp, vLLM, LM Studio) is reached
    // by pointing this at it, e.g. --provider-url http://host:8080.
    //
    // Scoped to the provider block, like the key and the model, and for the
    // same reason: a store holding a url and model for a local endpoint would
    // otherwise send them to a hosted provider, which fails in a way that looks
    // like a broken client rather than a config mix-up.
    if (providerUrl.empty()) providerUrl = settings.valueOr(provider + "-provider-url", "");
    if (providerUrl.empty() && provider == defaultProvider) {
        providerUrl = settings.valueOr("provider-url", "");
    }
    if (providerUrl.empty()) providerUrl = defaults.host;
    const std::string host = providerUrl;

    // What the config actually resolved to, printed before anything can fail.
    // With several named blocks in one store, "which backend am I driving" stops
    // being obvious from the command line — and a block pointing at the wrong
    // dialect is otherwise invisible until the requests come back malformed.
    // The key is deliberately not shown.
    std::cout << "Provider " << provider << " (" << dialect << ") " << model
              << " at " << host << "\n";

    // Naming a VM selects the VMware console; everything needed to reach it can
    // live in the config store, so the usual invocation is just --vm <name>.
    const bool vmwareMode = !vmName.empty();
    if (vmwareMode) {
        if (vcenter.host.empty())     vcenter.host     = settings.valueOr("vcenter-host", "");
        if (vcenter.username.empty()) vcenter.username = fromEnv("TAPTO_VCENTER_USER");
        if (vcenter.username.empty()) vcenter.username = settings.valueOr("vcenter-user", "");
        vcenter.password = fromEnv("TAPTO_VCENTER_PASSWORD");
        if (vcenter.password.empty()) vcenter.password = settings.valueOr("vcenter-password", "");
        if (!vcenter.insecure) {
            vcenter.insecure = settings.valueOr("vcenter-insecure", "false") == "true";
        }

        if (vcenter.host.empty()) {
            std::cerr << "ERROR: no vCenter host. Pass --vcenter or set vcenter-host in the config store.\n";
            return 2;
        }
        if (vcenter.username.empty()) {
            std::cerr << "ERROR: no vCenter user. Pass --user, set TAPTO_VCENTER_USER, or set vcenter-user.\n";
            return 2;
        }
        if (vcenter.password.empty()) {
            std::cerr << "ERROR: no vCenter password. Set TAPTO_VCENTER_PASSWORD or vcenter-password.\n";
            return 2;
        }

        if (resolution.empty()) resolution = settings.valueOr("resolution", "");
        if (!resolution.empty() &&
            !parseResolution(resolution, resolutionWidth, resolutionHeight)) {
            std::cerr << "ERROR: resolution '" << resolution
                      << "' is not <width>x<height> within 640x480..7680x4320,"
                         " e.g. 1280x1024\n";
            return 2;
        }
    } else if (!vcenter.host.empty()) {
        std::cerr << "ERROR: --vcenter also needs --vm to say which VM's console to open.\n";
        return 2;
    } else if (!resolution.empty()) {
        // Reached only when the flag was passed, since the config key is read
        // inside the VMware branch: resizing goes through VMware Tools and has
        // no equivalent on a plain VNC server.
        std::cerr << "ERROR: --resolution only applies to a VMware console; it needs --vm.\n";
        return 2;
    }

    if (!trace.empty()) mclog_set_file(trace);

    try {
        tapto::VncSession session;

        if (vmwareMode) {
            // Before the ticket, not after: the framebuffer is sized when the
            // RFB session opens, so a resize has to have landed by then.
            if (resolutionWidth > 0) {
                std::cout << "Setting guest resolution to " << resolutionWidth << "x"
                          << resolutionHeight << " (up to 30s)... " << std::flush;
                const tapto::ScreenSize actual = tapto::setScreenResolution(
                    vcenter, vmName, resolutionWidth, resolutionHeight);
                if (!actual.valid()) {
                    std::cout << "no answer from the guest; VMware Tools may not be running\n";
                } else if (actual.width != resolutionWidth || actual.height != resolutionHeight) {
                    std::cout << "guest settled on " << actual.width << "x" << actual.height << "\n";
                } else {
                    std::cout << "done\n";
                }
            }

            const tapto::ConsoleTicket ticket = tapto::acquireConsoleTicket(vcenter, vmName);
            session.connectWebSocket(ticket.websocketUrl());
        } else {
            session.connect(vnc);
        }

        std::cout << "Connected to " << session.desktopName() << " ("
                  << session.width() << "x" << session.height() << ")\n"
                  << "Provider: " << provider << "  model: " << model
                  << "  effort: " << effort << "\n\n";

        // Prime the framebuffer so the model's first screenshot is complete
        // rather than the partial first update the server happens to send.
        session.capture();

        // Layout check: type a string into whatever has focus and save the
        // result, so the mapping table can be verified against the real guest
        // instead of trusted. No model involved.
        if (!typeTest.empty()) {
            Context probe;
            probe.set(tapto::keys::kSession, &session);
            for (const ToolSpec& tool : tapto::makeComputerTools()) {
                if (tool.name != "vnc_type") continue;
                const std::string result =
                    tool.executor(probe, nlohmann::json{{"text", typeTest}});
                std::cout << result << "\n";
                break;
            }
            const std::string out = "layout-test.png";
            std::cout << (session.writePng(out) ? "Wrote " + out : "Failed to write " + out)
                      << " — compare it against: " << typeTest << "\n";
            session.disconnect();
            return 0;
        }

        AiConfig config;
        config.setMaxToolIterations(maxSteps);
        config.setMaxOutputTokens(maxOutputTokens);
        config.setPrintCot(!quiet);
        config.setEffort(effort);
        config.setKeepRecentImages(settings.intOr("keep-recent-images", 3));

        Context context;
        context.tools = tapto::makeComputerTools();
        context.set(tapto::keys::kSession, &session);
        if (!screenshotDir.empty()) {
            context.set(tapto::keys::kScreenshotDir, screenshotDir);
            std::cout << "Saving screenshots to " << screenshotDir << "\n";
        }

        // The last argument is the API key itself, despite the parameter being
        // named apiKeyRef — the clients return it verbatim from getApiKey().
        // Chosen by dialect, never by name: 'gemma4' is a name this program has
        // never heard of, and what it means is whatever its -provider-type says.
        std::unique_ptr<AiBackend> client;
        if (dialect == "openai") {
            client.reset(new OpenAIClient(&config, host, model, apiKey));
        } else if (dialect == "gemini") {
            client.reset(new GeminiClient(&config, host, model, apiKey));
        } else {
            client.reset(new ClaudeClient(&config, host, model, apiKey));
        }
        // Left unset by default, which sends no thinking field at all and
        // leaves each server on its own default.
        //
        // On an OpenAI-compatible server this becomes
        // chat_template_kwargs.enable_thinking, which is a *per-request*
        // switch, not a launch flag: a model whose template suppresses the
        // thought channel emits nothing without it, however the server was
        // started. Measured against the local Gemma 4 build — off by default,
        // and thinking coexists with tool calls when enabled.
        if (thinking == "on")       client->setThinkingBudget(1);
        else if (thinking == "off") client->setThinkingBudget(0);

        client->setSystemPrompt(kSystemPrompt);
        client->start();

        auto runTurn = [&](const std::string& message) {
            try {
                const std::string reply = client->chat(context, message);
                tapto::ui::print_reply(reply);
            } catch (const std::exception& e) {
                tapto::ui::end_status();
                tapto::ui::print_error(e.what());
            }
        };

        if (!task.empty()) {
            runTurn(task);
        }

        // A line carrying no actual request. Checked instead of line.empty()
        // because a redirected or piped stdin can deliver a UTF-8 byte-order
        // mark as its first "line": not the empty string, so it was sent as a
        // real turn, which cost an API call and made Gemini reject the request
        // outright. Trailing \r from CRLF input is caught here too.
        auto blank = [](const std::string& text) {
            size_t i = text.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
            for (; i < text.size(); ++i) {
                if (!std::isspace(static_cast<unsigned char>(text[i]))) return false;
            }
            return true;
        };

        // Interactive loop. Kept even after a one-shot task so the user can
        // follow up on what the model just did without reconnecting.
        std::string line;
        while (true) {
            if (!session.isConnected()) {
                tapto::ui::print_error("VNC connection lost");
                return 1;
            }
            std::cout << "\n> " << std::flush;
            if (!std::getline(std::cin, line)) break;   // Ctrl-D
            if (line == "/exit" || line == "/quit") break;
            if (blank(line)) continue;
            runTurn(line);
        }

        session.disconnect();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
