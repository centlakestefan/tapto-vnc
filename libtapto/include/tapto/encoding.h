// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

namespace tapto {

// Force `in` into well-formed UTF-8. Anything we embed in the conversation
// history goes through nlohmann::json, and serializing a string that holds an
// illegal UTF-8 byte aborts the whole turn with
// json.exception.type_error.316 — "invalid UTF-8 byte at index 0: 0xFF".
//
// A lone 0xFF/0xFE as the first byte is the signature of a UTF-16 (or UTF-32)
// payload, which is what `type`/`cat`/redirections on Windows tend to produce.
// BOM-marked UTF-16/UTF-32 is transcoded to UTF-8 so the real content survives
// instead of turning into U+FFFD; a UTF-8 BOM is dropped; any other malformed
// sequence is replaced with U+FFFD so the result always serializes. Tool results are the intended callers; the user prompt is
// handled upstream and passed through unsanitized.
std::string sanitizeUtf8(const std::string& in);

// sanitizeUtf8 plus a WARN log line naming the tool that produced the bad
// bytes, so the offending command can be fixed at its source.
std::string sanitizeToolResult(const std::string& result, const std::string& tool_name);

} // namespace tapto
