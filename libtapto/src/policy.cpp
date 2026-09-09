// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/policy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "tapto/paths.h"

namespace tapto {

namespace {

std::string trim(const std::string& s) {
    static const char* ws = " \t\r\n";
    size_t begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return std::string();
    size_t end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Insert or replace, keeping first-seen order. An empty value is no policy at
// all -- the key is left to the user -- so it is dropped rather than stored as
// "set to nothing", which get_effective() would treat as unset anyway but
// is_policy_managed() would not.
void put(std::vector<Config::Entry>& out, const std::string& key, const std::string& value) {
    const std::string k = trim(key);
    const std::string v = trim(value);
    if (k.empty()) return;
    auto it = std::find_if(out.begin(), out.end(), [&](const Config::Entry& e) { return e.first == k; });
    if (v.empty()) {
        if (it != out.end()) out.erase(it);
        return;
    }
    if (it != out.end()) it->second = v;
    else out.emplace_back(k, v);
}

std::optional<std::string> find(const std::vector<Config::Entry>& entries, const std::string& key) {
    for (const auto& e : entries) {
        if (e.first == key) return e.second;
    }
    return std::nullopt;
}

// "a, b,c" -> {"a", "b", "c"}
std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string::npos) comma = s.size();
        std::string item = trim(s.substr(start, comma - start));
        if (!item.empty()) out.push_back(std::move(item));
        start = comma + 1;
    }
    return out;
}

// How a boolean policy reads as false: a DWORD 0 arrives as "0"; a file may
// say the word.
bool is_false(const std::string& v) {
    const std::string l = lower(trim(v));
    return l == "0" || l == "false" || l == "no" || l == "off";
}

// The provider names the policy itself defines: its default plus every block
// it gives a dialect. Used both to allow them and to name them in a refusal.
std::vector<std::string> policy_defined_providers(const std::vector<Config::Entry>& policy) {
    std::vector<std::string> names;
    auto add = [&](const std::string& n) {
        if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    if (auto def = find(policy, "provider")) add(*def);
    const std::string suffix = "-provider-type";
    for (const auto& e : policy) {
        if (e.first.size() > suffix.size() &&
            e.first.compare(e.first.size() - suffix.size(), suffix.size(), suffix) == 0) {
            add(e.first.substr(0, e.first.size() - suffix.size()));
        }
    }
    return names;
}

#ifdef _WIN32

const wchar_t kPolicySubkey[] = L"SOFTWARE\\Policies\\Centlake\\tapto";

// Subkeys of the policy key that are stores of their own (see the header):
// free-form settings read as keys of the parent, and the command allow-list.
const wchar_t kSettingsSubkey[] = L"settings";
const wchar_t kCommandsSubkey[] = L"commands";

std::string narrow(const wchar_t* s, int len) {
    if (len <= 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, &out[0], n, nullptr, nullptr);
    return out;
}

std::string narrow(const std::wstring& s) {
    return narrow(s.data(), static_cast<int>(s.size()));
}

// A registry value as config text. Strings are taken as they are; a DWORD is
// its decimal, so the ADMX boolean/decimal elements arrive as "1"/"0"/"200";
// a MULTI_SZ becomes a comma-separated list. Other types are not config and
// yield "", which put() drops.
std::string value_text(DWORD type, const BYTE* data, DWORD size) {
    switch (type) {
        case REG_SZ:
        case REG_EXPAND_SZ: {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
            int len = static_cast<int>(size / sizeof(wchar_t));
            while (len > 0 && w[len - 1] == 0) --len; // the stored terminator(s)
            return narrow(w, len);
        }
        case REG_DWORD: {
            if (size < sizeof(DWORD)) return "";
            DWORD v = 0;
            std::memcpy(&v, data, sizeof(v));
            return std::to_string(v);
        }
        case REG_QWORD: {
            if (size < sizeof(unsigned long long)) return "";
            unsigned long long v = 0;
            std::memcpy(&v, data, sizeof(v));
            return std::to_string(v);
        }
        case REG_MULTI_SZ: {
            const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
            const size_t len = size / sizeof(wchar_t);
            std::string out;
            size_t start = 0;
            while (start < len && w[start] != 0) {
                size_t end = start;
                while (end < len && w[end] != 0) ++end;
                std::string item = trim(narrow(w + start, static_cast<int>(end - start)));
                if (!item.empty()) {
                    if (!out.empty()) out += ", ";
                    out += item;
                }
                start = end + 1;
            }
            return out;
        }
        default:
            return "";
    }
}

// Every value directly under `key`, as (name, text).
std::vector<Config::Entry> read_values(HKEY key) {
    std::vector<Config::Entry> out;
    DWORD count = 0, max_name = 0, max_data = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &count, &max_name,
                         &max_data, nullptr, nullptr) != ERROR_SUCCESS) {
        return out;
    }
    std::wstring name(max_name + 1, 0);
    std::vector<BYTE> data(max_data + sizeof(wchar_t) * 2);
    for (DWORD i = 0; i < count; ++i) {
        DWORD name_len = static_cast<DWORD>(name.size());
        DWORD data_len = static_cast<DWORD>(data.size());
        DWORD type = 0;
        if (RegEnumValueW(key, i, &name[0], &name_len, nullptr, &type, data.data(), &data_len) !=
            ERROR_SUCCESS) {
            continue;
        }
        std::string value_name = narrow(name.data(), static_cast<int>(name_len));
        // `**delvals.` is the marker the Group Policy client writes into a key
        // it manages when applying a list element. It is never a setting, and
        // left in it would spoil the numeric ordering of a list's items.
        if (value_name.compare(0, 2, "**") == 0) continue;
        out.emplace_back(std::move(value_name), value_text(type, data.data(), data_len));
    }
    return out;
}

// An ADMX list element stores its items as a subkey holding values named
// 1, 2, 3, ... (or the items' own names, for a list with explicit names).
// Either way the subkey's name is the config key and its values, in numeric
// order where the names are numbers, are the comma-separated list.
//
// Two subkeys are not lists: `settings` holds config keys of its own, which
// are read as if they sat in the parent key, and `commands` is the command
// allow-list, read by policy_commands_from_registry().
void read_list_subkeys(HKEY key, std::vector<Config::Entry>& out) {
    DWORD count = 0, max_name = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &count, &max_name, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
        return;
    }
    std::wstring name(max_name + 1, 0);
    for (DWORD i = 0; i < count; ++i) {
        DWORD name_len = static_cast<DWORD>(name.size());
        if (RegEnumKeyExW(key, i, &name[0], &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            continue;
        }
        const std::string sub_name = narrow(name.data(), static_cast<int>(name_len));
        const std::string kind = lower(sub_name); // registry key names are case-insensitive
        if (kind == narrow(kCommandsSubkey)) continue;

        HKEY sub = nullptr;
        if (RegOpenKeyExW(key, name.c_str(), 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        std::vector<Config::Entry> items = read_values(sub);
        RegCloseKey(sub);

        if (kind == narrow(kSettingsSubkey)) {
            for (const auto& item : items) put(out, item.first, item.second);
            continue;
        }

        auto numeric = [](const std::string& s) {
            return !s.empty() &&
                   std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        };
        const bool all_numeric =
            std::all_of(items.begin(), items.end(), [&](const Config::Entry& e) { return numeric(e.first); });
        std::stable_sort(items.begin(), items.end(), [&](const Config::Entry& a, const Config::Entry& b) {
            if (all_numeric) return std::stoull(a.first) < std::stoull(b.first);
            return a.first < b.first;
        });
        std::string joined;
        for (const auto& item : items) {
            const std::string v = trim(item.second);
            if (v.empty()) continue;
            if (!joined.empty()) joined += ", ";
            joined += v;
        }
        put(out, sub_name, joined);
    }
}

#endif // _WIN32

} // namespace

std::vector<Config::Entry> policy_entries_from_file(const std::filesystem::path& path) {
    std::vector<Config::Entry> out;
    if (path.empty()) return out;
    for (const auto& e : Config::load(path).entries()) put(out, e.first, e.second);
    return out;
}

#ifdef _WIN32

const wchar_t* policy_registry_subkey() {
    return kPolicySubkey;
}

std::vector<Config::Entry> policy_entries_from_registry(HKEY__* root, const std::wstring& subkey) {
    std::vector<Config::Entry> out;
    HKEY key = nullptr;
    // Policies is one of the keys Windows shares between the 32- and 64-bit
    // views, so no KEY_WOW64_* flag is needed for either build to see what
    // Group Policy wrote.
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (const auto& e : read_values(key)) put(out, e.first, e.second);
    read_list_subkeys(key, out);
    RegCloseKey(key);
    return out;
}

std::vector<Config::Entry> policy_commands_from_registry(HKEY__* root, const std::wstring& subkey) {
    std::vector<Config::Entry> out;
    HKEY key = nullptr;
    const std::wstring path = subkey + L"\\" + kCommandsSubkey;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (const auto& e : read_values(key)) put(out, e.first, e.second);
    RegCloseKey(key);
    return out;
}

std::vector<Config::Entry> policy_entries() {
    // User policy first, machine policy over it: an administrator who sets a
    // key for the whole machine means it for every account on it.
    std::vector<Config::Entry> out = policy_entries_from_registry(HKEY_CURRENT_USER, kPolicySubkey);
    for (const auto& e : policy_entries_from_registry(HKEY_LOCAL_MACHINE, kPolicySubkey)) {
        put(out, e.first, e.second);
    }
    return out;
}

std::vector<Config::Entry> policy_commands() {
    std::vector<Config::Entry> out = policy_commands_from_registry(HKEY_CURRENT_USER, kPolicySubkey);
    for (const auto& e : policy_commands_from_registry(HKEY_LOCAL_MACHINE, kPolicySubkey)) {
        put(out, e.first, e.second);
    }
    return out;
}

std::string policy_source() {
    return "HKLM\\" + narrow(kPolicySubkey) + " (or HKCU)";
}

#else

std::vector<Config::Entry> policy_entries() {
    return policy_entries_from_file(config_path(Level::Policy));
}

std::vector<Config::Entry> policy_commands() {
    return policy_entries_from_file(commands_path(Level::Policy));
}

std::string policy_source() {
    return config_path(Level::Policy).string();
}

#endif

// Whether either restriction confines the user's choice of provider.
bool policy_confines_providers(const std::vector<Config::Entry>& policy) {
    if (auto allowed = find(policy, "allowed-providers"); allowed && !split_list(*allowed).empty()) return true;
    auto v = find(policy, "allow-user-providers");
    return v && is_false(*v);
}

bool is_policy_managed(const std::string& key) {
    return find(policy_entries(), key).has_value();
}

bool policy_allows_user_commands() {
    return policy_allows_user_commands(policy_entries());
}

bool policy_allows_user_commands(const std::vector<Config::Entry>& policy) {
    auto v = find(policy, "allow-user-commands");
    return !(v && is_false(*v));
}

std::string provider_policy_refusal(const std::string& name) {
    return provider_policy_refusal(name, policy_entries());
}

std::string provider_policy_refusal(const std::string& name, const std::vector<Config::Entry>& policy) {
    auto list_of = [](const std::vector<std::string>& names) {
        std::string s;
        for (const auto& n : names) s += " " + n;
        return s;
    };

    // An explicit allow-list is the whole answer when there is one.
    if (auto allowed = find(policy, "allowed-providers")) {
        const auto names = split_list(*allowed);
        if (!names.empty()) {
            if (std::find(names.begin(), names.end(), name) != names.end()) return "";
            return "provider '" + name + "' is not allowed by your organization's policy; allowed:" +
                   list_of(names);
        }
    }

    // Otherwise the user may be confined to what the policy itself configures.
    if (auto v = find(policy, "allow-user-providers"); v && is_false(*v)) {
        const auto names = policy_defined_providers(policy);
        if (std::find(names.begin(), names.end(), name) != names.end()) return "";
        std::string msg = "provider '" + name +
                          "' is not one your organization's policy configures, and the policy allows no others";
        if (!names.empty()) msg += "; configured by policy:" + list_of(names);
        return msg;
    }

    return "";
}

std::string provider_url_policy_refusal(const std::string& key, Level origin) {
    return provider_url_policy_refusal(key, origin, policy_entries());
}

std::string provider_url_policy_refusal(const std::string& key, Level origin,
                                        const std::vector<Config::Entry>& policy) {
    if (origin == Level::Policy || !policy_confines_providers(policy)) return "";
    return "'" + key + "' is set in your " + level_name(origin) +
           " config, but your organization's policy confines providers to its own; "
           "their endpoints can only be set by policy. Remove the key, or ask your "
           "administrator to set it";
}

} // namespace tapto
