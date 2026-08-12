// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "context.h"
#include "tool_image.h"
#include "tool_registry.h"

namespace tapto {

class VncSession;

namespace keys {
// The session the tools drive. Stored as a bare VncSession* because the
// caller owns its lifetime for the whole run.
constexpr const char* kSession = "vnc.session";

// Optional directory (std::string). When set, every screenshot a tool takes is
// also written there, numbered in order. The model only ever sees the newest
// few images — older ones are pruned out of the transcript — so this is the
// only way to review what the run actually looked like afterwards, and the way
// to capture a rendering glitch that has already scrolled past.
constexpr const char* kScreenshotDir = "vnc.screenshot_dir";
}  // namespace keys

// Screen-control tools, with explicit JSON schemas so the same definitions
// work across Claude, OpenAI and Gemini.
//
// Every action tool answers with a fresh screenshot rather than only a
// confirmation string: the model needs to see the consequence of what it did,
// and a description of an action is not evidence that it worked.
std::vector<ToolSpec> makeComputerTools();

// Retrieves the session a tool should act on. Throws if it was never set.
VncSession& sessionFrom(Context& context);

// Requires vnc_click to be preceded by a vnc_zoom that contains the point
// being clicked, with no action in between. A click that fails the test is
// refused with an explanation rather than delivered.
//
// Off by default: a model that grounds well does not need it, and it costs a
// round trip per click. Worth turning on for local models, which have all
// been told to zoom first and have all ignored it — a position judged from a
// full screenshot is the most common way a run goes wrong.
void setRequireZoom(bool require);
bool requireZoom();

}  // namespace tapto
