// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>
#include <utility>

// Lightweight settings object for the AI backends.
class AiConfig {
public:
    int maxOutputTokens() const { return m_maxOutputTokens; }
    int maxToolIterations() const { return m_maxToolIterations; }
    int connectionTimeoutSeconds() const { return m_connectionTimeoutSeconds; }
    int readTimeoutSeconds() const { return m_readTimeoutSeconds; }
    // Older names; the timeouts apply to every dialect, not just openai.
    int openaiConnectionTimeoutSeconds() const { return m_connectionTimeoutSeconds; }
    int openaiReadTimeoutSeconds() const { return m_readTimeoutSeconds; }
    const std::string& effort() const { return m_effort; }
    const std::string& openaiReasoningEffort() const { return m_reasoningEffort; }
    int keepRecentImages() const { return m_keepRecentImages; }
    bool printCot() const { return m_printCot; }

    void setMaxOutputTokens(int v) { m_maxOutputTokens = v; }
    void setMaxToolIterations(int v) { m_maxToolIterations = v; }
    void setConnectionTimeoutSeconds(int v) { m_connectionTimeoutSeconds = v; }
    void setReadTimeoutSeconds(int v) { m_readTimeoutSeconds = v; }
    void setOpenaiConnectionTimeoutSeconds(int v) { m_connectionTimeoutSeconds = v; }
    void setOpenaiReadTimeoutSeconds(int v) { m_readTimeoutSeconds = v; }
    void setEffort(std::string v) { m_effort = std::move(v); }
    void setOpenaiReasoningEffort(std::string v) { m_reasoningEffort = std::move(v); }
    void setKeepRecentImages(int v) { m_keepRecentImages = v; }
    void setPrintCot(bool v) { m_printCot = v; }

private:
    int m_maxOutputTokens = 16000;
    int m_maxToolIterations = 200;
    int m_connectionTimeoutSeconds = 30;

    // How long to wait for the provider's answer. The response is not streamed,
    // so this is the whole generation: a 16k-token answer from a local model
    // decoding at 14 tok/s takes nineteen minutes and arrives all at once at
    // the end. A timeout shorter than the generation does not just fail the
    // turn -- the retry re-sends the same request, the server cancels the
    // work in progress, and the cycle repeats until the retry budget is spent,
    // which is the worst of every world. Programs set this from config
    // (read-timeout); the default is for a hosted provider.
    int m_readTimeoutSeconds = 300;

    // Claude's thinking depth and overall token spend, sent as
    // output_config.effort. Empty leaves the server default. A program whose
    // task is long-horizon and agentic (driving a GUI) wants "high"; a chat
    // client can leave it alone. The program decides, not the library.
    std::string m_effort;

    // The openai dialect's equivalent, sent as `reasoning_effort`. Empty means
    // the key is absent from the request, which is not the same as sending a
    // default: the server picks its own, and servers that have never heard of
    // the field keep working. No default is assumed here because, unlike
    // Claude's effort, the accepted values differ from one endpoint to the next.
    std::string m_reasoningEffort;

    // How many of the newest tool-produced images stay in the conversation as
    // real images; older ones are replaced with a placeholder before each
    // request. Full-screen PNGs are hundreds of kilobytes once base64-encoded
    // and the whole history is re-sent every request, so without a cap a long
    // run ends in HTTP 413. Three keeps "before, after, now" legible while
    // bounding the request size. 0 disables pruning. Only matters to programs
    // whose tools produce images (see tool_image.h).
    int m_keepRecentImages = 3;

    bool m_printCot = true; // surface intermediate reasoning/text to the user
};
