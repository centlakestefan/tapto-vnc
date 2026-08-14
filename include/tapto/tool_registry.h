// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <functional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

class Context;

// A tool executor receives the run context and the model-supplied JSON input
// and returns a string result that is fed back to the model.
using ToolExecutorFn = std::function<std::string(Context&, const nlohmann::json&)>;

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

}  // namespace tapto

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
};

// Wire formats for tool/function declarations across providers.
enum class ToolFormat { Claude, OpenAI, Gemini, Generic };

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

// Human-friendly label for a tool invocation, shown on the status line while
// the tool is running and committed to the scroll buffer afterwards.
//
// The point is that a human watching the run can tell what the model is doing
// to their screen without reading raw JSON:
//
//   vnc_screenshot            -> "Screenshot"
//   vnc_click                 -> "Click left (412,300)"
//   vnc_zoom                  -> "Zoom at (150,225)"
//   vnc_move                  -> "Move to (412,300)"
//   vnc_drag                  -> "Drag (10,20) -> (90,120)"
//   vnc_scroll                -> "Scroll down x3 at (400,500)"
//   vnc_type                  -> "Type \"hello world\""
//   vnc_key                   -> "Key ctrl+alt+delete"
//   vnc_wait                  -> "Wait 1500ms"
//   Unknown tool              -> raw tool name
inline std::string getToolDisplayName(const std::string& tool_name, const nlohmann::json& input) {
    auto str = [&](const char* field, const std::string& fallback = "") -> std::string {
        if (input.is_object() && input.contains(field) && input[field].is_string()) {
            return input[field].get<std::string>();
        }
        return fallback;
    };
    auto num = [&](const char* field, int fallback = 0) -> int {
        if (input.is_object() && input.contains(field) && input[field].is_number()) {
            return input[field].get<int>();
        }
        return fallback;
    };
    // Coordinates for display. Deliberately more forgiving than num() above and
    // deliberately distinguishes "absent or unreadable" from a real zero:
    // models sometimes send coordinates as strings, which the executors accept
    // via intField(), so a status line that silently printed 0 for those
    // disagreed with the click that actually happened. "?" says which case it
    // is instead of inventing a plausible number.
    auto coord = [&](const char* field) -> std::string {
        if (!input.is_object() || !input.contains(field)) return "?";
        const nlohmann::json& value = input[field];
        if (value.is_number_integer()) return std::to_string(value.get<int>());
        if (value.is_number_float())   return std::to_string(static_cast<int>(value.get<double>()));
        if (value.is_string()) {
            try { return std::to_string(std::stoi(value.get<std::string>())); }
            catch (...) { return "?"; }
        }
        return "?";
    };
    auto point = [&](const char* xf, const char* yf) -> std::string {
        return "(" + coord(xf) + "," + coord(yf) + ")";
    };

    if (tool_name == "vnc_screenshot") return "Screenshot";

    if (tool_name == "vnc_click") {
        const int clicks = num("clicks", 1);
        std::string label = "Click " + str("button", "left");
        if (clicks > 1) label += " x" + std::to_string(clicks);
        return label + " " + point("x", "y");
    }

    // The region size and magnification are fixed, so there is nothing to show
    // but where the model looked.
    if (tool_name == "vnc_zoom") return "Zoom at " + point("x", "y");

    if (tool_name == "vnc_move") return "Move to " + point("x", "y");

    if (tool_name == "vnc_drag") {
        return "Drag " + point("from_x", "from_y") + " -> " + point("to_x", "to_y");
    }

    if (tool_name == "vnc_scroll") {
        return "Scroll " + str("direction", "down") + " x" + std::to_string(num("amount", 3)) +
               " at " + point("x", "y");
    }

    if (tool_name == "vnc_type") {
        std::string text = str("text");
        // Keep the status line to one row: long text is elided, and newlines
        // would otherwise break the in-place rewrite.
        for (char& c : text) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        }
        constexpr size_t kMaxShown = 40;
        if (text.size() > kMaxShown) text = text.substr(0, kMaxShown) + "...";
        return "Type \"" + text + "\"";
    }

    if (tool_name == "vnc_key")  return "Key " + str("keys", "?");
    if (tool_name == "vnc_wait") return "Wait " + std::to_string(num("ms", 1000)) + "ms";

    // Unknown / future tool: return the raw name so nothing is lost.
    return tool_name;
}
