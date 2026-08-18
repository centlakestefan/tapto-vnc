// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// tapto-vnc: lets a model drive a remote screen over VNC.

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
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
#include "tapto/frame_index.h"
#include "tapto/gemini.h"
#include "tapto/input_map.h"
#include "tapto/log.h"
#include "tapto/openai.h"
#include "tapto/paths.h"
#include "tapto/secret.h"
#include "tapto/ui.h"
#include "tapto/version.h"
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
)";

// Spliced into the prompt above when --grid is on, which is why that literal
// is in two pieces. It belongs at this point in the list, right after the
// coordinate system it describes and before the advice about zooming, which it
// qualifies but does not replace.
const char* kGridGuidance =
    "- Screenshots are ruled along the top and left edges, with a grid drawn from those "
    "rulers. The numbers on them are screen coordinates, so you can read a position "
    "off the picture instead of estimating it: find the gridlines that bracket what you "
    "want and read their labels.\n"
    "- The grid tells you where something is, not what it is. It adds no detail to the "
    "picture — the whole screen still has to fit into one image — so it does not help "
    "you tell one row of a list from the next, or read a small label, or see whether a "
    "box is ticked. For that you still need vnc_zoom, and the paragraphs below still "
    "apply.\n";

const char* kSystemPromptZoom = R"(- A full screenshot has to show the whole screen at once, so small or tightly packed things — rows in a file list, items in a menu, buttons along the bottom of a dialog — are hard to tell apart and easy to misjudge. Do not estimate their position from the full screenshot. Use vnc_zoom on the area around them instead: it returns that area enlarged, with a numbered ruler along the top and left edge and a grid drawn from it, all labelled in screen coordinates.
- Read the target's position off that grid rather than judging it. Find which gridlines bracket the thing you want, read their numbers from the rulers, and give that position to vnc_click. This is reading, not estimating — if you find yourself guessing a coordinate, zoom on it and read it instead.
- Every zoom shows the same 320x100 area at the same magnification, so there is nothing to adjust: you choose only where to point it, and it is centred there. If what you want is not in the picture, point it somewhere else rather than reasoning about what lies outside.
)";

// Spliced in when --move-first is on, which is the second reason the prompt is
// in pieces. It belongs here, after the model has been told how to arrive at a
// coordinate and before it is told what to do about a click that went wrong —
// this is the step that stops there being one.
//
// The idea is the feedback loop a person uses without thinking: put the pointer
// on the target, see the control react, then commit. A hover reaction is the
// guest asserting what is under the pointer, which is stronger evidence than
// the model's own reading of a picture, because it cannot be a misjudged
// position. The asymmetry is what pays for the extra step: a wrong move costs
// nothing, while a wrong click has opened the wrong installer, dismissed a
// dialog and pushed a wizard backwards in earlier runs, each costing five steps
// or more to undo.
//
// Off by default because it is measured once, not established, and it spends a
// step on every click. Prompt only: nothing refuses a click that skipped the
// move, unlike --require-zoom. Should a model turn up that needs the rule
// enforced rather than asked for, that arrives as a third value on this switch
// — `--move-first force` — and not as a second flag. Off, asked and enforced
// are settings of one idea; two flags could be set to contradict each other.
const char* kMoveGuidance =
    "- Move before you click. Call vnc_move to the position you are about to click, look at "
    "the screenshot it returns, and only then call vnc_click at that same position. This "
    "costs one extra step and it is worth it: a wrong move costs nothing, a wrong click can "
    "open the wrong file, dismiss a dialog or undo a step.\n"
    "- What to look for in that screenshot: most controls react to the pointer resting on "
    "them. A button lightens or draws a border, a list row highlights, a close button turns "
    "red, a link underlines. That reaction is the machine itself telling you what is under "
    "the pointer — better evidence than your own reading of the picture, because it cannot "
    "be a misjudgement of position. If the thing you are aiming at reacts, click. If nothing "
    "reacts, or something next to your target reacts instead, you are not where you think: "
    "zoom, read the position again, and move again before clicking.\n"
    "- Some things do not react at all — a desktop icon, a plain text field, empty space. "
    "Then use the pointer itself: it is drawn in the screenshot, so check it is sitting on "
    "the target before you click.\n"
    "- Moving can also change the screen on its own: a menu can open on hover, a tooltip can "
    "appear. That is information, not a mistake — read it and carry on.\n";

const char* kSystemPromptRest = R"(- If a click lands in the wrong place, do not adjust it by guesswork and do not repeat it. Zoom on the target again and read its coordinates.
- Zooming is not only about aiming, it is also how you make sure you have the right thing. Neighbouring entries in a list often have names that differ only slightly — the same word with a version number, a suffix or a different extension — and adjacent rows sit about twenty pixels apart, so opening the wrong one is easy and looks like success afterwards. Read the whole row rather than a name alone: the other columns, such as type and size, are usually what tells two similar entries apart, and one zoom takes in about five rows. Confirm you have the right one before you open it.
- Aim at the largest part of whatever you are clicking, not its smallest visible detail. A big target absorbs a small error; a small one does not.
- This matters most for checkboxes and radio buttons. The little square or circle is only a few pixels across, but the text label beside it belongs to the same control and clicking the text toggles it just the same — and the label is usually many times wider. So to tick "I accept the terms in the License Agreement", click the middle of that sentence rather than trying to hit the box. The same goes for a row in a list: click its name, which is wide, rather than its icon, which is not.
- Afterwards, look at the screenshot and confirm the box actually changed state. A label is not always wired to its control, so if the click did nothing, zoom in and aim at the box itself.
- Click a text field before typing into it. Use vnc_type for text, vnc_key for named keys and shortcuts.
- Take particular care with the buttons along the bottom of a wizard. Back, Next, Install and Cancel sit side by side, are the same size and shape, and differ only by their label — and clicking Back undoes a step you have already completed. Zoom on the button you are about to hit, read its label, confirm it is the one you want, and afterwards check that the page moved forward rather than back.
- After an action that starts something slow — launching an application, loading a page, an installer step — use vnc_wait and look again rather than assuming it finished.
- If an action did not do what you expected, say so and look at the screen again. Do not repeat the same click hoping for a different result.
- Small text is drawn with subpixel antialiasing, so individual letters often contain strongly coloured pixels — orange, blue or purple fringes — even where the text is plain black. That is how the remote machine renders fonts, not a property of the document and not a fault. Read the words; do not report per-word colour differences in body text, and do not investigate them further — zoom is for working out where something is, not for studying how letters are drawn.

Be careful with actions that are hard to undo: deleting files, overwriting data, confirming destructive dialogs, changing system settings. If you are about to do something irreversible that the user did not clearly ask for, stop and ask them first.

Never guess a password, PIN or other credential. If the machine asks you to sign in and you have not been told what to enter, say so and stop rather than trying something plausible. Repeated wrong attempts can lock an account, and a guess that happens to work is not something the user authorised. The same goes for any other secret you were not given.

Your private reasoning is not kept between steps — only your replies, the tool results and the most recent screenshots are. So when you finish something that matters, say so in your reply rather than only thinking it: "the installer has finished and I clicked Finish" is a note to yourself as much as to the user, and it is the only part of that thought you will still have later.

When the task you were given is complete, say so and stop. Do not start it over. Before repeating any substantial action — running an installer, creating an account, sending something — check whether you have already done it: look at your earlier replies and at what is on screen. Doing an installation twice is not harmless, and a screen that looks like the starting point is not evidence that nothing has happened.

Tell the user what you observe and what you are doing, in plain language. When a task is done, say what the final state of the screen is.)";

// Spliced in only when the steps come from a prompt file, which is the third
// reason this prompt is in pieces.
//
// It is the one setup where nobody reads the reply before the next instruction
// is sent: the model finishes a step, the program sends the next one, and a
// model that has quietly given up carries on being asked for the steps that
// depended on the one it did not do. A step that cannot be done needs a way to
// say so that the program can act on, and the reply is the only channel every
// provider has.
//
// A word at the start of the reply rather than a tool, deliberately. A tool
// would have to end the turn in three separate tool loops, and adding one
// changes the schema every earlier run was measured against — for an escape
// hatch that is used when everything else has already gone wrong. This costs
// nothing when it is not used, and it is not offered at all to an interactive
// session, where the person reading the reply is the escape hatch.
//
// The instruction is worded against the two ways it fails in practice: a model
// that reports being stuck in prose and expects someone to notice, and a model
// that says the word because a step went differently, not because it is stuck.
const char* kBlockedGuidance = R"(
The steps of this task were written in advance and are sent to you one at a time: as soon as you finish this one, the next is sent automatically. Nobody is reading your replies while that happens, and each step assumes the screen the step before it was supposed to produce.

So if you cannot do what this step asks — you cannot find what it names, it needs a credential you were not given, the screen is not what it assumes, or doing it would be irreversible and was not clearly asked for — do not improvise something close to it and do not carry on. Carrying on runs the remaining steps against a screen nobody expected, on a machine nobody is watching.

Say so in a form the program can act on: make BLOCKED: the first thing in your reply, followed on that same line by what stopped you. For example:

BLOCKED: there is no Downloads folder in this file manager and no setup.exe on the desktop

That stops the remaining steps and hands the session back to a person, with the screen exactly as you left it, so write the line for whoever picks it up: what you were trying to do, and what you found instead.

Use it only when you are actually stuck. Finishing the step is what is wanted, and a step that went differently from how it was described but reached what it asked for is finished, not blocked — report that in the ordinary way.)";

// "tapto-vnc 0.2.0 (v0.1.0-9-gcef058c)" — the release, then the exact commit.
//
// The commit is the part that earns its place. A run's lasting artifact is its
// trace, and a trace whose build is only implied is a trap: source moves on, a
// link that failed leaves yesterday's binary on disk, and the behaviour being
// puzzled over may not be in the code being read. "-dirty" says the build had
// uncommitted changes, so it cannot be reproduced from the hash alone.
std::string versionLine() {
    return std::string("tapto-vnc ") + TAPTO_VNC_VERSION + " (" + TAPTO_VNC_COMMIT + ")";
}

void usage(const char* argv0) {
    std::cout
        << versionLine() << "\n\n"
        << "Usage: " << argv0 << " [connection options] [agent options] [task]\n\n"
        << "Direct VNC:\n"
        << "  --host <host>       VNC server host (default: localhost)\n"
        << "  --display <n>       Display number; port is 5900+n\n"
        << "  --password <pw>     VNC password, or where it lives (see below)\n\n"
        << "VMware console (WebMKS) — naming a VM selects this mode:\n"
        << "  --vm <name>         VM name (exact, case-sensitive; overrides\n"
        << "                      config key default-vm)\n"
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
        << "  -f, --file <path>   Send the prompts in <path> as consecutive turns,\n"
        << "                      separated by a line of three or more '='. For a\n"
        << "                      task that takes several steps: each block is sent\n"
        << "                      once the one before it has finished, so the model\n"
        << "                      works from the screen that step produced instead\n"
        << "                      of planning the lot up front. A failed turn stops\n"
        << "                      the rest, and exits 1 if the file ends in /exit.\n"
        << "                      A block of just /exit ends the run there instead\n"
        << "                      of returning to the interactive prompt. The model\n"
        << "                      is told to open its reply with BLOCKED: and a\n"
        << "                      reason when a step cannot be done, which stops the\n"
        << "                      file the same way — nobody is reading the replies\n"
        << "                      while it runs.\n"
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
        << "  --move-first <m>    on|off (config key move-first, default off).\n"
        << "                      Asks the model to vnc_move to a point and look\n"
        << "                      at what reacts before clicking it. Advice, not a\n"
        << "                      gate: nothing refuses a click that skipped it.\n"
        << "                      Costs a step per click and is measured once, not\n"
        << "                      established — try it where wrong clicks are\n"
        << "                      expensive.\n"
        << "  --grid <px>         Rule and grid the full screenshots every <px>\n"
        << "                      screen pixels, labelled in screen coordinates\n"
        << "                      (config key screenshot-grid; 0/off by default,\n"
        << "                      10..500 otherwise). vnc_zoom is always ruled;\n"
        << "                      this is the same idea applied at 1:1, where it\n"
        << "                      is not yet known to help — measure before\n"
        << "                      trusting it.\n"
        << "  --wake <mode>       on|off (config key wake, default on). Sends\n"
        << "                      left Shift after connecting, to dismiss a\n"
        << "                      blank screen saver before the first\n"
        << "                      screenshot. Shift types nothing and activates\n"
        << "                      nothing; --wake off skips it, at the cost of a\n"
        << "                      black first frame on a guest left alone.\n"
        << "  --layout <name>     Keyboard layout of the REMOTE machine\n"
        << "                      (config key keyboard-layout; default us)\n"
        << "  --altcode <mode>    on|off|auto — Alt+numpad fallback for characters\n"
        << "                      the layout cannot reach. Windows guests only;\n"
        << "                      needs NumLock on. (config key altcode-fallback)\n"
        << "  --type-test <text>  Type <text> into whatever has focus, save\n"
        << "                      layout-test.png, and exit. No model involved —\n"
        << "                      use it to check a layout against a real guest.\n"
        << "  --version           Version and the commit it was built from\n\n"
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
        << "An api-key, a vcenter-password or --password may say where the secret\n"
        << "lives instead of holding it:\n"
        << "  env:ANTHROPIC_API_KEY        an environment variable\n"
        << "  cmd:pass show anthropic      first line of a command's output\n"
        << "  wincred:tapto/work-claude    Windows Credential Manager (cmdkey /generic:)\n"
        << "A value with no scheme is the secret itself. $TAPTO_VCENTER_PASSWORD and\n"
        << "the vendor key variables are taken verbatim; they are secrets, not\n"
        << "references.\n\n"
        << "Settings resolve in this order: CLI flag, environment, then the shared\n"
        << "tapto config store (~/.tapto/config, same one tapto-code uses), then a\n"
        << "default. The API key comes from $ANTHROPIC_API_KEY or the store's\n"
        << "api-key; model, effort, provider-url, max-output-tokens,\n"
        << "max-tool-iterations, print-cot and trace-file are read from it too.\n\n"
        << "Any remaining arguments are sent as the first task, ahead of any -f\n"
        << "file. With neither, tapto-vnc starts at an interactive prompt; it returns\n"
        << "to one after a task or a file, so a run can be followed up by hand.\n"
        << "Type /exit to quit.\n";
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

// Whether a reply is the model reporting itself stuck rather than done, and
// what it said stopped it.
//
// Only the first line is examined and the word has to open it. A reply that
// merely contains the token is not a surrender: models quote the instruction
// that told them about it, describe a dialog that says "blocked", and explain
// what they would have said had they been stuck. Abandoning the rest of a file
// on any of those is worse than not having the escape hatch at all.
//
// Leading markdown comes off first, because a model emphasises the opening
// word of a reply without being asked to and "**BLOCKED:**" is the same
// statement. What follows the word must be a separator or nothing, so
// "blocked the print queue and carried on" is prose, not a verdict.
bool blockedReply(const std::string& reply, std::string& reason) {
    static const std::string kToken = "blocked";
    static const char* kNoise = " \t\r*_#>`\"'";
    reason.clear();

    const size_t start = reply.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return false;
    const size_t newline = reply.find('\n', start);
    std::string line = reply.substr(
        start, newline == std::string::npos ? std::string::npos : newline - start);

    const size_t opening = line.find_first_not_of(kNoise);
    if (opening == std::string::npos) return false;
    line.erase(0, opening);
    if (line.size() < kToken.size()) return false;
    for (size_t i = 0; i < kToken.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != kToken[i]) return false;
    }

    std::string rest = line.substr(kToken.size());
    const size_t after = rest.find_first_not_of(kNoise);
    if (after == std::string::npos) return true;   // The word on its own line.
    // ":" is what the prompt asks for; a dash, including the em dash a model
    // reaches for unprompted, says the same thing.
    static const std::string kEmDash = "\xE2\x80\x94";
    size_t skip = 0;
    if (rest[after] == ':' || rest[after] == '-') skip = 1;
    else if (rest.compare(after, kEmDash.size(), kEmDash) == 0) skip = kEmDash.size();
    else return false;

    rest.erase(0, after + skip);
    const size_t begin = rest.find_first_not_of(kNoise);
    if (begin == std::string::npos) return true;   // Blocked, but it did not say why.
    reason = rest.substr(begin, rest.find_last_not_of(kNoise) - begin + 1);
    return true;
}

// The two words that end a session. Recognised in a prompt file as well as at
// the prompt, so a file can say "and then stop" instead of leaving a run that
// nobody is watching parked at a prompt.
bool isExitCommand(const std::string& text) {
    return text == "/exit" || text == "/quit";
}

// The turns held in a prompt file, in the order they will be sent.
//
// A task on a desktop is rarely one instruction: install this, then configure
// it, then check what it did. Handing the whole plan over in one message makes
// the model commit to all of it from the first screenshot, so the steps are
// better typed one at a time — which is what this reads from a file instead,
// so a sequence that worked can be repeated exactly rather than from memory.
//
// The separator is a line of three or more '=' and nothing else: visible in
// any editor, and not something prose walks into by accident. Everything
// between two separators is one turn, trimmed of surrounding whitespace, and
// empty blocks are dropped so a file that opens or closes with a separator
// still says what it looks like it says.
std::vector<std::string> readPromptFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "ERROR: cannot read prompt file '" << path << "'\n";
        std::exit(2);
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Notepad and PowerShell's Set-Content both write a UTF-8 byte-order mark.
    // It is invisible here and a stray character in the first turn the model
    // is given, so it goes before anything is split.
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);

    auto trim = [](const std::string& text) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    };
    auto isSeparator = [&trim](const std::string& line) {
        const std::string bare = trim(line);
        if (bare.size() < 3) return false;
        return bare.find_first_not_of('=') == std::string::npos;
    };

    std::vector<std::string> prompts;
    std::string block;
    auto endBlock = [&]() {
        const std::string prompt = trim(block);
        if (!prompt.empty()) prompts.push_back(prompt);
        block.clear();
    };

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (isSeparator(line)) { endBlock(); continue; }
        block += line;   // A trailing \r from CRLF input is trimmed with the rest.
        block += '\n';
    }
    endBlock();

    if (prompts.empty()) {
        std::cerr << "ERROR: prompt file '" << path << "' contains no prompts\n";
        std::exit(2);
    }
    return prompts;
}

std::string fromEnv(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// Replace a value that names where a secret lives with the secret itself; see
// tapto/secret.h. A value with no scheme is already the secret and is left
// alone, so this is safe to apply to anything the user can write a password
// into.
//
// A failed reference is reported against the name the user wrote — `--password`
// or `vcenter-password` — and answered with false rather than an empty string,
// so the caller stops instead of carrying on to a login attempt that would look
// like a wrong password.
bool resolveSecretInPlace(const char* source, std::string& value) {
    if (value.empty()) return true;
    const tapto::Secret resolved = tapto::resolve_secret(value);
    if (!resolved.error.empty()) {
        std::cerr << "ERROR: " << source << ": " << resolved.error << "\n";
        return false;
    }
    value = resolved.value;
    return true;
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
                screenshotDir, providerUrl, resolution, thinking, requireZoom, moveFirst, grid,
                wake, promptFile;
    int maxSteps = 0;
    int resolutionWidth = 0, resolutionHeight = 0;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { usage(argv[0]); return 0; }
        if (arg == "--version") { std::cout << versionLine() << "\n"; return 0; }
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
        if (const char* v = option(argc, argv, i, "--layout"))    { layout = v; continue; }
        if (const char* v = option(argc, argv, i, "--type-test")) { typeTest = v; continue; }
        if (const char* v = option(argc, argv, i, "--provider-url")) { providerUrl = v; continue; }
        if (const char* v = option(argc, argv, i, "--provider"))  { provider = v; continue; }
        if (const char* v = option(argc, argv, i, "--model"))     { model = v; continue; }
        if (const char* v = option(argc, argv, i, "--effort"))    { effort = v; continue; }
        if (const char* v = option(argc, argv, i, "--thinking"))  { thinking = v; continue; }
        if (const char* v = option(argc, argv, i, "--require-zoom")) { requireZoom = v; continue; }
        if (const char* v = option(argc, argv, i, "--move-first")) { moveFirst = v; continue; }
        if (const char* v = option(argc, argv, i, "--wake"))      { wake = v; continue; }
        if (const char* v = option(argc, argv, i, "--grid"))      { grid = v; continue; }
        if (const char* v = option(argc, argv, i, "--max-steps")) { maxSteps = std::atoi(v); continue; }
        if (const char* v = option(argc, argv, i, "--trace"))     { trace = v; continue; }
        if (const char* v = option(argc, argv, i, "-f"))          { promptFile = v; continue; }
        if (const char* v = option(argc, argv, i, "--file"))      { promptFile = v; continue; }
        if (!arg.empty() && arg[0] == '-') {
            std::cerr << "ERROR: unknown argument '" << arg << "'\n\n";
            usage(argv[0]);
            return 2;
        }
        // Everything else is the task text.
        if (!task.empty()) task += " ";
        task += arg;
    }

    // Read before a single connection is made: a mistyped path should cost
    // nothing, and a VMware console ticket taken here would already be
    // expiring by the time the mistake surfaced.
    const std::vector<std::string> filePrompts =
        promptFile.empty() ? std::vector<std::string>() : readPromptFile(promptFile);

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
    //
    // A configured value may name where the key lives — `env:`, `cmd:`,
    // `wincred:` — instead of being the key; see tapto/secret.h. The vendor
    // environment variable is a secret in its own right, never a reference, so
    // it is taken verbatim.
    tapto::Secret key;
    if (const std::string configured = settings.valueOr(provider + "-api-key", "");
        !configured.empty()) {
        key = tapto::resolve_secret(configured);
    } else if (std::string vendorEnv = fromEnv(defaults.apiKeyEnv); !vendorEnv.empty()) {
        key.value = std::move(vendorEnv);
    } else if (provider == defaultProvider) {
        // The unscoped api-key belongs to the default provider only. Otherwise
        // one vendor's key would be handed to another.
        if (const std::string unscoped = settings.valueOr("api-key", ""); !unscoped.empty()) {
            key = tapto::resolve_secret(unscoped);
        }
    }

    // A key that is configured but could not be read is reported as itself,
    // never as a missing key: sending the user off to set something they have
    // already set hides the reference that is actually broken. Falling through
    // to the next source would be worse still — that is how one endpoint ends
    // up being handed another one's key.
    if (!key.error.empty()) {
        std::cerr << "ERROR: provider '" << provider << "': " << key.error << "\n";
        return 2;
    }
    const std::string apiKey = key.value;
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

    if (moveFirst.empty()) moveFirst = settings.valueOr("move-first", "off");
    if (moveFirst != "on" && moveFirst != "off") {
        std::cerr << "ERROR: --move-first expects on or off\n";
        return 2;
    }

    // On by default, unlike the other switches here. Those add a behaviour
    // worth measuring before trusting; this removes a failure — a blank screen
    // the model cannot interpret — and its cost when unnecessary is one second
    // and a keystroke that does nothing.
    if (wake.empty()) wake = settings.valueOr("wake", "on");
    if (wake != "on" && wake != "off") {
        std::cerr << "ERROR: --wake expects on or off\n";
        return 2;
    }

    // Must land before makeComputerTools(): vnc_screenshot's description
    // explains the grid, so it has to know whether there is one.
    if (grid.empty()) grid = settings.valueOr("screenshot-grid", "0");
    if (grid == "off") grid = "0";
    int gridStep = 0;
    try { gridStep = std::stoi(grid); }
    catch (const std::exception&) {
        std::cerr << "ERROR: --grid expects a number of pixels, or off\n";
        return 2;
    }
    // A grid finer than this is unreadable once the screenshot is downscaled
    // for the model, and one coarser than the screen is not a grid.
    if (gridStep != 0 && (gridStep < 10 || gridStep > 500)) {
        std::cerr << "ERROR: --grid expects 0 (off) or 10..500 pixels\n";
        return 2;
    }
    tapto::setScreenshotGrid(gridStep);

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

    // Naming a VM selects the VMware console; everything else in that branch
    // can live in the config store, and with default-vm so can the name itself.
    if (vmName.empty()) vmName = settings.valueOr("default-vm", "");
    const bool vmwareMode = !vmName.empty();
    if (vmwareMode) {
        if (vcenter.host.empty())     vcenter.host     = settings.valueOr("vcenter-host", "");
        if (vcenter.username.empty()) vcenter.username = fromEnv("TAPTO_VCENTER_USER");
        if (vcenter.username.empty()) vcenter.username = settings.valueOr("vcenter-user", "");
        vcenter.password = fromEnv("TAPTO_VCENTER_PASSWORD");
        if (vcenter.password.empty()) {
            // The store may say where the password lives rather than holding
            // it. The environment variable above is taken verbatim, like the
            // vendor API-key variables: it is a secret in its own right.
            vcenter.password = settings.valueOr("vcenter-password", "");
            if (!resolveSecretInPlace("vcenter-password", vcenter.password)) return 2;
        }
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

    // A VNC password only ever arrives on the command line, where it is visible
    // in the shell history and the process list, so being able to name where it
    // lives instead — --password wincred:tapto/guest-vnc — is worth more here
    // than in a file only the user can read.
    if (!resolveSecretInPlace("--password", vnc.password)) return 2;

    if (!trace.empty()) {
        mclog_set_file(trace);
        // First line of every trace, before anything can go wrong: which build
        // wrote what follows. The file is appended to across runs, so this also
        // separates one run from the next.
        mclog(versionLine() + "\n");
    }

    try {
        tapto::VncSession session;

        // How to open the connection, kept rather than run once. A console can
        // be replaced after it drops, but a VMware ticket is single-use and
        // expires in minutes, so restoring one is a fresh vCenter login and a
        // new ticket rather than redialling the same URL. Only this scope knows
        // which of the two modes is in play, so the knowledge is packaged here
        // and handed to the tools.
        auto establish = [&](tapto::VncSession& target) {
            if (vmwareMode) {
                const tapto::ConsoleTicket ticket = tapto::acquireConsoleTicket(vcenter, vmName);
                target.connectWebSocket(ticket.websocketUrl());
            } else {
                target.connect(vnc);
            }

            // A guest left alone blanks its screen, and a black first frame is
            // the worst thing to hand a model that can only see through
            // screenshots: it shows nothing, and working out that a keystroke
            // might reveal something is a lot to ask of a picture with nothing
            // in it. Left Shift is the conventional nudge — it dismisses a
            // blank screen saver, types nothing and activates nothing, so
            // sending it to a guest that was already awake costs a second and
            // changes nothing.
            //
            // Here rather than after the first connect, because this is also
            // the reconnect path, and a reconnect follows the longest idle in
            // the run — exactly when the screen has had time to blank.
            if (wake == "on") {
                constexpr uint32_t kShiftLeft = 0xFFE1;  // X11 keysym
                target.sendKey(kShiftLeft, true);
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                target.sendKey(kShiftLeft, false);
                // The guest redraws in its own time; asking for the screen
                // before it has would capture the blank one we just dismissed.
                std::this_thread::sleep_for(std::chrono::milliseconds(1200));
                target.pump(std::chrono::milliseconds(200));
                // It is input the model did not ask for, so the trace says it
                // happened. A keystroke nobody can account for later is worse
                // than the blank screen it was sent to fix.
                mclog("Sent a wake keystroke (left Shift) after connecting\n");
            }
        };

        if (vmwareMode) {
            // Before the ticket, not after: the framebuffer is sized when the
            // RFB session opens, so a resize has to have landed by then.
            if (resolutionWidth > 0) {
                // Two waits, not one: up to 30s for VMware Tools to be there to
                // take the request, then up to 30s for the guest to apply it.
                // The second cannot be folded into the first — the console
                // framebuffer is sized when the RFB session opens, so a resize
                // still in flight would land mid-stream.
                std::cout << "Setting guest resolution to " << resolutionWidth << "x"
                          << resolutionHeight << " (up to 60s)... " << std::flush;
                const tapto::ScreenSize actual = tapto::setScreenResolution(
                    vcenter, vmName, resolutionWidth, resolutionHeight);
                if (!actual.valid()) {
                    // Not fatal. The console works at whatever size the guest is
                    // already using, and ending the run here would trade a
                    // slightly wrong screen for no screen at all.
                    std::cout << (actual.note.empty()
                                      ? "no answer from the guest; VMware Tools may not be running"
                                      : actual.note)
                              << "; continuing at the guest's current size\n";
                } else if (actual.width != resolutionWidth || actual.height != resolutionHeight) {
                    std::cout << "guest settled on " << actual.width << "x" << actual.height << "\n";
                } else {
                    std::cout << "done\n";
                }
            }

        }
        establish(session);

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

        // One attempt to put the console back, so a dropped socket costs a
        // pause rather than the run. Deliberately does not loop: if the first
        // try fails the cause is not transient, and a run that keeps redialling
        // a machine nobody is watching is worse than one that stops.
        context.set<std::function<bool()>>(tapto::keys::kReconnect, [&]() -> bool {
            try { session.disconnect(); } catch (const std::exception&) {}
            try {
                establish(session);
                // Prime it, exactly as after the first connect: the composite
                // is empty again and the next screenshot must not be a partial
                // first update.
                session.capture();
                return session.isConnected();
            } catch (const std::exception& e) {
                mclog(std::string("VNC reconnect failed: ") + e.what() + "\n");
                return false;
            }
        });

        if (!screenshotDir.empty()) {
            tapto::frames::set_directory(screenshotDir);
            std::cout << "Saving screenshots to " << screenshotDir << "\n";

            // An establishing shot: the desktop as it was found, before the
            // model has been asked for anything. Every other frame in the
            // directory is the consequence of an action, so without this one
            // there is no record of the state they all started from — and a
            // movie of the run opens on the result of its first click.
            tapto::VncSession::Rect whole;
            tapto::frames::Meta meta;
            meta.width  = (tapto::screenshotGrid() > 0
                               ? tapto::VncSession::rulerMarginLeft(1) : 0) + session.width();
            meta.height = (tapto::screenshotGrid() > 0
                               ? tapto::VncSession::rulerMarginTop(1) : 0) + session.height();
            tapto::frames::save(
                session.screenshotRegionPng(whole, 1, nullptr, tapto::screenshotGrid() > 0,
                                            tapto::screenshotGrid()),
                "Connected, before the first prompt", meta);
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

        // Spliced rather than always present: telling a model to read rulers
        // that are not in the picture is worse than saying nothing at all, and
        // the move-first paragraphs are a change of method rather than an extra
        // hint — a run that is not using them should not be reading them.
        std::string systemPrompt = kSystemPrompt;
        if (tapto::screenshotGrid() > 0) systemPrompt += kGridGuidance;
        systemPrompt += kSystemPromptZoom;
        if (moveFirst == "on") systemPrompt += kMoveGuidance;
        systemPrompt += kSystemPromptRest;
        // Only for a prompt file. In an interactive session the person reading
        // the reply is the escape hatch, and a rule about a magic word nothing
        // acts on would be noise in the prompt.
        if (!filePrompts.empty()) systemPrompt += kBlockedGuidance;
        client->setSystemPrompt(systemPrompt);
        client->start();

        // Returns false when the turn failed, which only a scripted sequence
        // acts on: at the interactive prompt the person can see what happened
        // and decide, but the prompts in a file are steps of one task and
        // step three is meaningless if step two never ran.
        struct Turn { bool ok; std::string reply; };
        auto runTurn = [&](const std::string& message) -> Turn {
            try {
                // Both ends of a turn go into the frame index: what it was
                // asked, and what it said when it finished. The frames in
                // between show a machine being operated by nobody until these
                // two lines say who asked and why.
                tapto::frames::record("prompt", message);
                const std::string reply = client->chat(context, message);
                tapto::frames::record("reply", reply);
                tapto::ui::print_reply(reply);
                return {true, reply};
            } catch (const std::exception& e) {
                tapto::ui::end_status();
                tapto::ui::print_error(e.what());
                return {false, {}};
            }
        };

        if (!task.empty()) {
            runTurn(task);
        }

        // The prompts from -f, one turn each. Echoed in the shape the
        // interactive prompt uses, so the transcript of a scripted run reads
        // like a typed one and shows which step a reply belongs to.
        //
        // `quit` is what a closing /exit block sets: the file was a whole run
        // rather than an opening, so there is nobody to hand the prompt back
        // to. `failed` is remembered because such a run is usually started by
        // something that reads an exit status.
        bool quit = false, failed = false;
        for (size_t i = 0; i < filePrompts.size(); ++i) {
            if (isExitCommand(filePrompts[i])) {
                quit = true;
                const size_t skipped = filePrompts.size() - i - 1;
                if (skipped > 0) {
                    tapto::ui::print_warning(std::to_string(skipped) + " prompt(s) after " +
                                             filePrompts[i] + " in " + promptFile +
                                             " were not sent");
                }
                break;
            }
            std::cout << "\n> " << filePrompts[i] << "\n";
            const Turn turn = runTurn(filePrompts[i]);
            // A step the model could not do ends the file for the same reason
            // a failed one does: what follows was written for the screen this
            // step was supposed to leave behind.
            std::string reason;
            const bool blocked = turn.ok && blockedReply(turn.reply, reason);
            if (turn.ok && !blocked) continue;
            failed = true;
            if (blocked) {
                tapto::ui::print_error(reason.empty()
                                           ? "Stopped: the model reported itself blocked"
                                           : "Stopped: the model reported itself blocked — " + reason);
            }
            const size_t unsent = filePrompts.size() - i - 1;
            if (unsent > 0) {
                tapto::ui::print_error((blocked ? std::string() : std::string("Stopped after a failed turn; ")) +
                                       std::to_string(unsent) + " prompt(s) from " +
                                       promptFile + " were not sent");
            }
            // The /exit that is now never reached still says what the file
            // wanted: a failure must not leave an unattended run sitting at a
            // prompt until someone notices.
            for (size_t j = i + 1; j < filePrompts.size(); ++j) {
                if (isExitCommand(filePrompts[j])) quit = true;
            }
            break;
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
        while (!quit) {
            if (!session.isConnected()) {
                // Sitting at this prompt is the longest silence in a run —
                // nothing pumps the socket while getline blocks — so it is the
                // most likely place to discover a console that timed out. Give
                // it the same single attempt the tools get, rather than ending
                // the session next to the machinery that could have saved it.
                const int idle = session.idleSeconds();
                mclog("VNC connection lost while idle at the prompt after " +
                      std::to_string(idle) + "s; reconnecting\n");
                const bool ok =
                    context.get<std::function<bool()>>(tapto::keys::kReconnect)();
                mclog(ok ? "VNC reconnected\n" : "VNC reconnect failed\n");
                if (!ok) {
                    tapto::ui::print_error("VNC connection lost");
                    return 1;
                }
                std::cout << "Reconnected after " << idle << "s without traffic\n";
            }
            std::cout << "\n> " << std::flush;
            if (!std::getline(std::cin, line)) break;   // Ctrl-D
            if (isExitCommand(line)) break;
            if (blank(line)) continue;
            runTurn(line);
        }

        session.disconnect();
        // Non-zero only for a prompt file that ran to a stop of its own: after
        // an interactive session the last thing that happened was the person's
        // doing, and a failed turn earlier says nothing about it.
        return (quit && failed) ? 1 : 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
