// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context.h"

namespace tapto {

// An image a tool produced for the model to look at.
//
// Tool executors return a string, so an image cannot be part of the return
// value. Instead the tool leaves it in the Context and the backend picks it up
// when it builds the tool result. How it is delivered is provider-specific:
// Claude accepts image blocks inside a tool_result, whereas OpenAI's
// `role: "tool"` messages and Gemini's functionResponse parts are text-only
// and need the image appended as a following user message.
struct ToolImage {
    std::vector<uint8_t> png;
    int         width  = 0;
    int         height = 0;
    std::string label;   // what produced it, e.g. "clicked at (400,300)"

    bool empty() const { return png.empty(); }
};

namespace keys {
constexpr const char* kToolImage = "tool.image";
}

// Moves the pending image out of the context, if there is one. Returns false
// when no tool left an image, or when the image is empty.
bool takeToolImage(Context& context, ToolImage& image);

// Stores `image` for the backend to collect.
void putToolImage(Context& context, ToolImage image);

// Replaces all but the `keepMostRecent` newest images in a conversation
// history with a short text placeholder, and returns how many were replaced.
//
// A screen-driving agent attaches a screenshot to every action, and the whole
// history is re-sent on each request, so an unbounded transcript will hit the
// provider's request-size limit within a couple of dozen steps — observed as
// HTTP 413 request_too_large. Older screenshots are also the least useful
// part of the history: what matters is the current screen, and the text
// results still record what each earlier step did.
//
// Downscaling images would be the other lever, but it is deliberately not what
// this does: the model reports coordinates in image space, so shrinking the
// screenshots would require scaling every coordinate back and would silently
// degrade click accuracy. Dropping whole images keeps the ones that survive
// pixel-exact.
//
// Handles all three provider shapes: Claude image blocks, OpenAI image_url
// blocks, and Gemini inline_data parts.
size_t pruneHistoryImages(nlohmann::json& history, size_t keepMostRecent);

// Index of the earliest history entry that still holds a real image, or
// history.size() if none does.
//
// This is the boundary between the frozen and the changing parts of the
// transcript, and it is where a prompt-cache breakpoint belongs. Pruning
// rewrites entries in place, and prompt caching is a prefix match — so a
// breakpoint placed *after* a rewritten entry can never hit, because the bytes
// beneath it changed since the last request. Entries before this index have
// already been reduced to placeholders and will never change again, so they
// cache cleanly.
size_t firstLiveImageMessage(const nlohmann::json& history);

}  // namespace tapto
