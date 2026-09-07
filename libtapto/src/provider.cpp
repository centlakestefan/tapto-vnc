// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/provider.h"

#include <cstdlib>

#include "tapto/config.h"
#include "tapto/ui.h"

namespace tapto {

namespace {

// True if `key` ends with `suffix`, with at least one character before it.
bool has_suffix(const std::string& key, const std::string& suffix) {
    return key.size() > suffix.size() &&
           key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// If `key` is a provider-block key, the block's name; otherwise empty.
// `provider-type` alone is not one: it is the legacy unscoped key.
std::string provider_name_of_key(const std::string& key, const std::string& block_key) {
    const std::string suffix = "-" + block_key;
    if (!has_suffix(key, suffix)) return "";
    return key.substr(0, key.size() - suffix.size());
}

std::string default_url(const std::string& dialect) {
    if (dialect == "claude") return "https://api.anthropic.com";
    if (dialect == "openai") return "https://api.openai.com";
    if (dialect == "gemini") return "https://generativelanguage.googleapis.com";
    return "";
}

std::string default_model(const std::string& dialect) {
    if (dialect == "claude") return "claude-sonnet-4-6";
    if (dialect == "openai") return "gpt-4o";
    if (dialect == "gemini") return "gemini-2.0-flash";
    return "";
}

std::optional<std::string> env_value(const char* name) {
    if (!name || !*name) return std::nullopt;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // std::getenv is the portable, intended call here
#endif
    const char* v = std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (v && *v) return std::string(v);
    return std::nullopt;
}

// Every provider block the store defines, found by its `<name>-provider-type`
// key. Only used to name the alternatives when someone asks for a provider that
// isn't configured — a list of what exists is worth more than a list of what is
// allowed.
std::vector<std::string> configured_provider_names() {
    std::vector<std::string> names;
    for (const auto& entry : effective_config()) {
        if (entry.value.empty()) continue; // an empty value counts as unset
        std::string name = provider_name_of_key(entry.key, "provider-type");
        if (!name.empty()) names.push_back(std::move(name));
    }
    return names;
}

} // namespace

const char* api_key_env_var(const std::string& dialect) {
    if (dialect == "claude") return "ANTHROPIC_API_KEY";
    if (dialect == "openai") return "OPENAI_API_KEY";
    if (dialect == "gemini") return "GEMINI_API_KEY";
    return "";
}

// The provider used when none is named on the command line. `provider-type`
// doubles as the legacy spelling: a store saying `provider-type = claude` names
// the block "claude", whose dialect is claude because that is also a dialect
// name, so nothing needs rewriting.
std::optional<std::string> default_provider_name() {
    if (auto v = get_effective("provider")) return v;
    return get_effective("provider-type");
}

// The dialect a provider name resolves to: its block's `-provider-type`, or the
// name itself when that is a dialect. Empty when the name isn't configured.
std::string provider_dialect(const std::string& name) {
    if (auto v = get_effective(name + "-provider-type")) return *v;
    return is_dialect(name) ? name : "";
}

// Resolve the API key for a provider block. The block's own key comes first: it
// is the most specific thing the user wrote, it is the only thing that can be
// right when two blocks share a dialect, and — more sharply — an environment
// variable winning here would send a real vendor key to whatever
// `<name>-provider-url` points at, which for a local server means writing it
// into somebody's log.
//
// A configured value may name where the key lives — `env:`, `cmd:`, `wincred:`
// — instead of being the key; see tapto/secret.h. The vendor environment
// variable is a secret in its own right, never a reference, so it is taken
// verbatim.
Secret resolve_api_key(const std::string& name, const std::string& dialect) {
    if (auto v = get_effective(name + "-api-key")) return resolve_secret(*v);
    if (auto v = env_value(api_key_env_var(dialect))) {
        Secret s;
        s.value = *v;
        return s;
    }
    // The unscoped api-key belongs to the default provider only; otherwise one
    // vendor's key would be handed to another.
    if (auto def = default_provider_name(); def && *def == name) {
        if (auto v = get_effective("api-key")) return resolve_secret(*v);
    }
    return Secret{};
}

std::vector<EffectiveEntry> effective_config() {
    std::vector<EffectiveEntry> merged;

    auto apply = [&](Level level) {
        Config cfg = Config::load(config_path(level));
        for (const auto& entry : cfg.entries()) {
            bool found = false;
            for (auto& existing : merged) {
                if (existing.key == entry.first) {
                    existing.value = entry.second;
                    existing.origin = level;
                    found = true;
                    break;
                }
            }
            if (!found) merged.push_back({entry.first, entry.second, level});
        }
    };

    apply(Level::System);
    apply(Level::Global);
    apply(Level::Local);
    return merged;
}

std::optional<std::string> get_effective(const std::string& key) {
    for (const auto& entry : effective_config()) {
        if (entry.key == key) {
            if (entry.value.empty()) return std::nullopt;
            return entry.value;
        }
    }
    return std::nullopt;
}

bool is_dialect(const std::string& s) {
    return s == "claude" || s == "openai" || s == "gemini";
}

std::optional<ResolvedProvider> resolve_provider(const std::string& requested) {
    const std::string def = default_provider_name().value_or("claude");

    ResolvedProvider p;
    p.name = requested.empty() ? def : requested;
    p.dialect = provider_dialect(p.name);

    if (p.dialect.empty()) {
        std::string msg = "unknown provider '" + p.name + "'";
        auto names = configured_provider_names();
        if (!names.empty()) {
            msg += "; configured:";
            for (const auto& n : names) msg += " " + n;
        }
        msg += ".\n  name one by setting '" + p.name +
               "-provider-type' to claude, openai or gemini, "
               "or use claude, openai or gemini directly";
        ui::print_error(msg);
        return std::nullopt;
    }
    if (!is_dialect(p.dialect)) {
        ui::print_error("'" + p.name + "-provider-type' is '" + p.dialect +
                        "'; expected claude, openai or gemini."
                        "\n  that key names the API shape to speak, not the model");
        return std::nullopt;
    }

    // The unscoped keys belong to the default provider only. Otherwise a local
    // endpoint's URL and model would be sent to a hosted vendor, and vice versa.
    const bool is_default = (p.name == def);
    auto scoped = [&](const std::string& key) -> std::optional<std::string> {
        if (auto v = get_effective(p.name + "-" + key)) return v;
        if (is_default) return get_effective(key);
        return std::nullopt;
    };

    p.url = scoped("provider-url").value_or(default_url(p.dialect));
    p.model = scoped("model").value_or(default_model(p.dialect));
    p.reasoning_effort = scoped("reasoning-effort").value_or("");
    if (!p.reasoning_effort.empty() && p.dialect != "openai") {
        ui::print_warning("'" + p.name + "-reasoning-effort' is ignored: only the "
                          "openai dialect sends it");
        p.reasoning_effort.clear();
    }
    p.api_key = resolve_api_key(p.name, p.dialect);
    return p;
}

std::string provider_label(const ResolvedProvider& p) {
    if (p.name == p.dialect) return p.name;
    return p.name + " (" + p.dialect + ")";
}

} // namespace tapto
