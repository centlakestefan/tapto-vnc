// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// The status-line labels of the screen-control tools, reached the way the
// backends and the MCP server reach them: through getToolDisplayName() and
// each ToolSpec's `display` hook. A missing hook shows the raw tool name and
// fails nothing, so this is the only place a regression would surface. Plain
// assertions; ctest runs the binary.

#include "tapto/computer_tools.h"
#include "tapto/tool_registry.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

int g_checks = 0;
int g_failures = 0;

void check_eq(int line, const std::string& actual, const std::string& expected) {
    ++g_checks;
    if (actual == expected) return;
    ++g_failures;
    std::cout << "FAIL (line " << line << ")\n"
              << "  expected: " << expected << "\n"
              << "  actual:   " << actual << "\n";
}

#define CHECK_EQ(actual, expected) check_eq(__LINE__, (actual), (expected))

}  // namespace

int main() {
    const std::vector<ToolSpec> tools = tapto::makeComputerTools();
    auto label = [&](const char* tool, const json& in) {
        return getToolDisplayName(tools, tool, in);
    };

    CHECK_EQ(label("vnc_screenshot", json::object()), "Screenshot");
    CHECK_EQ(label("vnc_click", json{{"x", 412}, {"y", 300}}), "Click left (412,300)");
    CHECK_EQ(label("vnc_click", json{{"x", 412}, {"y", 300}, {"button", "right"}, {"clicks", 2}}),
             "Click right x2 (412,300)");
    // Coordinates sent as strings are accepted by the executors, so the label
    // must agree with the click that happens; unreadable ones say so.
    CHECK_EQ(label("vnc_click", json{{"x", "10"}, {"y", "20"}}), "Click left (10,20)");
    CHECK_EQ(label("vnc_click", json{{"x", "ten"}}), "Click left (?,?)");
    CHECK_EQ(label("vnc_zoom", json{{"x", 150}, {"y", 225}}), "Zoom at (150,225)");
    CHECK_EQ(label("vnc_move", json{{"x", 412}, {"y", 300}}), "Move to (412,300)");
    CHECK_EQ(label("vnc_drag", json{{"from_x", 10}, {"from_y", 20}, {"to_x", 90}, {"to_y", 120}}),
             "Drag (10,20) -> (90,120)");
    CHECK_EQ(label("vnc_scroll", json{{"x", 400}, {"y", 500}}), "Scroll down x3 at (400,500)");
    CHECK_EQ(label("vnc_scroll", json{{"x", 400}, {"y", 500}, {"direction", "up"}, {"amount", 5}}),
             "Scroll up x5 at (400,500)");
    CHECK_EQ(label("vnc_type", json{{"text", "hello world"}}), "Type \"hello world\"");
    CHECK_EQ(label("vnc_type", json{{"text", "a\nb\tc"}}), "Type \"a b c\"");
    CHECK_EQ(label("vnc_type", json{{"text", std::string(50, 'x')}}),
             "Type \"" + std::string(40, 'x') + "...\"");
    CHECK_EQ(label("vnc_key", json{{"keys", "ctrl+alt+delete"}}), "Key ctrl+alt+delete");
    CHECK_EQ(label("vnc_wait", json{{"ms", 1500}}), "Wait 1500ms");
    CHECK_EQ(label("vnc_wait", json::object()), "Wait 1000ms");
    CHECK_EQ(label("no_such_tool", json::object()), "no_such_tool");

    std::cout << (g_failures ? "FAILED " : "ok ") << (g_checks - g_failures) << "/" << g_checks
              << " checks\n";
    return g_failures ? 1 : 0;
}
