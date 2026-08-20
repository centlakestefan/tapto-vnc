// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

// Lightweight settings object for the AI backends.
class AiConfig {
public:
    int maxOutputTokens() const { return m_maxOutputTokens; }
    int maxToolIterations() const { return m_maxToolIterations; }
    int openaiConnectionTimeoutSeconds() const { return m_connectionTimeoutSeconds; }
    int openaiReadTimeoutSeconds() const { return m_readTimeoutSeconds; }
    bool printCot() const { return m_printCot; }
    const std::string& effort() const { return m_effort; }
    const std::string& openaiReasoningEffort() const { return m_reasoningEffort; }
    int keepRecentImages() const { return m_keepRecentImages; }

    void setMaxOutputTokens(int v) { m_maxOutputTokens = v; }
    void setMaxToolIterations(int v) { m_maxToolIterations = v; }
    void setOpenaiConnectionTimeoutSeconds(int v) { m_connectionTimeoutSeconds = v; }
    void setOpenaiReadTimeoutSeconds(int v) { m_readTimeoutSeconds = v; }
    void setPrintCot(bool v) { m_printCot = v; }
    void setEffort(const std::string& v) { m_effort = v; }
    void setOpenaiReasoningEffort(const std::string& v) { m_reasoningEffort = v; }
    void setKeepRecentImages(int v) { m_keepRecentImages = v; }

private:
    int m_maxOutputTokens = 16000;
    int m_maxToolIterations = 200;
    int m_connectionTimeoutSeconds = 30;
    int m_readTimeoutSeconds = 300;
    bool m_printCot = true; // surface intermediate reasoning/text to the terminal

    // Thinking depth and overall token spend. Empty leaves the server default
    // (high). Driving a GUI is a long-horizon agentic task, which is exactly
    // where the higher settings earn their cost.
    std::string m_effort = "high";

    // The openai dialect's equivalent, sent as `reasoning_effort`. Empty means
    // the key is absent from the request, which is not the same as sending a
    // default: the server picks its own, and servers that have never heard of
    // the field keep working. No default is assumed here because, unlike
    // Claude's effort, the accepted values differ from one endpoint to the next.
    std::string m_reasoningEffort;

    // How many of the newest screenshots stay in the conversation as real
    // images. Full-screen PNGs are hundreds of kilobytes once base64-encoded
    // and the whole history is re-sent every request, so without a cap a long
    // run ends in HTTP 413. Three keeps "before, after, now" legible while
    // bounding the request size.
    int m_keepRecentImages = 3;
};
