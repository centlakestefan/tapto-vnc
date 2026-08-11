// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <any>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "tool_registry.h"

// Minimal run context handed to an AI backend during a chat: the tools the
// model may call, plus a small typed key/value bag shared with the tools so a
// tool can stash state for later turns.
//
class Context {
public:
    std::vector<ToolSpec> tools;

    bool has(const std::string& key) const { return m_store.count(key) > 0; }
    void remove(const std::string& key) { m_store.erase(key); }

    template <class T>
    void set(const std::string& key, T value) {
        m_store[key] = std::any(std::move(value));
    }

    template <class T>
    T get(const std::string& key) const {
        auto it = m_store.find(key);
        if (it == m_store.end()) {
            throw std::runtime_error("Context: missing key '" + key + "'");
        }
        return std::any_cast<T>(it->second);
    }

private:
    std::map<std::string, std::any> m_store;
};
