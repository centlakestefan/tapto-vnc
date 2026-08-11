// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/gemini.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "tapto/base64.h"
#include "tapto/log.h"
#include "tapto/tool_image.h"
#include "tapto/ui.h"

using json = nlohmann::json;
namespace ui = tapto::ui;

// --- GeminiClient Method Implementations ---

GeminiClient::GeminiClient(const AiConfig* config, const std::string& url, const std::string& model, const std::string& apiKey)
    : m_config(config), m_model(model), m_apiKeyRef(apiKey) {
    m_url = url;

    mclog("GeminiClient initialized with url=" + m_url + "\n");
    // API client is created lazily on first chat() call (matches Claude/OpenAI).
}

/**
 * @brief Initializes the persistent client for API connection reuse.
 */
void GeminiClient::init_api_client() {
    mclog("Initializing Gemini API client connection to " + m_url + "\n");

    m_api_client = std::make_unique<httplib::Client>(m_url);
    // Certificate verification only applies to https endpoints; skip it for plain http.
    if (m_url.rfind("https://", 0) == 0) {
        m_api_client->enable_server_certificate_verification(true);
    }

    m_api_client->set_connection_timeout(30);
    m_api_client->set_read_timeout(120);
    m_api_client->set_keep_alive(true);

    mclog("Gemini API client initialized with keep-alive enabled\n");
}

/**
 * @brief Returns the configured Gemini API key.
 */
std::string GeminiClient::getApiKey() {
    if (!m_apiKeyRef.empty()) {
        return m_apiKeyRef;
    }
    throw std::runtime_error("Gemini API key is not set");
}

/**
 * @brief Makes a single call to the Gemini API, sending the full conversation history (prompt caching).
 */
nlohmann::json GeminiClient::call_gemini(
    const std::string& user_message,
    const nlohmann::json& tool_schemas,
    const nlohmann::json& conversation_history
) {
    std::ostringstream path_stream;
    path_stream << "/v1beta/models/" << m_model << ":generateContent";
    const std::string path = path_stream.str();

    // Build Gemini 'contents' array (history + new user message)
    json contents = conversation_history;

    // Add the new user message (if present)
    if (!user_message.empty()) {
        contents.push_back({
            {"role", "user"},
            {"parts", {
                {{"text", user_message}}
            }}
            });
    }

    // Build request body
    json request_body = {
        {"contents", contents}
    };

    // Add tools (function declarations) at top level
    if (!tool_schemas.empty()) {
        request_body["tools"] = json::array({
            {{"function_declarations", tool_schemas}}
            });

        // Add tool configuration to enforce proper function calling
        request_body["tool_config"] = {
            {"function_calling_config", {
                {"mode", "AUTO"}  // AUTO allows the model to decide when to call functions
            }}
        };
    }

    // Add system instruction (if present) at top level
    if (!m_systemInstruction.empty()) {
        request_body["system_instruction"] = {
            {"parts", {
                {{"text", m_systemInstruction}}
            }}
        };
    }

    // Configure generation settings
    request_body["generationConfig"] = {
        {"temperature", 1.0},
        {"topP", 0.95},
        {"topK", 40},
        {"maxOutputTokens", m_config->maxOutputTokens()}
    };

    // Provider-level extended-thinking override.
    //
    // Gemini 3 replaced thinkingBudget with thinkingLevel and rejects a budget
    // of 0 outright — HTTP 400 "Request contains an invalid argument", which
    // gives no hint that this field is the cause. Measured against
    // gemini-3.6-flash: no thinkingConfig, thinkingBudget=1024 and
    // thinkingLevel="low" all succeed; thinkingBudget=0 alone fails.
    //
    // There is also no true off switch on these models: "low" is the floor, so
    // --thinking off means as little thinking as the model allows rather than
    // none. Leaving --thinking at its default sends no field at all, which is
    // the safe choice for an older model that predates thinkingLevel.
    if (m_thinkingBudget.has_value()) {
        request_body["generationConfig"]["thinkingConfig"] = {
            {"thinkingLevel", m_thinkingBudget.value() > 0 ? "high" : "low"}
        };
    }

    std::string api_key = getApiKey();

    httplib::Headers headers = {
        { "Content-Type", "application/json" },
        { "x-goog-api-key", api_key }
    };

    std::string body_str = request_body.dump();

    const int max_connection_retries = 5;
    const int max_rate_limit_retries = 10;
    const int max_server_error_retries = 5;

    int connection_retry_count = 0;
    int rate_limit_retries = 0;
    int server_error_retries = 0;

    for (;;) {
        // Use persistent SSL client for connection reuse
        auto res = m_api_client->Post(path, headers, body_str, "application/json");

        // Connection error (no response): reinitialize and retry with backoff.
        if (!res) {
            std::string error_msg = httplib::to_string(res.error());
            mclog("Connection failed (attempt " + std::to_string(connection_retry_count + 1) +
                                  "/" + std::to_string(max_connection_retries) + "): " + error_msg + "\n");

            if (connection_retry_count >= max_connection_retries) {
                throw std::runtime_error("Connection failed after " +
                    std::to_string(max_connection_retries) + " retries: " + error_msg);
            }
            connection_retry_count++;
            init_api_client();
            int wait_seconds = std::min(2 * (1 << std::min(connection_retry_count - 1, 16)), 32);
            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        connection_retry_count = 0;

        if (res->status == 200) {
            return json::parse(res->body);
        }

        // Rate limiting (429)
        if (res->status == 429) {
            if (rate_limit_retries >= max_rate_limit_retries) {
                std::ostringstream err;
                err << "Exceeded maximum rate limit retries (" << max_rate_limit_retries << ")\n";
                mclog(err.str());
                throw std::runtime_error("Exceeded maximum rate limit retries");
            }
            rate_limit_retries++;

            int wait_seconds = 0;

            // Try to parse retry-after header with error handling
            if (res->has_header("retry-after")) {
                try {
                    wait_seconds = std::stoi(res->get_header_value("retry-after"));
                }
                catch (const std::exception& e) {
                    std::ostringstream err;
                    err << "Failed to parse retry-after header: " << e.what() << "\n";
                    mclog(err.str());
                    wait_seconds = 0;
                }
            }

            // Use exponential backoff if no valid retry-after header.
            if (wait_seconds <= 0) {
                wait_seconds = 5 * (1 << std::min(rate_limit_retries - 1, 16));
            }
            // Cap whatever we got (header or backoff) so a hostile/buggy
            // retry-after can't make us sleep indefinitely.
            wait_seconds = std::min(wait_seconds, 300);

            std::ostringstream log_msg;
            log_msg << "Rate limited (attempt " << rate_limit_retries << "/" << max_rate_limit_retries
                   << "). Waiting " << wait_seconds << " seconds before retry\n";
            mclog(log_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Server errors (5xx) - can be retried
        if (res->status >= 500 && res->status < 600) {
            if (server_error_retries >= max_server_error_retries) {
                std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
                std::ostringstream err;
                err << "Exceeded maximum server error retries (" << max_server_error_retries << ")\n";
                mclog(err.str());

                std::ostringstream exception_msg;
                exception_msg << "Server error (" << res->status << ") after "
                            << max_server_error_retries << " retries: " << error_detail;
                throw std::runtime_error(exception_msg.str());
            }
            server_error_retries++;

            // Exponential backoff: 5, 10, 20, 40, 80 seconds (capped at 120)
            // (shift clamped so it can't overflow if the retry limit is raised)
            int wait_seconds = std::min(5 * (1 << std::min(server_error_retries - 1, 16)), 120);

            std::string error_msg;
            switch (res->status) {
            case 500: error_msg = "Internal Server Error"; break;
            case 502: error_msg = "Bad Gateway"; break;
            case 503: error_msg = "Service Unavailable"; break;
            case 504: error_msg = "Gateway Timeout"; break;
            default: error_msg = "Server Error"; break;
            }

            std::ostringstream log_msg;
            log_msg << error_msg << " (" << res->status << ") - attempt "
                   << server_error_retries << "/" << max_server_error_retries
                   << ". Retrying in " << wait_seconds << " seconds\n";
            mclog(log_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Unknown / non-retryable status code
        std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
        mclog("Unexpected HTTP status " + std::to_string(res->status) + ": " + error_detail + "\n");
        throw std::runtime_error("Unexpected HTTP status " + std::to_string(res->status) + ": " + error_detail);
    }
}


// --- Chat Loop ---

/**
 * @brief Sends one user message and returns Gemini's text reply, running any
 *        tools the model calls along the way.
 */
std::string GeminiClient::chat(Context& context, const std::string& user_message) {

    m_tool_registry.clear();
    m_tools.clear();

    // Wrap all tools to bind context
    for (const auto& tool : context.tools) {
        auto executor = tool.executor;
        m_tool_registry[tool.name] = [executor, &context](const json& input) {
            return executor(context, input);
        };
        m_tools.push_back(tool);
    }

    mclog("Start " + user_message + " ===\n");

    // Use the pre-loaded tool definitions (convert to Gemini format)
    json tool_schemas = json::array();
    for (const auto& tool_spec : m_tools) {
        tool_schemas.push_back(tool_definition_to_json(tool_spec, ToolFormat::Gemini));
    }

    int iteration = 0;
    const int max_iterations = m_config->maxToolIterations();

    if (!m_api_client) init_api_client();

    // Add the user message to persistent history.
    if (!user_message.empty()) {
        m_conversation_history.push_back({
            {"role", "user"},
            {"parts", {
                {{"text", user_message}}
            }}
        });
    }

    ui::set_status("Thinking...", 0, max_iterations);
    json response = call_gemini("", tool_schemas, m_conversation_history);
    mclog("Gemini API response body: " + response.dump(2) + "\n");

    json candidate = response["candidates"][0];
    json model_response_message = {
        {"role", "model"},
        {"parts", candidate["content"]["parts"]}
    };
    m_conversation_history.push_back(model_response_message);

    std::string finish_reason = candidate.value("finishReason", "");
    if (finish_reason == "MALFORMED_FUNCTION_CALL") {
        std::string error_msg = "Gemini generated a malformed function call.";
        if (candidate.contains("finishMessage")) {
            error_msg += " Details: " + candidate["finishMessage"].get<std::string>();
        }
        throw std::runtime_error(error_msg);
    }

    // Tool loop: keep going while the model emits functionCall parts.
    while (true) {
        // Gather text and functionCall parts from the latest model message.
        bool has_text = false;
        json tool_calls = json::array();

        for (const auto& part : model_response_message["parts"]) {
            if (part.contains("functionCall")) {
                tool_calls.push_back(part["functionCall"]);
            }
        }

        // Surface text/reasoning to the user only when this turn also calls
        // tools. On the final-answer turn the caller prints the reply, so
        // emitting here would duplicate it.
        for (const auto& part : model_response_message["parts"]) {
            if (!part.contains("text")) continue;
            std::string text = part["text"].get<std::string>();
            bool is_thought = part.value("thought", false);
            if (is_thought) {
                mclog("assistant thinks: " + text + "\n");
            } else {
                has_text = true;
                mclog("assistant says: " + text + "\n");
            }
            if (!tool_calls.empty()) {
                ui::emit_intermediate(text, /*is_reasoning=*/is_thought, m_config->printCot());
            }
        }

        if (tool_calls.empty()) {
            // No more tool calls — the model has given its final answer.
            break;
        }
        if (!has_text) {
            mclog("WARNING: Model called tools without providing text explanation\n");
        }

        // Count an iteration only when tools actually run, so the budget matches
        // Claude/OpenAI (which count tool rounds, not the terminal text turn).
        if (iteration >= max_iterations) break;
        iteration++;
        mclog("=== Gemini Iteration " + std::to_string(iteration) + " ===\n");

        json tool_response_parts = json::array();

        for (const auto& func_call : tool_calls) {
            std::string tool_name = func_call["name"];
            json args = func_call.value("args", json::object());

            ui::set_status(getToolDisplayName(tool_name, args), iteration, max_iterations);
            mclog("Executing tool: " + tool_name + "\n");
            mclog("Input: " + args.dump(2) + "\n");

            std::string result;
            auto it = m_tool_registry.find(tool_name);
            if (it != m_tool_registry.end()) {
                try {
                    result = it->second(args);
                }
                catch (const std::exception& e) {
                    result = "ERROR: Tool execution failed: " + std::string(e.what());
                    mclog("Tool execution exception: " + std::string(e.what()) + "\n");
                }
            }
            else {
                result = formatUnknownToolError(tool_name, m_tool_registry);
                mclog("Unknown tool requested: " + tool_name + "\n");
            }

            ui::commit_status(); // lock the tool line into the scroll buffer

            // Format the tool result as a function_response part for Gemini
            tool_response_parts.push_back({
                {"functionResponse", {
                    {"name", tool_name},
                    {"response", {
                        {"content", result}
                    }}
                }}
            });

            // A functionResponse carries no image, so anything a tool produced
            // is appended to the same user turn as an inlineData part. Keeping
            // it in one turn avoids emitting two consecutive user roles, which
            // the alternation rules would otherwise have to tolerate.
            tapto::ToolImage image;
            if (tapto::takeToolImage(context, image)) {
                if (!image.label.empty()) {
                    tool_response_parts.push_back({{"text", image.label + ":"}});
                }
                tool_response_parts.push_back({
                    {"inline_data", {
                        {"mime_type", "image/png"},
                        {"data", tapto::base64Encode(image.png)}
                    }}
                });
                mclog("Attached a " + std::to_string(image.png.size()) +
                      " byte image to the tool response turn\n");
            }
        }

        // Send the tool results back. The Gemini REST API only accepts the
        // roles "user" and "model" in `contents`, so function responses go in
        // a "user" turn (a "function" role is rejected).
        m_conversation_history.push_back({
            {"role", "user"},
            {"parts", tool_response_parts}
        });

        // See claude.cpp: unbounded screenshot history ends in HTTP 413.
        if (m_config->keepRecentImages() > 0) {
            const size_t pruned = tapto::pruneHistoryImages(
                m_conversation_history, static_cast<size_t>(m_config->keepRecentImages()));
            if (pruned > 0) {
                mclog("Pruned " + std::to_string(pruned) + " older image(s) from history\n");
            }
        }

        ui::set_status("Thinking...", iteration, max_iterations);
        response = call_gemini("", tool_schemas, m_conversation_history);
        mclog("Gemini API response body: " + response.dump(2) + "\n");

        candidate = response["candidates"][0];
        model_response_message = {
            {"role", "model"},
            {"parts", candidate["content"]["parts"]}
        };
        m_conversation_history.push_back(model_response_message);

        finish_reason = candidate.value("finishReason", "");
        if (finish_reason == "MALFORMED_FUNCTION_CALL") {
            std::string error_msg = "Gemini generated a malformed function call at iteration " + std::to_string(iteration) + ".";
            if (candidate.contains("finishMessage")) {
                error_msg += " Details: " + candidate["finishMessage"].get<std::string>();
            }
            throw std::runtime_error(error_msg);
        }
    }

    if (iteration >= max_iterations) {
        mclog("Warning: Reached maximum iterations\n");
    }

    // If we stopped with unanswered function calls (iteration cap), answer them
    // so the conversation stays valid on the next turn.
    {
        json pending = json::array();
        for (const auto& part : model_response_message["parts"]) {
            if (part.contains("functionCall")) {
                pending.push_back({
                    {"functionResponse", {
                        {"name", part["functionCall"].value("name", std::string())},
                        {"response", {{"content", "Tool not run: iteration limit reached."}}}
                    }}
                });
            }
        }
        if (!pending.empty()) {
            m_conversation_history.push_back({{"role", "user"}, {"parts", pending}});
        }
    }

    mclog("=== Final Response ===" + finish_reason + "\n");

    ui::end_status();

    // Collect the assistant's final text reply.
    std::string reply;
    for (const auto& part : model_response_message["parts"]) {
        if (part.contains("text") && !part.value("thought", false)) {
            reply += part["text"].get<std::string>();
        }
    }
    if (finish_reason == "MAX_TOKENS") {
        reply += "\n[truncated: hit max output tokens - raise max-output-tokens]";
    }
    return reply;
}

/// <summary>Starts a new conversation by clearing the conversation history.</summary>
void GeminiClient::start() {
    m_conversation_history = json::array();
    mclog("Started new conversation (history cleared)\n");
}

/// <summary>Checks if conversation history exists.</summary>
bool GeminiClient::hasHistory() const {
    return !m_conversation_history.empty();
}

/// <summary>Loads conversation history from stored format.</summary>
void GeminiClient::loadHistory(const json& history) {
    if (!history.is_array()) {
        mclog("ERROR: Invalid history format - expected array\n");
        return;
    }

    m_conversation_history = history;
    mclog("Loaded conversation history with " + std::to_string(history.size()) + " messages\n");
}

/// <summary>Gets the current conversation history.</summary>
json GeminiClient::getHistory() const {
    return m_conversation_history;
}

/// <summary>Sets the model for Gemini client.</summary>
void GeminiClient::setModel(const std::string& model) {
    m_model = model;
    mclog("GeminiClient model updated to: " + model + "\n");
}

/// <summary>Sets the host URL for Gemini client and resets the API client.</summary>
void GeminiClient::setHost(const std::string& host) {
    m_url = host;
    m_api_client.reset();  // Reset client to force re-initialization with new URL
    mclog("GeminiClient host updated to: " + host + " (API client reset)\n");
}

/// <summary>Sets the API key for Gemini client.</summary>
void GeminiClient::setApiKeyRef(const std::string& apiKey) {
    m_apiKeyRef = apiKey;
    mclog("GeminiClient API key updated\n");
}
