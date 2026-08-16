# Changelog

Notable changes to tapto-vnc. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); each entry is one line
about what changed for you, and the commit it links to carries the reasoning.

## What counts as a breaking change

tapto-vnc is a program, not a library, so the surface that has to stay stable is
not a C++ API. It is:

- **command-line flags** — a flag removed or given a different meaning
- **config keys** — a key renamed, dropped, or resolved in a different order
- **the tools the model sees** — their names, their parameters, and what a call
  does

That last one is easy to underrate. A model's behaviour is a function of the
tool schema it is given, so renaming a tool or changing what its arguments mean
invalidates every prompt, transcript and measurement taken before it, in a way
no compiler will report.

Versions are [Semantic Versioning](https://semver.org/) against that surface.
Before 1.0.0 the minor number carries breaking changes, and the patch number
carries everything else.

## [Unreleased]

### Added

- `--grid <px>` (config key `screenshot-grid`, `0` by default) rules and grids
  the full screenshots in screen coordinates, the way `vnc_zoom` already does.
  An open experiment: at 1:1 the labels are least legible exactly where the
  picture is least detailed, so measure before trusting it. ([38ae1c2])
- `--move-first on` (config key `move-first`, off by default) asks the model to
  `vnc_move` to a point and read what reacts before clicking it. Advice, not a
  gate — nothing refuses a click that skipped it. ([65b225d])
- A dropped console reconnects by itself, once per failed tool call, and the
  action that hit the dead connection is never replayed — a reconnect cannot
  turn one click into two. A WebMKS ticket is single-use and expires in
  minutes, so this is a fresh vCenter login rather than a retry.
  ([92f10b4], [a0711aa])
- The trace says who closed the transport and why: the peer or us, with the
  last error where there was one, and how long the connection had been idle
  when the drop was noticed. A healthy stream closed by the far end is now
  distinguishable from one that failed. ([a3fb534], [2af77c2], [5d7918a])
- `tools/kill-tcp.ps1` aborts a live console connection on demand — the only
  mechanism Windows offers for someone else's socket (`SetTcpEntry` with
  `MIB_TCP_STATE_DELETE_TCB`; needs elevation, IPv4 only). It refuses to act
  when the filter matches more than one connection. ([4bf4274])
- `tools/fake-rfb.ps1`, a stub RFB server that fails on purpose: `reset`, `fin`
  and `hold`, which produce three distinct traces. Recovery is otherwise only
  testable by waiting for a real drop. ([a633cd5])
- `--screenshots <dir>` writes `frames.jsonl` beside the images: one object per
  frame with its sequence, file, `epoch_ms`, kind (`full` or `zoom`), phase,
  label, size and aim point. Assembling frames into a contact sheet or a movie
  needs facts a filename cannot carry — not least that zooms and full screens
  are different sizes. ([ba61887])
- Screenshot numbering continues where a directory left off, so a session
  stopped and restarted against the same directory extends one timeline instead
  of overwriting it from `0001` again. Each process appends a start record
  naming what it continues from and which build it is, and frame times are
  absolute, so the gap at a restart is a real measured gap. ([c8dfd86])

### Changed

- **Breaking:** `vnc_zoom` takes `x` and `y` and nothing else, centring a fixed
  320x100 view at 3x. Every zoom is now the same picture with the same 10 px
  ruler pitch; the previous per-call rectangle produced a spacing that changed
  with every call, which models misread. ([5b02ffd])
- **Breaking:** `vnc_click_zoom` is gone and so is `--coord-span`. There is one
  coordinate system — screen pixels — instead of two live at once. ([fce9dee])
- A click saved with `--screenshots` now writes two frames — the screen it aimed
  at, carrying the red dot, and then the screen it produced. The dot used to go
  on the result, which is the one picture it cannot be measured against: the
  target may not still be on screen. Drags mark the point they grab. Neither
  frame is ever shown to the model. ([ba61887])

Both zoom entries are changes to the tools the model sees, so prompts and
measurements taken against the old schema do not carry over.

### Fixed

- A reset socket is readable, not idle. Both transports polled for `POLLIN`
  alone, and Windows signals a dead connection with `POLLHUP`/`POLLERR` and
  never `POLLIN` — so a dropped console was indistinguishable from a quiet one
  until a later send failed. Detection moved from seconds later, in the write
  path, to immediately, in the read path. ([ccc51fc])
- A completed handshake counts as activity, so the idle time reported with a
  drop is measured from the connection rather than from process start.
  ([3da7f8b])
- The interactive prompt reconnects too. It is where a connection sits idle
  longest, and it was the one path that could not recover. ([98d2850])
- Windows socket errors are reported rather than whatever the thread-local
  buffer happened to hold when `FormatMessage` wrote nothing, and their
  trailing `.\r\n` no longer splits a log line in two. ([ccc51fc])
- `--resolution` no longer ends the run when VMware Tools is unavailable. It
  waits the full timeout instead of nine seconds — a guest that has just booted
  is exactly the case where the size is wrong and worth setting — then reports
  what it found and connects at the guest's current size. The first refusal
  reads `guest.toolsStatus`, so `toolsNotInstalled` stops immediately rather
  than waiting for something that will never arrive. ([75842dd])

## [0.2.0] — 2026-08-15

### Added

- Named provider blocks: a provider has a free-form *name* selecting a block of
  config keys, and a *dialect* named by `<name>-provider-type`. Two local
  servers speaking the same API can now be told apart, where one store
  previously held at most one configuration per vendor. ([a1c3e48])
- Secrets may say where they live instead of holding the value —
  `env:ANTHROPIC_API_KEY`, `cmd:pass show anthropic`,
  `wincred:tapto/work-claude` — for any `api-key`, for `vcenter-password`, and
  for `--password`. A value with no scheme is still the secret itself.
  ([af7a6b4], [7e53812])
- `--version`, and the version and originating commit on the first line of
  every trace, so a trace names the build that wrote it.
- The resolved provider, dialect, model and URL are printed at startup, so a
  block wired to the wrong dialect shows up immediately rather than as
  malformed requests. ([ccd944f])

### Changed

- **Breaking:** an API key configured for the provider block now takes
  precedence over the vendor's environment variable, which previously won. The
  old order sent a real vendor key to whatever `<name>-provider-url` pointed at,
  and a local server logs it. ([a1c3e48])
- **Breaking:** the unscoped `model`, `provider-url` and `api-key` apply only to
  the default provider. They were previously applied to whichever provider was
  selected, which sent a local endpoint's URL to a hosted vendor. ([a1c3e48])

An existing store keeps working: `provider-type = claude` is read as the default
provider's name when `provider` is absent.

### Fixed

- `SetScreenResolution` retries on `ToolsUnavailableFault` instead of ending the
  run. VMware Tools is briefly unavailable after a boot or an update, and the
  condition clears in seconds. ([cef058c])
- The model's closing reply reaches the trace on the OpenAI provider; it was
  recorded for Claude only. ([a9990f1])
- Token usage is logged on the OpenAI provider, which is how the per-model
  vision token budgets in [docs/visual-grounding.md](docs/visual-grounding.md)
  were measured. ([6e1e482])

## [0.1.0] — 2026-08-11

Initial release: a model drives a remote screen over RFB/VNC, either against a
plain VNC server or a VMware WebMKS console, with keyboard, mouse and
screenshots as tools. ([3d11be9])

- CI building on Linux and Windows. ([42a022d])
- `NOMINMAX` for Windows builds, where `<windows.h>` breaks `std::min` and
  `std::max` in the vendored RFB sources under MSVC. ([f069d78])

[Unreleased]: https://github.com/centlakestefan/tapto-vnc/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/centlakestefan/tapto-vnc/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/centlakestefan/tapto-vnc/releases/tag/v0.1.0
[3d11be9]: https://github.com/centlakestefan/tapto-vnc/commit/3d11be9
[42a022d]: https://github.com/centlakestefan/tapto-vnc/commit/42a022d
[f069d78]: https://github.com/centlakestefan/tapto-vnc/commit/f069d78
[a9990f1]: https://github.com/centlakestefan/tapto-vnc/commit/a9990f1
[6e1e482]: https://github.com/centlakestefan/tapto-vnc/commit/6e1e482
[a1c3e48]: https://github.com/centlakestefan/tapto-vnc/commit/a1c3e48
[ccd944f]: https://github.com/centlakestefan/tapto-vnc/commit/ccd944f
[af7a6b4]: https://github.com/centlakestefan/tapto-vnc/commit/af7a6b4
[7e53812]: https://github.com/centlakestefan/tapto-vnc/commit/7e53812
[cef058c]: https://github.com/centlakestefan/tapto-vnc/commit/cef058c
[fce9dee]: https://github.com/centlakestefan/tapto-vnc/commit/fce9dee
[38ae1c2]: https://github.com/centlakestefan/tapto-vnc/commit/38ae1c2
[5b02ffd]: https://github.com/centlakestefan/tapto-vnc/commit/5b02ffd
[65b225d]: https://github.com/centlakestefan/tapto-vnc/commit/65b225d
[92f10b4]: https://github.com/centlakestefan/tapto-vnc/commit/92f10b4
[a0711aa]: https://github.com/centlakestefan/tapto-vnc/commit/a0711aa
[3da7f8b]: https://github.com/centlakestefan/tapto-vnc/commit/3da7f8b
[98d2850]: https://github.com/centlakestefan/tapto-vnc/commit/98d2850
[a3fb534]: https://github.com/centlakestefan/tapto-vnc/commit/a3fb534
[2af77c2]: https://github.com/centlakestefan/tapto-vnc/commit/2af77c2
[ccc51fc]: https://github.com/centlakestefan/tapto-vnc/commit/ccc51fc
[4bf4274]: https://github.com/centlakestefan/tapto-vnc/commit/4bf4274
[a633cd5]: https://github.com/centlakestefan/tapto-vnc/commit/a633cd5
[5d7918a]: https://github.com/centlakestefan/tapto-vnc/commit/5d7918a
[ba61887]: https://github.com/centlakestefan/tapto-vnc/commit/ba61887
[75842dd]: https://github.com/centlakestefan/tapto-vnc/commit/75842dd
[c8dfd86]: https://github.com/centlakestefan/tapto-vnc/commit/c8dfd86
