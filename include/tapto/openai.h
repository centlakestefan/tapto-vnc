// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

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

class OpenAIClient : public AiBackend {
private:
    // Type alias for the client's internal tool executor (context already bound)
    using ToolExecutor = std::function<std::string(const nlohmann::json&)>;

    // Retry limit constants
    static constexpr int MAX_CONNECTION_RETRIES = 5;
    static constexpr int MAX_RATE_LIMIT_RETRIES = 10;
    static constexpr int MAX_SERVER_ERROR_RETRIES = 5;

    // Backoff constants (in seconds)
    static constexpr int CONNECTION_BACKOFF_BASE = 2;
    static constexpr int CONNECTION_BACKOFF_MAX = 32;
    static constexpr int RATE_LIMIT_BACKOFF_BASE = 5;
    static constexpr int RATE_LIMIT_BACKOFF_MAX = 300;
    static constexpr int SERVER_ERROR_BACKOFF_BASE = 5;
    static constexpr int SERVER_ERROR_BACKOFF_MAX = 120;

    const AiConfig*     m_config;
    std::string         m_url;
    std::string         m_model;
    std::string         m_apiKeyRef;
    std::string         m_systemPrompt;
    std::optional<int>  m_thinkingBudget;  // nullopt = server default; 0 = off; >0 = on (budget ignored, just a toggle)

    // Persistent client for connection reuse (can be SSL or non-SSL)
    std::unique_ptr<httplib::Client> m_api_client;

    // Tool registry: maps tool name to executor function
    std::map<std::string, ToolExecutor> m_tool_registry;

    // Tool definitions for OpenAI API
    std::vector<ToolSpec> m_tools;

    // Persistent conversation history for context across multiple process() calls
    nlohmann::json      m_conversation_history;

    void init_api_client();

    // Resolves the OpenAI API key. Stubbed to read the environment variable
    // named by m_apiKeyRef until wired to the tapto-code config store.
    std::string getApiKey();

public:
    OpenAIClient(const AiConfig* config, const std::string& host, const std::string& model, const std::string& apiKeyRef);

    void setSystemPrompt(const std::string& systemPrompt) override { m_systemPrompt = systemPrompt; }
    const std::string& getSystemPrompt() const override { return m_systemPrompt; }

    void setModel(const std::string& model) override;
    void setHost(const std::string& host) override;
    void setApiKeyRef(const std::string& apiKeyRef) override;
    void setThinkingBudget(std::optional<int> budget) override { m_thinkingBudget = budget; }

    nlohmann::json call_openai(const std::string& user_message, const nlohmann::json& tools, const nlohmann::json& conversation_history = nlohmann::json::array());

    std::string chat(Context& context, const std::string& user_message) override;

    // History management for context persistence
    void start() override;
    bool hasHistory() const override;
    void loadHistory(const nlohmann::json& history) override;
    nlohmann::json getHistory() const override;
};
