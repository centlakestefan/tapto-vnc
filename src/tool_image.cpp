// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/tool_image.h"

#include <functional>
#include <string>

namespace tapto {

void putToolImage(Context& context, ToolImage image) {
    context.set(keys::kToolImage, std::move(image));
}

bool takeToolImage(Context& context, ToolImage& image) {
    if (!context.has(keys::kToolImage)) return false;
    image = context.get<ToolImage>(keys::kToolImage);
    // Remove unconditionally: a stale image must never be attached to a later,
    // unrelated tool result.
    context.remove(keys::kToolImage);
    return !image.empty();
}

namespace {

constexpr const char* kPlaceholder =
    "[earlier screenshot omitted to stay within the request size limit; "
    "take a new screenshot if you need to see the screen again]";

// True for the image node of any of the three providers.
bool isImageNode(const nlohmann::json& node) {
    if (!node.is_object()) return false;
    if (node.contains("inline_data") || node.contains("inlineData")) return true;
    if (!node.contains("type") || !node["type"].is_string()) return false;
    const std::string type = node["type"].get<std::string>();
    return type == "image" || type == "image_url";
}

// Rewrites an image node in place as the text node of the same dialect.
void replaceWithPlaceholder(nlohmann::json& node) {
    const bool gemini = node.contains("inline_data") || node.contains("inlineData");
    node = gemini ? nlohmann::json{{"text", kPlaceholder}}
                  : nlohmann::json{{"type", "text"}, {"text", kPlaceholder}};
}

void walk(nlohmann::json& node, const std::function<void(nlohmann::json&)>& visit) {
    if (node.is_array()) {
        for (auto& child : node) {
            if (isImageNode(child)) visit(child);
            else walk(child, visit);
        }
    } else if (node.is_object()) {
        for (auto& entry : node.items()) {
            if (isImageNode(entry.value())) visit(entry.value());
            else walk(entry.value(), visit);
        }
    }
}

}  // namespace

size_t firstLiveImageMessage(const nlohmann::json& history) {
    if (!history.is_array()) return 0;
    for (size_t i = 0; i < history.size(); ++i) {
        bool found = false;
        // walk() takes a mutable reference, so probe a copy of the entry.
        nlohmann::json entry = history[i];
        walk(entry, [&](nlohmann::json&) { found = true; });
        if (found) return i;
    }
    return history.size();
}

size_t pruneHistoryImages(nlohmann::json& history, size_t keepMostRecent) {
    size_t total = 0;
    walk(history, [&](nlohmann::json&) { ++total; });
    if (total <= keepMostRecent) return 0;

    // Images are visited in document order, which for a conversation history is
    // oldest first, so the ones to drop are simply the leading `toReplace`.
    const size_t toReplace = total - keepMostRecent;
    size_t seen = 0;
    walk(history, [&](nlohmann::json& node) {
        if (seen++ < toReplace) replaceWithPlaceholder(node);
    });
    return toReplace;
}

}  // namespace tapto
