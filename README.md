# tapto-vnc

[![CI](https://github.com/centlakestefan/tapto-vnc/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/centlakestefan/tapto-vnc/actions/workflows/ci.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)

Lets a language model drive a computer through its screen, keyboard and mouse,
over VNC.

It connects to a remote desktop, hands the model screenshots, and gives it
tools to click, type and look closer. Nothing runs inside the guest —
everything travels over the RFB wire protocol, so the target can be any machine
with a VNC server or a VMware console, including one that has just booted or
one you would rather not install anything on.

```sh
tapto-vnc --host 192.0.2.10 "Open the settings app and turn on dark mode"
```

**Highlights**

- **Nothing to install on the target** — plain RFB, so it works on a fresh
  install, a locked-down box, or a machine sitting at its login screen.
- **Multiple providers** — Claude, any OpenAI-compatible endpoint (vLLM,
  llama.cpp, LM Studio, OpenAI itself), or Gemini, sharing one set of tools and
  one system prompt. Name as many as you like in one config, including two
  local servers speaking the same API, and switch with `--provider`.
- **Works with local models** — a 4-bit 27B quant can complete real GUI tasks,
  not just frontier models.
- **VMware consoles** — connects through a vCenter/ESXi WebMKS console, so a VM
  needs no VNC server of its own.
- **Remote keyboard layouts** — text is translated to the guest's layout, since
  RFB carries key positions rather than characters.
- **Self-contained** — one C++17 binary; nlohmann/json and cpp-httplib are
  fetched at build time.

## Build

Requires CMake 3.16+ and a C++17 compiler (MSVC, gcc, or clang).

```sh
cmake -S . -B build
cmake --build build --config Release
```

Binaries land at `build/tapto-vnc` (Linux) or `build/Release/tapto-vnc.exe`
(Windows / MSVC), alongside `vnc-smoketest` — which connects and writes a PNG
with no model involved, and is the first thing to reach for when a capture
looks wrong.

### Dependencies

Fetched automatically at configure time via CMake `FetchContent` (needs git and
network on the first configure):

- [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3`
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) `v0.15.3`

**OpenSSL and zlib are required** — OpenSSL for the HTTPS provider APIs and for
DES in VNC authentication, zlib for the ZRLE encoding. Both must be installed
and findable by CMake. On Linux, install your distro's `libssl-dev` /
`zlib1g-dev`. On Windows, if OpenSSL isn't auto-detected:

```sh
cmake -S . -B build -DOPENSSL_ROOT_DIR=C:/path/to/openssl
```

## First run

Set a provider and a key, then give it a task:

```sh
export ANTHROPIC_API_KEY=sk-ant-...
tapto-vnc --host 192.0.2.10 --password secret "Open Notepad and type hello"
```

With no task on the command line it starts an interactive prompt; `/exit` or
Ctrl-D quits.

Add `--screenshots shots/` to keep every frame it saw, each click marked with a
red dot. That is the only reliable record of what a run actually did — models
report success they did not achieve.

## Connecting

**Plain VNC** — port is `5900 + display`.

```sh
tapto-vnc --host 192.0.2.10 --display 0 --password secret "your task"
```

**VMware console (WebMKS)** — naming a VM selects this mode. The console is RFB
tunnelled over a TLS WebSocket on the ESXi host, gated by a ticket that expires
in minutes, so it is acquired immediately before connecting.

```sh
tapto-vnc --vcenter vcenter.example.com --vm BUILD01 "your task"
```

vCenter credentials come from `$TAPTO_VCENTER_USER` / `$TAPTO_VCENTER_PASSWORD`
or the config store, where the password may be a
[reference](#keeping-secrets-out-of-the-config-file) rather than the password
itself. Add `--insecure` for a self-signed certificate.

## Configuration

Settings resolve as: command-line flag, then environment, then a config store
at `~/.tapto/config`, then a default. The store is shared with `tapto-code`.
Plain `key = value` lines; `#` and `;` begin comments.

```
provider = claude
claude-api-key = sk-ant-...
claude-model = claude-sonnet-5

vcenter-host = vcenter.example.com
vcenter-user = automation@vsphere.local
vcenter-password = ...

keyboard-layout = sv
keep-recent-images = 2
```

| Key | Default | Notes |
| --- | --- | --- |
| `provider` | `claude` | which provider block to use |
| `<name>-provider-type` | — | `claude` / `openai` / `gemini` — the API to speak |
| `<name>-api-key` | — | or the vendor's environment variable; may be an `env:` / `cmd:` / `wincred:` reference |
| `<name>-provider-url` | per dialect | point at a local OpenAI-compatible server |
| `<name>-model` | per dialect | |
| `keyboard-layout` | `us` | layout of the **remote** machine |
| `keep-recent-images` | `3` | screenshots kept in the conversation |
| `max-output-tokens` | `16000` | |
| `max-tool-iterations` | `60` | tool calls per reply |
| `trace-file` | unset | path for request/response diagnostics |

### Naming providers

A provider has a **name** and a **dialect**. The name picks a block of keys and
can be anything; the dialect is one of the three request shapes this program
speaks, named by that block's `-provider-type`. So two local servers that both
expose an OpenAI-compatible API can still be told apart:

```
qwen-provider-type = openai          gemma-provider-type = openai
qwen-provider-url  = http://box:8000 gemma-provider-url  = http://box:8081
qwen-model         = Qwen3-VL-30B    gemma-model         = gemma-3-27b
qwen-api-key       = local           gemma-api-key       = local
```

```sh
tapto-vnc --provider gemma "your task"
```

`claude`, `openai` and `gemini` need no block at all — used as a name, each
means its own dialect with that vendor's defaults. Asking for a name the store
does not define lists the ones it does.

On startup the resolved provider is printed, so a block wired to the wrong
dialect shows up immediately rather than as malformed requests:

```
Provider gemma (openai) gemma-3-27b at http://box:8081
```

The unscoped `model`, `provider-url` and `api-key` apply only to the **default**
provider — the one `provider` names. Otherwise a local endpoint's URL would be
sent to a hosted vendor, or a hosted key to a local server.

API keys resolve as: `<name>-api-key`, then the vendor's environment variable
(`ANTHROPIC_API_KEY` / `OPENAI_API_KEY` / `GEMINI_API_KEY`), then the unscoped
`api-key` for the default provider. The block's own key wins on purpose — an
environment variable taking precedence would send a real vendor key to whatever
`<name>-provider-url` points at, and a local server will log it. A server that
checks no key still needs one; any non-empty value does.

An older store using `provider-type = claude` with `claude-api-key` keeps
working: `provider-type` is read as the default provider's name when `provider`
is absent.

### Keeping secrets out of the config file

A secret may say **where** it lives instead of holding it:

| Value | Reads from |
| --- | --- |
| `sk-ant-...` | the value itself |
| `env:ANTHROPIC_API_KEY` | an environment variable |
| `cmd:pass show anthropic` | the first line of a command's output |
| `wincred:tapto/work-claude` | a Windows Credential Manager generic credential |

This applies to `<name>-api-key`, the unscoped `api-key`, `vcenter-password`,
and the `--password` flag. A value with no scheme is the secret itself, so an
existing store keeps working unchanged. `$TAPTO_VCENTER_PASSWORD` and the vendor
key variables are taken verbatim — an environment variable is a secret in its own
right, not a reference to one.

```
work-api-key     = wincred:tapto/work-claude
vcenter-password = wincred:tapto/vcenter
```

```sh
cmdkey /generic:tapto/vcenter /user:tapto /pass    # prompts for the password
tapto-vnc --host 192.0.2.10 --password wincred:tapto/guest-vnc "your task"
```

The flag is worth pointing somewhere too: a password typed on the command line
is in the shell history and visible in the process list for as long as the run
lasts.

`cmd:` is the general escape hatch — `pass`, `gopass`, `op read op://vault/item`,
`gcloud`, `security find-generic-password`, or a git credential helper all work,
and the store keeps saying which provider uses which secret. The command's stderr
and stdin are left attached to the terminal, so a helper that needs to unlock a
vault can prompt.

`wincred:` reads a **generic** credential, which Windows encrypts under your user
account with DPAPI. That protects the key from a config file that gets backed up,
synced, screen-shared or pasted into an issue — it does *not* protect it from
code running as you, which can read the credential without a prompt. It is the
same trade git's `wincred` credential helper makes. Both blob encodings are
accepted, so credentials written by `cmdkey` or by git's helper are read
correctly.

A reference that fails — variable unset, command non-zero, credential missing —
is a hard error naming the problem. It never falls through to the next source,
because that is how one endpoint ends up being handed another one's key.

The store is shared with `tapto-code`, which can write it for you:
`tapto-code --global config set work-api-key wincred:tapto/work-claude`.

### Useful flags

| Flag | Why |
| --- | --- |
| `--screenshots <dir>` | save every frame — a click writes the screen it aimed at with a red dot, then the screen it produced — plus `frames.jsonl` describing them |
| `--trace <path>` | request/response diagnostics, including cache usage |
| `--require-zoom on` | refuse a click unless a zoom containing it came first — for weaker models |
| `--move-first on` | ask the model to hover a point and see what reacts before clicking it — advice, not a gate |
| `--grid 50` | rule and grid the full screenshots too, every 50 px — an experiment, off by default |
| `--resolution 1280x1024` | set the guest's screen size before connecting |
| `--thinking on\|off` | ask the model to reason before acting |
| `--wake off` | skip the left-Shift nudge sent after connecting to dismiss a blank screen saver (on by default) |
| `--layout <name>` | keyboard layout of the remote machine |
| `--type-test <text>` | type a string and save the result, no model involved |
| `--quiet` | hide the model's intermediate reasoning |
| `--version` | version and the commit it was built from |

### Watching a run back

`--screenshots <dir>` leaves the frames and a `frames.jsonl` describing them.
`tools/make-movie.py` turns that into an mp4:

```sh
pip install -r tools/requirements.txt
python tools/make-movie.py shots
```

A four-hour session came out as 5½ minutes and 15 MB, because each frame is
held for the time until the next one actually arrived — clamped at both ends,
since without a ceiling the movie plays the operator's lunch break in real time
and without a floor the click pairs flash past unseen. `--crf 28 --width 960`
takes the same run to 6 MB when it has to be sent somewhere.

The only dependency is PyAV, which carries FFmpeg inside its wheel, so there is
no system ffmpeg to install. If you have one anyway, `frames.concat` is written
alongside and is the same cut list.

### Versions

[CHANGELOG.md](CHANGELOG.md) records what changed and what counts as a breaking
change — for a program, that is the flags, the config keys, and the tools the
model sees, rather than any C++ API.

Every build stamps its version and originating commit, printed by `--version`
and written as the first line of each trace:

```
tapto-vnc 0.2.0 (v0.2.0-3-g1a2b3c4-dirty)
```

Worth having because the lasting artifact of a run is its trace. Source moves
on, a failed link leaves the previous binary in place, and `-dirty` marks a
build that had uncommitted changes and so cannot be reproduced from the hash.

## Tools the model gets

`vnc_screenshot` · `vnc_zoom` · `vnc_click` · `vnc_move` · `vnc_drag` ·
`vnc_scroll` · `vnc_type` · `vnc_key` · `vnc_wait`

Every action returns a fresh screenshot, because a description of an action is
not evidence that it worked.

`vnc_zoom` is the one worth knowing about: it returns a magnified crop of the
screen carrying a numbered ruler and a grid labelled in screen coordinates, so
the model can read a position off it instead of estimating one. Clicking
accurately turns out to be the hard part of this whole exercise, and
[docs/visual-grounding.md](docs/visual-grounding.md) explains why, with the
measurements. [The Eyes of an Agent](https://taptomatic.com/blog/eyes-of-an-agent)
is the same story told as prose.

`vnc_type` translates text through the remote machine's keyboard layout —
typing `@` on a Swedish guest means pressing a different key than on a US one.
Characters the layout cannot reach are typed via Alt+numpad. See
[docs/keyboard-layouts.md](docs/keyboard-layouts.md).

## Safety

The model is driving a real machine. It is told not to guess credentials, to
confirm before irreversible actions, and to check that a click did what it
intended. None of that substitutes for pointing it at a machine you are willing
to have it break. Screenshots contain whatever is on that screen, and
`--screenshots` writes them to disk unencrypted.

## Status

Working and useful, not finished. Known gaps: the incremental (delta) rendering
path has never had a controlled test, and Proxmox/noVNC support is designed but
unbuilt ([docs/proxmox.md](docs/proxmox.md)).

## Credits

The RFB client is vendored from `cpp-vnc`; the agent loop, tool registry and
provider clients are adapted from
[tapto-code](https://github.com/centlakestefan/tapto-code), a command-line AI
coding assistant built on the same foundations. Local modifications to vendored
code are listed in `third_party/rfb/PATCHES.md`.

## Part of TaptoMatic

tapto-vnc is a standalone spin-off of
[TaptoMatic](https://taptomatic.com) — a larger AI-powered development platform
from Centlake Software AB where teams of AI agents collaborate on software
projects under your direction. Like tapto-vnc, it runs locally and uses your own
provider API keys.

## License

tapto-vnc is licensed under the Apache License, Version 2.0 (SPDX:
`Apache-2.0`). See [LICENSE](LICENSE) and [NOTICE](NOTICE).
Copyright 2026 Centlake Software AB.
