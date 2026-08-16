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

### Changed

- **Breaking:** `vnc_zoom` takes `x` and `y` and nothing else, centring a fixed
  320x100 view at 3x. Every zoom is now the same picture with the same 10 px
  ruler pitch; the previous per-call rectangle produced a spacing that changed
  with every call, which models misread. ([5b02ffd])
- **Breaking:** `vnc_click_zoom` is gone and so is `--coord-span`. There is one
  coordinate system — screen pixels — instead of two live at once. ([fce9dee])

Both are changes to the tools the model sees, so prompts and measurements taken
against the old schema do not carry over.

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
