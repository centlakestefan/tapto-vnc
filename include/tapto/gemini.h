// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

// gemini.h
#pragma once
#include <string>
#include <functional>
#include <map>
#include <vector>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include <httplib.h>

#include "aibackend.h"
#include "tool_registry.h"
#include "context.h"
#include "aiconfig.h"

class GeminiClient : public AiBackend {
private:
    // Type alias for the client's internal tool executor (context already bound)
    using ToolExecutor = std::function<std::string(const nlohmann::json&)>;

    const AiConfig*     m_config;
    std::string         m_url;
    std::string         m_model;
    std::string         m_apiKeyRef;
    std::string         m_systemInstruction; // Equivalent to systemPrompt
    std::optional<int>  m_thinkingBudget;    // nullopt = server default; 0 = off; >0 = on with budget

    // Persistent client for connection reuse (can be SSL or non-SSL)
    std::unique_ptr<httplib::Client> m_api_client;

    // Tool registry: maps tool name to executor function
    std::map<std::string, ToolExecutor> m_tool_registry;

    // Tool definitions for Gemini API
    std::vector<ToolSpec> m_tools;

    // Persistent conversation history for context across multiple process() calls
    nlohmann::json      m_conversation_history;

    void init_api_client();

    // Resolves the Gemini API key. Stubbed to read the environment variable
    // named by m_apiKeyRef until wired to the tapto-code config store.
    std::string getApiKey();

public:
    GeminiClient(const AiConfig* config, const std::string& host, const std::string& model, const std::string& apiKeyRef);

    void setSystemPrompt(const std::string& systemPrompt) override {
        m_systemInstruction = systemPrompt;
    }

    const std::string& getSystemPrompt() const override {
        return m_systemInstruction;
    }

    void setModel(const std::string& model) override;
    void setHost(const std::string& host) override;
    void setApiKeyRef(const std::string& apiKeyRef) override;
    void setThinkingBudget(std::optional<int> budget) override { m_thinkingBudget = budget; }

    // It takes the full history for prompt caching
    nlohmann::json call_gemini(
        const std::string& user_message,
        const nlohmann::json& tool_schemas,
        const nlohmann::json& conversation_history
    );

    // Main chat loop (runs any tools the model calls, returns its text reply)
    std::string chat(Context& context, const std::string& user_message) override;

    // History management for context persistence
    void start() override;
    bool hasHistory() const override;
    void loadHistory(const nlohmann::json& history) override;
    nlohmann::json getHistory() const override;
};
