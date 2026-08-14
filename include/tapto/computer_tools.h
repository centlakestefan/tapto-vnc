// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
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

// Optional `std::function<bool()>` that re-establishes the connection the
// tools drive, returning whether it worked. Without it a dropped console ends
// the run; with it the tools get one attempt to carry on.
//
// A closure rather than a set of parameters because reconnecting is not always
// redialling the same address: a VMware console ticket is single-use and
// expires in minutes, so restoring one means logging in to vCenter again and
// asking for another. Only the caller that opened the session knows how it was
// opened.
constexpr const char* kReconnect = "vnc.reconnect";
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

// Gridline spacing, in screen pixels, for the full screenshots every tool
// returns. Zero — the default — leaves them plain.
//
// The zoom's rulers are known to work. Whether the same trick pays at 1:1 is
// open: a full screenshot is where a vision model's downscaling bites hardest,
// so the labels are least legible exactly where the picture is least detailed,
// and the gridlines cross every piece of text on the screen. Hence a setting
// to be measured rather than a default, and hence the spacing being the
// caller's choice.
//
// Call before makeComputerTools(): vnc_screenshot's description changes with
// it, since a grid nobody explains is just something to reason about.
void setScreenshotGrid(int step);
int  screenshotGrid();

}  // namespace tapto
