// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tapto {

// In-memory view of a single config file. Entries keep insertion order so
// listing and rewriting stay stable.
class Config {
public:
    using Entry = std::pair<std::string, std::string>;

    // Load a config file. A missing file yields an empty Config (not an error).
    static Config load(const std::filesystem::path& path);

    std::optional<std::string> get(const std::string& key) const;

    // Insert or update a key.
    void set(const std::string& key, const std::string& value);

    // Remove a key; returns true if it existed.
    bool unset(const std::string& key);

    const std::vector<Entry>& entries() const { return entries_; }

    // Serialize to disk, creating parent directories. Throws on I/O failure.
    void save(const std::filesystem::path& path) const;

private:
    std::vector<Entry> entries_;
};

} // namespace tapto
