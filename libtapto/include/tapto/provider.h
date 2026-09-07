// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "tapto/paths.h"
#include "tapto/secret.h"

// ---------------------------------------------------------------------------
// Provider resolution against the shared tapto config store.
//
// Lifted verbatim from tapto-code's main.cpp (the anonymous namespace around
// lines 410-618), because tapto-word needs the same resolution and a second
// copy would drift. This header is the intended home for it in both programs:
// when tapto-code is next touched, delete its private copy and include this.
//
// A provider has a *name* and a *dialect*, and they are not the same thing.
// The name selects a block of config keys and is free-form — qwen36, gemma4,
// work-claude. The dialect is one of the three request shapes we can speak,
// named by that block's `<name>-provider-type` key:
//
//   qwen36-provider-type = openai         gemma4-provider-type = openai
//   qwen36-provider-url  = http://a:8000  gemma4-provider-url  = http://b:8081
//   qwen36-model         = Qwen3-VL-30B   gemma4-model         = gemma-3-27b
//   qwen36-api-key       = local          gemma4-api-key       = local
//
// The store is shared with tapto-code and tapto-vnc, which read the same
// blocks. tapto-word adds no keys of its own beyond the `word-` block below.
// ---------------------------------------------------------------------------

namespace tapto {

struct EffectiveEntry {
    std::string key;
    std::string value;
    Level origin;
};

// Merge all scopes lowest-to-highest so later scopes override earlier ones,
// while preserving first-seen ordering of keys.
std::vector<EffectiveEntry> effective_config();

// Look up a config key's effective value across all scopes. A key present but
// empty counts as unset, so `model =` falls back to the default instead of
// asking the provider for a model with no name.
std::optional<std::string> get_effective(const std::string& key);

// The request shapes this program can speak. Used as a provider name, each one
// means its own dialect with that vendor's defaults, so `claude`, `openai` and
// `gemini` need no block at all.
bool is_dialect(const std::string& s);

// --- The pieces of resolution a first-run setup needs one at a time ---------
//
// resolve_provider() below does the whole job for a chat session. A program
// that walks the user through configuring one (tapto-code's setup prompts)
// needs to ask the same questions separately: is there a default provider,
// what dialect is it, which environment variable would carry its key, and is
// a key already available before prompting for one.

// The provider used when none is named on the command line: the `provider`
// key, else the legacy unscoped `provider-type`. nullopt when neither is set.
std::optional<std::string> default_provider_name();

// The dialect a provider name resolves to: its block's `-provider-type`, or the
// name itself when that is a dialect. Empty when the name isn't configured.
std::string provider_dialect(const std::string& name);

// The vendor's conventional environment variable for a dialect's API key
// (ANTHROPIC_API_KEY, OPENAI_API_KEY, GEMINI_API_KEY). "" for anything else;
// never null.
const char* api_key_env_var(const std::string& dialect);

// The API key for a provider block, in order: the block's own `<name>-api-key`
// (a literal or a secret reference), the dialect's environment variable, and
// for the default provider only, the unscoped `api-key`. Returns an unresolved
// Secret when none is configured, and one carrying `error` when a reference is
// configured but cannot be read -- the caller should report that rather than
// prompt for a replacement.
Secret resolve_api_key(const std::string& name, const std::string& dialect);

// A provider block resolved into everything a chat session needs.
struct ResolvedProvider {
    std::string name;    // the config block, e.g. "qwen36"
    std::string dialect; // claude | openai | gemini
    std::string url;
    std::string model;
    std::string reasoning_effort; // empty when unset; openai dialect only
    Secret api_key;      // unresolved if none is configured; the caller decides
};

// Resolve a provider name (empty for the configured default). Prints its own
// error and returns nullopt when the name names no dialect this program speaks.
std::optional<ResolvedProvider> resolve_provider(const std::string& requested);

// How the provider is shown to the user: the block name, plus the dialect when
// it adds something the name doesn't already say.
std::string provider_label(const ResolvedProvider& p);

} // namespace tapto
