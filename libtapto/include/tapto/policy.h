// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "tapto/config.h"

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
// Neither set means any configured provider is fine, as before.
// ---------------------------------------------------------------------------

struct HKEY__; // <windows.h> is not dragged into every consumer for one handle

namespace tapto {

// The policy in force on this machine for this user, merged: on Windows HKCU
// then HKLM (so the machine overrides the user), elsewhere the policy file.
// Empty values are already dropped. First-seen key order.
std::vector<Config::Entry> policy_entries();

// Where the policy is read from, for messages: the registry key or file path.
std::string policy_source();

// True when policy sets `key`, so the user's programs must not let it be
// changed. (`config set` refuses; setup does not prompt for it.)
bool is_policy_managed(const std::string& key);

// Why policy forbids using the provider `name`, or "" when it is allowed.
// Applies `allowed-providers` and `allow-user-providers` (see above).
std::string provider_policy_refusal(const std::string& name);
std::string provider_policy_refusal(const std::string& name,
                                    const std::vector<Config::Entry>& policy);

// --- The readers, one per source ---------------------------------------------
// Public so the library test can exercise them against a scratch file and a
// scratch registry key without administrator rights; the programs call
// policy_entries().

// Parse a policy file (the config store's `key = value` format).
std::vector<Config::Entry> policy_entries_from_file(const std::filesystem::path& path);

#ifdef _WIN32
// Read `subkey` under `root` (HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER, ...) as
// described above. Missing key: empty.
std::vector<Config::Entry> policy_entries_from_registry(HKEY__* root, const std::wstring& subkey);

// The subkey Group Policy writes: SOFTWARE\Policies\Centlake\tapto.
const wchar_t* policy_registry_subkey();
#endif

} // namespace tapto
