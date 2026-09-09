// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tapto/config.h"
#include "tapto/paths.h" // Level

// ---------------------------------------------------------------------------
// Organization policy: config an administrator mandates, which the user
// cannot change.
//
// The user's three scopes (system, global, local) are files the programs read
// and write, and the highest one wins. Policy is a fourth scope applied after
// all of them, so a key set by policy wins over anything the user wrote, and
// the programs refuse to `config set` it. A key the policy does not set is
// left to the user. That is the whole locking model: presence is enforcement.
//
// Where it comes from:
//
//   Windows   HKLM\SOFTWARE\Policies\Centlake\tapto   (machine, wins)
//             HKCU\SOFTWARE\Policies\Centlake\tapto   (user)
//   others    /etc/tapto/policy                        (same key = value format)
//
// The registry keys are what Group Policy writes from the tapto.admx template
// (admx/ in the repo) and what Intune writes through ADMX ingestion. Windows
// keeps them writable by administrators only and Group Policy re-applies them
// on every refresh, which is why they are used rather than a file under
// ProgramData: a file's protection depends on the machine's ACLs, and a
// user-created file there belongs to the user.
//
// Value names are the config keys, verbatim: `provider`, `work-provider-type`,
// `work-provider-url`, `work-model`, `work-api-key`, and every other key the
// programs read. REG_SZ is the value; REG_DWORD is its decimal text, so a
// boolean policy arrives as "1" or "0"; REG_MULTI_SZ, and a subkey holding
// values named 1, 2, 3, ... (how an ADMX list element is stored), become one
// comma-separated value. An empty value is not a policy: it is dropped, and
// the user's own value stands.
//
// Two subkeys are stores of their own rather than config keys:
//
//   ...\tapto\settings   free-form `key = value` pairs, read as if they were
//                        values of the parent key. The template's "Additional
//                        settings" list writes here and not into the parent,
//                        because Group Policy clears a list's key before it
//                        rewrites the list and would take every other setting
//                        in the key with it.
//   ...\tapto\commands   the organization's allow-listed commands, name =
//                        command line: the shape of the user's commands store
//                        (tapto/commands.h). Elsewhere it is the file
//                        /etc/tapto/policy-commands.
//
// A value named `**delvals.` (or anything else starting with `**`) is a marker
// the Group Policy client leaves in a key it manages, never a setting; the
// readers skip it.
//
// A policy must not carry an API key: Group Policy objects are readable by
// every account in the domain. It carries a reference (`wincred:tapto/work`,
// `env:`, `cmd:`; see tapto/secret.h) and the credential travels separately,
// or it points `-provider-url` at a gateway that authenticates the user.
//
// Two keys exist only as policy and restrict which provider the user may pick
// with --provider or `provider =`:
//
//   allowed-providers = work, review    only these names (comma-separated)
//   allow-user-providers = 0            only blocks the policy itself defines
//                                       (`<name>-provider-type` set by policy,
//                                       or the policy's own `provider`)
//
// Neither set means any configured provider is fine, as before. Either set
// also fixes where a permitted block points: its `-provider-url` may come from
// policy or be the vendor's default, never from a user scope, since a user who
// could redirect the block would have a personal endpoint after all. The
// template requires the URL for that reason; this rule covers policies written
// another way.
//
// One more confines the command allow-list the same way:
//
//   allow-user-commands = 0             only the commands the policy defines
//                                       (its `commands` store) can be run or
//                                       listed; the user's own stores are
//                                       ignored and `command add` is refused
//
// Policy commands are merged above the user's whether or not that key is set,
// so a name the policy defines cannot be redefined in a user scope.
// ---------------------------------------------------------------------------

struct HKEY__; // <windows.h> is not dragged into every consumer for one handle

namespace tapto {

// The policy in force on this machine for this user, merged: on Windows HKCU
// then HKLM (so the machine overrides the user), elsewhere the policy file.
// Empty values are already dropped. First-seen key order.
std::vector<Config::Entry> policy_entries();

// The organization's allow-listed commands, (name, command line), merged the
// same way. Empty when the policy defines none.
std::vector<Config::Entry> policy_commands();

// Where the policy is read from, for messages: the registry key or file path.
std::string policy_source();

// True when policy sets `key`, so the user's programs must not let it be
// changed. (`config set` refuses; setup does not prompt for it.)
bool is_policy_managed(const std::string& key);

// False when policy sets `allow-user-commands` to 0/false: the user's own
// command stores are ignored and `command add` is refused.
bool policy_allows_user_commands();
bool policy_allows_user_commands(const std::vector<Config::Entry>& policy);

// Why policy forbids using the provider `name`, or "" when it is allowed.
// Applies `allowed-providers` and `allow-user-providers` (see above).
std::string provider_policy_refusal(const std::string& name);
std::string provider_policy_refusal(const std::string& name,
                                    const std::vector<Config::Entry>& policy);

// Why policy forbids the effective `<name>-provider-url` (or `provider-url`)
// entry `key`, set at `origin`, or "" when it is fine: with providers confined
// by either key above, an endpoint from a user scope is refused.
std::string provider_url_policy_refusal(const std::string& key, Level origin);
std::string provider_url_policy_refusal(const std::string& key, Level origin,
                                        const std::vector<Config::Entry>& policy);

// --- The readers, one per source ---------------------------------------------
// Public so the library test can exercise them against a scratch file and a
// scratch registry key without administrator rights; the programs call
// policy_entries().

// Parse a policy file (the config store's `key = value` format). A policy
// commands file has the same shape and is read with the same call.
std::vector<Config::Entry> policy_entries_from_file(const std::filesystem::path& path);

#ifdef _WIN32
// Read `subkey` under `root` (HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, ...) as
// described above. Missing key: empty.
std::vector<Config::Entry> policy_entries_from_registry(HKEY__* root, const std::wstring& subkey);

// Read the `commands` subkey of `subkey` under `root` as (name, command line).
// Missing key: empty.
std::vector<Config::Entry> policy_commands_from_registry(HKEY__* root, const std::wstring& subkey);

// The subkey Group Policy writes: SOFTWARE\Policies\Centlake\tapto.
const wchar_t* policy_registry_subkey();
#endif

} // namespace tapto
