// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/openai.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <regex>
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

namespace {

/// <summary>Field names carrying chain-of-thought on an assistant message.
/// There is no standard: vLLM, llama.cpp and sglang with the deepseek/qwen
/// templates use `reasoning_content`, while some gateways and newer builds use
/// a bare `reasoning`. Both are checked, in that order.</summary>
const char* const kReasoningFields[] = {"reasoning_content", "reasoning"};

/// <summary>Returns the assistant message's chain-of-thought as a string when
/// present and non-empty, otherwise an empty string.</summary>
std::string extractReasoning(const json& message) {
    for (const char* field : kReasoningFields) {
        if (!message.contains(field) || message[field].is_null()) continue;
        if (!message[field].is_string()) continue;
        const std::string value = message[field].get<std::string>();
        if (!value.empty()) return value;
    }
    return "";
}

/// <summary>Strip Harmony / channel-style special tokens from user-visible
/// content. When the inference server runs with `--reasoning-format none` (or
/// without a matching extractor for the model's chat template), tokens such
/// as `<|channel|>analysis<|message|>...<|end|>` leak into `content`
/// instead of being routed to `reasoning_content`. This is a defensive scrub;
/// the server-side extractor remains the correct fix when it works.</summary>
std::string stripChannelTokens(std::string text) {
    if (text.empty()) return text;

    // Drop reasoning channels entirely (analysis/thought blocks), up to the
    // next <|end|>, the next channel opening, or end of string. \|? makes the
    // pipe optional so we tolerate mangled variants like <channel>thought<message>.
    static const std::regex reasoningBlock(
        R"(<\|?channel\|?>\s*(?:analysis|thought)\s*<\|?message\|?>[\s\S]*?(?=<\|?end\|?>|<\|?channel\|?>|$))",
        std::regex::ECMAScript | std::regex::icase
    );
    text = std::regex_replace(text, reasoningBlock, "");

    // Strip stray channel/message/end and start-of-message markers that
    // remain (e.g. the wrappers around the `final` channel content).
    static const std::regex strayTokens(
        R"(<\|?(?:channel|message|end|start|im_start|im_end)\|?>)",
        std::regex::ECMAScript | std::regex::icase
    );
    text = std::regex_replace(text, strayTokens, "");

    // Trim leading/trailing whitespace left behind by the strips.
    auto first = text.find_first_not_of(" \t\r\n");
    auto last  = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, last - first + 1);
}

/// <summary>Returns the user-visible `content` of an assistant message, or an
/// empty string when absent/null/non-string. Strips Harmony channel tokens
/// (see stripChannelTokens) so callers don't have to.</summary>
std::string extractContent(const json& message) {
    if (!message.contains("content") || message["content"].is_null()) {
        return "";
    }
    if (!message["content"].is_string()) {
        return "";
    }
    return stripChannelTokens(message["content"].get<std::string>());
}

/// <summary>Strip per-turn-only fields from an assistant message before
/// echoing it back to the server in conversation history. `reasoning_content`
/// is chain-of-thought scratch that some providers (DeepSeek's hosted API)
/// reject when sent back, and all providers waste tokens reading it.</summary>
json historyMessage(json message) {
    for (const char* field : kReasoningFields) {
        message.erase(field);
    }
    return message;
}

} // namespace

OpenAIClient::OpenAIClient(const AiConfig* config, const std::string& url, const std::string& model, const std::string& apiKey)
    : m_config(config), m_model(model), m_apiKeyRef(apiKey) {
    m_url = url;

    mclog("OpenAIClient initialized with url=" + m_url + "\n");
}

/// <summary>Initializes the persistent client with proper timeout and keep-alive settings.</summary>
void OpenAIClient::init_api_client() {
    mclog("Initializing API client connection to " + m_url + "\n");

    m_api_client = std::make_unique<httplib::Client>(m_url);
    // Certificate verification only applies to https endpoints; skip it for plain http.
    if (m_url.rfind("https://", 0) == 0) {
        m_api_client->enable_server_certificate_verification(true);
    }

    m_api_client->set_connection_timeout(m_config->openaiConnectionTimeoutSeconds());
    m_api_client->set_read_timeout(m_config->openaiReadTimeoutSeconds());
    m_api_client->set_keep_alive(true);

    mclog("API client initialized with keep-alive enabled\n");
}

/// <summary>Returns the configured OpenAI API key.</summary>
std::string OpenAIClient::getApiKey() {
    if (!m_apiKeyRef.empty()) {
        return m_apiKeyRef;
    }
    throw std::runtime_error("OpenAI API key is not set");
}

/// <summary>Makes API call to OpenAI with retry logic.</summary>
json OpenAIClient::call_openai(const std::string& user_message, const json& tools, const json& conversation_history) {
    // Build messages array
    json messages = json::array();

    // Add system message first (OpenAI puts system as first message)
    if (!m_systemPrompt.empty()) {
        messages.push_back({
            {"role", "system"},
            {"content", m_systemPrompt}
        });
    }

    // Add conversation history
    for (const auto& msg : conversation_history) {
        messages.push_back(msg);
    }

    // Add new user message if provided
    if (!user_message.empty()) {
        messages.push_back({
            {"role", "user"},
            {"content", user_message}
        });
    }

    // Build request body
    json request_body = {
        {"model", m_model},
        {"max_tokens", m_config->maxOutputTokens()},
        {"messages", messages}
    };

    // Add tools if provided
    if (!tools.empty()) {
        request_body["tools"] = tools;
        request_body["tool_choice"] = "auto";
    }

    // Provider-level extended-thinking override. OpenAI-compatible servers
    // that support a thinking toggle (Qwen3, GLM-4.5, etc.) read it from
    // chat_template_kwargs.enable_thinking. The toggle is boolean — the
    // numeric budget has no place in this protocol, so any value > 0 just
    // means "on".
    if (m_thinkingBudget.has_value()) {
        request_body["chat_template_kwargs"] = {
            {"enable_thinking", m_thinkingBudget.value() > 0}
        };
    }

    std::string api_key = getApiKey();

    // Set headers
    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key},
        {"Content-Type", "application/json"}
    };

    int connection_retry_count = 0;
    int rate_limit_retries = 0;
    int server_error_retries = 0;

    for (;;) {
        auto res = m_api_client->Post("/v1/chat/completions", headers, request_body.dump(), "application/json");

        // Handle connection errors
        if (!res) {
            auto error = res.error();
            std::string error_msg = httplib::to_string(error);

            std::ostringstream log_msg;
            log_msg << "Connection failed (attempt " << (connection_retry_count + 1)
                   << "/" << MAX_CONNECTION_RETRIES << "): " << error_msg << "\n";
            mclog(log_msg.str());

            // Check if we should retry
            if (connection_retry_count >= MAX_CONNECTION_RETRIES) {
                mclog("Exceeded maximum connection retries\n");
                std::ostringstream exception_msg;
                exception_msg << "Connection failed after " << MAX_CONNECTION_RETRIES
                            << " retries: " << error_msg;
                throw std::runtime_error(exception_msg.str());
            }

            connection_retry_count++;

            // Reinitialize connection on connection failure
            mclog("Reinitializing API client connection...\n");
            init_api_client();

            // Exponential backoff for connection retries: 2, 4, 8, 16, 32 seconds
            // (shift clamped so it can't overflow if the retry limit is raised)
            int wait_seconds = std::min(CONNECTION_BACKOFF_BASE * (1 << std::min(connection_retry_count - 1, 16)), CONNECTION_BACKOFF_MAX);

            std::ostringstream retry_msg;
            retry_msg << "Retrying in " << wait_seconds << " seconds...\n";
            mclog(retry_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Reset connection retry counter on successful connection
        connection_retry_count = 0;

        // Handle HTTP status codes
        int status = res->status;

        // Success
        if (status == 200) {
            return json::parse(res->body);
        }

        // Rate limiting (429)
        if (status == 429) {
            if (rate_limit_retries >= MAX_RATE_LIMIT_RETRIES) {
                std::ostringstream err;
                err << "Exceeded maximum rate limit retries (" << MAX_RATE_LIMIT_RETRIES << ")\n";
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
                wait_seconds = RATE_LIMIT_BACKOFF_BASE * (1 << std::min(rate_limit_retries - 1, 16));
            }
            // Cap whatever we got (header or backoff) so a hostile/buggy
            // retry-after can't make us sleep indefinitely.
            wait_seconds = std::min(wait_seconds, RATE_LIMIT_BACKOFF_MAX);

            std::ostringstream log_msg;
            log_msg << "Rate limited (attempt " << rate_limit_retries << "/" << MAX_RATE_LIMIT_RETRIES
                   << "). Waiting " << wait_seconds << " seconds before retry\n";
            mclog(log_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Client errors (4xx) - generally not retryable
        if (status >= 400 && status < 500) {
            std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
            std::ostringstream log_msg;
            log_msg << "Client error " << status << ": " << error_detail << "\n";
            mclog(log_msg.str());

            std::ostringstream exception_msg;
            switch (status) {
            case 400:
                exception_msg << "Bad Request (400): Invalid request format or parameters - " << error_detail;
                // A per-prompt image cap is a server-side limit with a
                // client-side remedy, and the server's wording does not say so.
                if (error_detail.find("image") != std::string::npos &&
                    (error_detail.find("At most") != std::string::npos ||
                     error_detail.find("limit") != std::string::npos)) {
                    exception_msg << "\nThe server caps how many images one prompt may carry. Lower "
                                     "'keep-recent-images' in the tapto config to at or below that "
                                     "number (it is currently "
                                  << m_config->keepRecentImages()
                                  << "), or restart the server with a higher "
                                     "--limit-mm-per-prompt image=N.";
                }
                break;
            case 401:
                exception_msg << "Unauthorized (401): Invalid or missing API key - " << error_detail;
                break;
            case 403:
                exception_msg << "Forbidden (403): Access denied - " << error_detail;
                break;
            case 404:
                exception_msg << "Not Found (404): Endpoint not found - " << error_detail;
                break;
            default:
                exception_msg << "Client error (" << status << "): " << error_detail;
                break;
            }
            throw std::runtime_error(exception_msg.str());
        }

        // Server errors (5xx) - can be retried
        if (status >= 500 && status < 600) {
            // ...unless the body says the request itself is the problem.
            //
            // OpenAI-compatible servers do not all classify errors the same
            // way: llama.cpp answers "image input is not supported" with a 500
            // even though it is a permanent property of the loaded model, not a
            // transient fault. Retrying that five times with backoff wastes
            // over a minute and buries the one line that explains the failure.
            {
                std::string lowered = res->body;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                static const char* kPermanent[] = {
                    "not supported", "unsupported", "invalid_request", "does not support",
                };
                for (const char* marker : kPermanent) {
                    if (lowered.find(marker) != std::string::npos) {
                        std::string detail = res->body.length() > 500
                                                 ? res->body.substr(0, 500) + "..." : res->body;
                        mclog("Permanent server error, not retrying: " + detail + "\n");
                        throw std::runtime_error(
                            "Server rejected the request (" + std::to_string(status) +
                            "), and the error is not transient so it was not retried: " + detail);
                    }
                }
            }

            if (server_error_retries >= MAX_SERVER_ERROR_RETRIES) {
                std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
                std::ostringstream err;
                err << "Exceeded maximum server error retries (" << MAX_SERVER_ERROR_RETRIES << ")\n";
                mclog(err.str());

                std::ostringstream exception_msg;
                exception_msg << "Server error (" << status << ") after "
                            << MAX_SERVER_ERROR_RETRIES << " retries: " << error_detail;
                throw std::runtime_error(exception_msg.str());
            }
            server_error_retries++;

            int wait_seconds = 0;

            // For 503 Service Unavailable, respect retry-after header
            if (status == 503 && res->has_header("retry-after")) {
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

            // Use exponential backoff if no retry-after header.
            if (wait_seconds <= 0) {
                wait_seconds = SERVER_ERROR_BACKOFF_BASE * (1 << std::min(server_error_retries - 1, 16));
            }
            // Cap whatever we got (header or backoff) against a hostile retry-after.
            wait_seconds = std::min(wait_seconds, SERVER_ERROR_BACKOFF_MAX);

            std::string error_msg;
            switch (status) {
            case 500: error_msg = "Internal Server Error"; break;
            case 502: error_msg = "Bad Gateway"; break;
            case 503: error_msg = "Service Unavailable"; break;
            case 504: error_msg = "Gateway Timeout"; break;
            default: error_msg = "Server Error"; break;
            }

            std::ostringstream log_msg;
            log_msg << error_msg << " (" << status << ") - attempt "
                   << server_error_retries << "/" << MAX_SERVER_ERROR_RETRIES
                   << ". Retrying in " << wait_seconds << " seconds\n";
            mclog(log_msg.str());

            std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
            continue;
        }

        // Unknown status code
        std::string error_detail = res->body.length() > 500 ? res->body.substr(0, 500) + "..." : res->body;
        mclog("Unexpected HTTP status " + std::to_string(status) + ": " + error_detail + "\n");
        throw std::runtime_error("Unexpected HTTP status " + std::to_string(status) + ": " + error_detail);
    }
}

/// <summary>Sends one user message and returns OpenAI's text reply, running any
/// tools the model calls along the way.</summary>
std::string OpenAIClient::chat(Context& context, const std::string& user_message) {

    m_tool_registry.clear();
    m_tools.clear();

    // Wrap all tools to bind the context
    for (const auto& tool : context.tools) {
        auto executor = tool.executor;
        m_tool_registry[tool.name] = [executor, &context](const json& input) {
            return executor(context, input);
        };
        m_tools.push_back(tool);
    }

    mclog("Start " + user_message + " ===\n");

    // Convert tools to OpenAI format
    json tools = json::array();
    for (const auto& tool_spec : m_tools) {
        json tool_def = tool_definition_to_json(tool_spec, ToolFormat::OpenAI);

        // OpenAI expects tools in a specific format with "type": "function"
        json openai_tool = {
            {"type", "function"},
            {"function", tool_def}
        };

        tools.push_back(openai_tool);
    }

    int iteration = 0;
    const int max_iterations = m_config->maxToolIterations();

    if (!m_api_client) init_api_client();

    // Add the user message to persistent history.
    if (!user_message.empty()) {
        m_conversation_history.push_back({
            {"role", "user"},
            {"content", user_message}
        });
    }

    ui::set_status("Thinking...", 0, max_iterations);
    json response = call_openai("", tools, m_conversation_history);
    if (!response.contains("choices") || response["choices"].empty()) {
        throw std::runtime_error("Invalid OpenAI response: " + response.dump());
    }

    auto choice = response["choices"][0];
    auto message = choice["message"];
    m_conversation_history.push_back(historyMessage(message));

    // Tool loop: keep going while the model requests tools. Terminates as soon
    // as the model replies without tool_calls.
    while (message.contains("tool_calls") && !message["tool_calls"].empty() && iteration < max_iterations) {
        iteration++;
        mclog("=== Iteration " + std::to_string(iteration) + " ===\n");

        {
            std::string cot = extractReasoning(message);
            if (!cot.empty()) {
                mclog("Assistant CoT: " + cot + "\n");
                ui::emit_intermediate(cot, /*is_reasoning=*/true, m_config->printCot());
            }
        }
        {
            std::string text = extractContent(message);
            if (!text.empty()) {
                mclog("Assistant text: " + text + "\n");
                ui::emit_intermediate(text, /*is_reasoning=*/false, m_config->printCot());
            }
        }

        // Images produced by tools this round. They cannot travel inside the
        // tool results: an OpenAI `role: "tool"` message carries text only.
        // They also cannot be interleaved between tool results, because every
        // tool_call in the assistant message must be answered before any other
        // message appears. So they are collected here and delivered as one
        // user turn once the whole round is answered.
        std::vector<tapto::ToolImage> pending_images;

        // Process each tool call
        for (const auto& tool_call : message["tool_calls"]) {
            std::string tool_id = tool_call["id"];
            std::string tool_name = tool_call["function"]["name"];

            json tool_input;
            std::string result;
            bool args_parsed = false;
            try {
                tool_input = json::parse(tool_call["function"]["arguments"].get<std::string>());
                args_parsed = true;
            }
            catch (const std::exception& e) {
                std::string raw_args;
                try {
                    raw_args = tool_call["function"]["arguments"].get<std::string>();
                }
                catch (...) {
                    raw_args = tool_call["function"]["arguments"].dump();
                }
                result = "ERROR: Tool call arguments are not valid JSON: " + std::string(e.what()) +
                        "\nReceived arguments: " + raw_args +
                        "\nPlease retry this tool call with a valid JSON object as the arguments.";
                mclog("Tool argument parse failure for '" + tool_name + "': " + e.what() + "\n");
            }

            if (!args_parsed) {
                m_conversation_history.push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_id},
                    {"content", result}
                });
                continue;
            }

            ui::set_status(getToolDisplayName(tool_name, tool_input), iteration, max_iterations);
            mclog("Executing tool: " + tool_name + "\n");
            mclog("Input: " + tool_input.dump(2) + "\n");

            auto it = m_tool_registry.find(tool_name);
            if (it != m_tool_registry.end()) {
                try {
                    result = it->second(tool_input);
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

            tapto::ToolImage image;
            if (tapto::takeToolImage(context, image)) {
                pending_images.push_back(std::move(image));
            }

            m_conversation_history.push_back({
                {"role", "tool"},
                {"tool_call_id", tool_id},
                {"content", result}
            });
        }

        // Every tool_call is now answered, so it is safe to add the images.
        if (!pending_images.empty()) {
            json parts = json::array();
            for (const auto& image : pending_images) {
                if (!image.label.empty()) {
                    parts.push_back({{"type", "text"}, {"text", image.label + ":"}});
                }
                parts.push_back({
                    {"type", "image_url"},
                    {"image_url", {{"url", "data:image/png;base64," +
                                           tapto::base64Encode(image.png)}}}
                });
            }
            m_conversation_history.push_back({{"role", "user"}, {"content", parts}});
            mclog("Attached " + std::to_string(pending_images.size()) +
                  " image(s) as a user turn\n");
        }

        // See claude.cpp: unbounded screenshot history ends in HTTP 413.
        if (m_config->keepRecentImages() > 0) {
            const size_t pruned = tapto::pruneHistoryImages(
                m_conversation_history, static_cast<size_t>(m_config->keepRecentImages()));
            if (pruned > 0) {
                mclog("Pruned " + std::to_string(pruned) + " older image(s) from history\n");
            }
        }

        ui::set_status("Thinking...", iteration, max_iterations);
        response = call_openai("", tools, m_conversation_history);
        if (!response.contains("choices") || response["choices"].empty()) {
            throw std::runtime_error("Invalid OpenAI response: " + response.dump());
        }

        choice = response["choices"][0];
        message = choice["message"];
        m_conversation_history.push_back(historyMessage(message));
    }

    if (iteration >= max_iterations) {
        mclog("Warning: Reached maximum iterations\n");
    }

    // If we stopped with unanswered tool calls (iteration cap), answer them so
    // the conversation stays valid on the next turn.
    if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
        for (const auto& tc : message["tool_calls"]) {
            m_conversation_history.push_back({
                {"role", "tool"},
                {"tool_call_id", tc.value("id", std::string())},
                {"content", "Tool not run: iteration limit reached."}
            });
        }
    }

    // The closing message never passes through the loop above, because the loop
    // exits precisely when a message arrives without tool calls — so the one
    // thing missing from the trace was the model's summary of what it had just
    // done, which is the part a human reads first. Logged here rather than at
    // the top of the loop so it also covers a turn stopped by the iteration
    // cap, whose message never reached the loop body either.
    {
        const std::string cot = extractReasoning(message);
        if (!cot.empty()) mclog("Assistant CoT: " + cot + "\n");
        const std::string text = extractContent(message);
        if (!text.empty()) mclog("Assistant text: " + text + "\n");
    }

    mclog("=== Final Response === " + choice.value("finish_reason", "unknown") + "\n");

    ui::end_status();

    // Collect the assistant's final text reply (falling back to reasoning
    // content for reasoning models that emit only that).
    std::string reply = extractContent(message);
    if (reply.empty()) {
        reply = extractReasoning(message);
    }
    if (choice.value("finish_reason", std::string()) == "length") {
        reply += "\n[truncated: hit max output tokens - raise max-output-tokens]";
    }
    return reply;
}

/// <summary>Starts a new conversation by clearing the conversation history.</summary>
void OpenAIClient::start() {
    m_conversation_history = json::array();
    mclog("Started new conversation (history cleared)\n");
}

/// <summary>Checks if conversation history exists.</summary>
bool OpenAIClient::hasHistory() const {
    return !m_conversation_history.empty();
}

/// <summary>Loads conversation history from stored format.</summary>
void OpenAIClient::loadHistory(const json& history) {
    if (!history.is_array()) {
        mclog("ERROR: Invalid history format - expected array\n");
        return;
    }

    m_conversation_history = history;
    mclog("Loaded conversation history with " + std::to_string(history.size()) + " messages\n");
}

/// <summary>Gets the current conversation history.</summary>
json OpenAIClient::getHistory() const {
    return m_conversation_history;
}

/// <summary>Sets the model for OpenAI client.</summary>
void OpenAIClient::setModel(const std::string& model) {
    m_model = model;
    mclog("OpenAIClient model updated to: " + model + "\n");
}

/// <summary>Sets the host URL for OpenAI client and resets the API client.</summary>
void OpenAIClient::setHost(const std::string& host) {
    m_url = host;
    m_api_client.reset();  // Reset client to force re-initialization with new URL
    mclog("OpenAIClient host updated to: " + host + " (API client reset)\n");
}

/// <summary>Sets the API key for OpenAI client.</summary>
void OpenAIClient::setApiKeyRef(const std::string& apiKey) {
    m_apiKeyRef = apiKey;
    mclog("OpenAIClient API key updated\n");
}
