// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Context;

// A tool executor receives the run context and the model-supplied JSON input
// and returns a string result that is fed back to the model.
using ToolExecutorFn = std::function<std::string(Context&, const nlohmann::json&)>;

// Renders a tool invocation as the short label shown on the status line while
// it runs, from the model-supplied input. Optional; see ToolSpec::display.
using ToolDisplayFn = std::function<std::string(const nlohmann::json&)>;

namespace tapto {

// Thrown by an executor when whatever the tools drive is gone for good, so
// every later call in this turn would fail identically.
//
// The agent loop turns an ordinary tool exception into a tool result and hands
// it back to the model, which is right for a bad argument and wrong for this:
// the model cannot repair a dead connection, so it retries, and each retry
// costs an API round trip. One observed run spent five iterations asking for
// screenshots of a machine it was no longer attached to, then reasoned at
// length about whether its last click had registered. A provider must let this
// one propagate rather than answer it.
struct ConnectionLost : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace tapto

// Declarative description of a tool the model may call.
struct ToolSpec {
    std::string name;
    std::string description;
    nlohmann::json parameters = nlohmann::json::object(); // JSON Schema for the args
    ToolExecutorFn executor;

    // When non-empty, this tool maps to a Claude server-side built-in tool of
    // this type (e.g. "text_editor_20250728"). For ToolFormat::Claude it is
    // declared as {type, name} with no schema (Claude knows the schema); other
    // providers fall back to the explicit `parameters` schema below.
    std::string claude_builtin_type;

    // Human-friendly label for an invocation, shown on the status line while
    // the tool runs and committed to the transcript afterwards -- "Edit
    // src/foo.cpp" rather than the tool name and a JSON blob. Unset means the
    // raw tool name is shown. Each program knows its own tools, so this is a
    // hook per tool and not a table in the library.
    ToolDisplayFn display;
};

// Wire formats for tool/function declarations across providers, plus MCP --
// which is not a provider but is one more spelling of the same three fields,
// and belongs with them rather than in the server that happens to speak it.
enum class ToolFormat { Claude, OpenAI, Gemini, Generic, Mcp };

// Render a ToolSpec into the JSON shape a given provider expects. (The OpenAI
// caller wraps this in {"type":"function","function":{...}} itself.)
inline nlohmann::json tool_definition_to_json(const ToolSpec& spec, ToolFormat format) {
    using nlohmann::json;

    // Claude built-in tools are declared by type + name only; the schema is
    // built into the model and must not be sent.
    if (format == ToolFormat::Claude && !spec.claude_builtin_type.empty()) {
        return json{
            {"type", spec.claude_builtin_type},
            {"name", spec.name},
        };
    }

    switch (format) {
        case ToolFormat::Claude:
        case ToolFormat::Generic:
            return json{
                {"name", spec.name},
                {"description", spec.description},
                {"input_schema", spec.parameters},
            };
        case ToolFormat::OpenAI:
        case ToolFormat::Gemini:
            return json{
                {"name", spec.name},
                {"description", spec.description},
                {"parameters", spec.parameters},
            };
        // Same three fields as Claude, one letter apart: MCP spells the schema
        // key inputSchema, not input_schema. A tool list built with the wrong
        // one is accepted and then called with no arguments at all, so the
        // difference is worth a case of its own.
        case ToolFormat::Mcp:
            return json{
                {"name", spec.name},
                {"description", spec.description},
                {"inputSchema", spec.parameters},
            };
    }
    return json::object();
}

// Error string returned to the model when it calls an unknown tool. Templated
// on the registry type so it works with the clients' internal registry maps.
template <class Registry>
inline std::string formatUnknownToolError(const std::string& tool_name, const Registry& registry) {
    std::string msg = "ERROR: Unknown tool '" + tool_name + "'. Available tools:";
    if (registry.empty()) {
        msg += " (none)";
    } else {
        for (const auto& entry : registry) {
            msg += " " + entry.first;
        }
    }
    return msg;
}

// The status-line label for a tool call: the tool's own display hook when it
// has one, otherwise the raw tool name so nothing is lost. The backends call
// this with the tool table they were handed.
inline std::string getToolDisplayName(const std::vector<ToolSpec>& tools,
                                      const std::string& tool_name,
                                      const nlohmann::json& input) {
    for (const auto& spec : tools) {
        if (spec.name != tool_name) continue;
        if (!spec.display) break;
        try {
            return spec.display(input);
        } catch (...) {
            break; // a label is decoration; never let it fail the call
        }
    }
    return tool_name;
}
