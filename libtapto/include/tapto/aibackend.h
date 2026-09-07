// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

class Context;

// Abstract interface every provider client implements. tapto-code is a chat
// client: chat() sends one user message and returns the model's text reply,
// running any tools the model calls along the way and retaining conversation
// history across calls for multi-turn dialogue.
class AiBackend {
public:
    virtual ~AiBackend() = default;

    virtual void setSystemPrompt(const std::string& systemPrompt) = 0;
    virtual const std::string& getSystemPrompt() const = 0;

    virtual void setModel(const std::string& model) = 0;
    virtual void setHost(const std::string& host) = 0;
    virtual void setApiKeyRef(const std::string& apiKey) = 0;
    virtual void setThinkingBudget(std::optional<int> budget) = 0;

    // Send one user message and return the model's text reply. Tools supplied
    // via context.tools are executed as the model requests them.
    virtual std::string chat(Context& context, const std::string& user_message) = 0;

    // Conversation-history management for context persistence across calls.
    virtual void start() = 0;
    virtual bool hasHistory() const = 0;
    virtual void loadHistory(const nlohmann::json& history) = 0;
    virtual nlohmann::json getHistory() const = 0;

    // Input tokens the provider reported for the most recent request (usage.
    // input_tokens / usageMetadata.promptTokenCount) — i.e. how full the
    // context window is right now. 0 until the first turn has run, or when a
    // provider does not report it.
    virtual std::size_t lastInputTokens() const = 0;

    // Reset the request accounting (start() re-clears the conversation and
    // beginWithSummary() replaces it with a /compact summary).
    virtual void resetTokenAccounting() { m_lastInputTokens = 0; }
    std::size_t m_lastInputTokens = 0;

    // Replace the conversation with a fresh one whose first user turn carries
    // a summary of the discussion up to this point (used by the /compact
    // command). Providers must format the seed message in their native shape.
    virtual void beginWithSummary(const std::string& summaryText) = 0;

    // Compaction input trimming (used by the /compact command). Summarizing
    // the conversation means *sending it* -- so this is the single largest
    // request of the session, carrying every file dump the tools produced.
    // buildTrimmedHistoryForSummary() returns a *copy* of the current history
    // with any oversized tool-result payload replaced by a short placeholder,
    // so that request stays small and leaves output-token headroom on providers
    // that share one window for input + output (vLLM, Ollama, LM Studio, ...).
    //
    // The live conversation (m_conversation_history) is never touched: on a
    // successful compact it is replaced by the summary, and on failure the
    // caller restores the original history it snapshot. Only *large results*
    // are shrunk -- the tool *calls* (which file was read, what was edited) are
    // left intact, and that pairing is exactly what the summarizer needs.
    //
    // Header-only on purpose: it walks the three history shapes by their JSON
    // and needs nothing from the provider, so every backend gets it for free.
    nlohmann::json buildTrimmedHistoryForSummary() {
        using json = nlohmann::json;
        json history = getHistory();
        if (!history.is_array()) return history;
        const std::size_t limit = kCompactTrimPayloadChars;

        auto placeholder = [](const std::size_t len) {
            return std::string("[omitted tool output (")
                 + std::to_string(len) + " chars)]";
        };
        // Shrink a payload in place if it exceeds the limit; leave it alone if
        // not (or if it is not a string -- short/structured output is kept).
        auto shrink = [&](json& c) {
            if (c.is_string()) {
                std::string s = c.get<std::string>();
                if (s.size() > limit) c = placeholder(s.size());
            }
        };

        for (auto& msg : history) {
            if (!msg.is_object()) continue;

            // OpenAI: a tool result is a message of role "tool" with a string
            // "content" field.
            if (msg.value("role", std::string()) == "tool" && msg.contains("content"))
                shrink(msg["content"]);

            // Claude: a user turn whose "content" is a block array; each result
            // is a block of type "tool_result" carrying a string "content".
            if (msg.contains("content") && msg["content"].is_array())
                for (auto& block : msg["content"])
                    if (block.is_object()
                        && block.value("type", std::string()) == "tool_result"
                        && block.contains("content"))
                        shrink(block["content"]);

            // Gemini: a part may hold a functionResponse with a "response.
            //  content" string.
            if (msg.contains("parts") && msg["parts"].is_array())
                for (auto& part : msg["parts"])
                    if (part.is_object() && part.contains("functionResponse")
                        && part["functionResponse"].is_object()
                        && part["functionResponse"].contains("response")
                        && part["functionResponse"]["response"].is_object()
                        && part["functionResponse"]["response"].contains("content"))
                        shrink(part["functionResponse"]["response"]["content"]);
        }
        return history;
    }

private:
    // Tool-result payloads longer than this many characters are replaced by a
    // short placeholder when building the /compact summarization input (see
    // buildTrimmedHistoryForSummary). Short outputs (status lines, small reads)
    // are kept verbatim; long ones (whole-file dumps, large greps) are not.
    static constexpr std::size_t kCompactTrimPayloadChars = 2000;
};
