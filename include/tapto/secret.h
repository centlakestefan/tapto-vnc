// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

namespace tapto {

// Secrets in the config store may be written down, or the store may say where
// the secret lives instead of holding it:
//
//   sk-ant-...                     the secret itself
//   env:ANTHROPIC_API_KEY          an environment variable
//   cmd:op read op://work/key      the first line of a command's output
//   wincred:tapto/work-claude      a Windows Credential Manager generic credential
//
// The indirect forms keep the key out of a plaintext file that gets backed up,
// synced, screen-shared or pasted into an issue, while the store stays the one
// place that says which provider uses which key.
struct Secret {
    std::string value; // the resolved secret; empty if it could not be resolved
    std::string error; // why a reference failed; empty when nothing went wrong

    bool ok() const { return !value.empty(); }
};

// True when `value` names where a secret lives rather than being one. Such a
// value is not itself sensitive, so it is shown rather than masked.
bool is_secret_reference(const std::string& value);

// Resolve a config value. A value with no recognized scheme is the secret
// itself and is returned unchanged.
//
// A reference that fails to resolve yields an error, never an empty value: the
// caller must not quietly fall through to some other source, because that is
// exactly how one endpoint ends up being handed another one's key.
Secret resolve_secret(const std::string& value);

} // namespace tapto
