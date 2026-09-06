// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

class Context;

namespace tapto {

// Unassigned by IANA, and the only number a client's configuration has to
// agree with, so it is stated once and read by both the server and --help.
constexpr int kDefaultMcpPort = 8722;

// How to serve the screen-control tools to somebody else's model.
struct McpOptions {
    // Loopback only, so an unauthenticated server cannot be reached off-box.
    // The port is the one thing worth choosing, since it is the only part a
    // client's configuration has to agree with.
    int port = kDefaultMcpPort;

    // Handed to the client in the initialize result, where a host is expected
    // to put it in front of its model. The same prose the built-in agent gets
    // as its system prompt: the tools are no easier to aim without it just
    // because a different model is holding them.
    std::string instructions;
};

// Serves MCP over Streamable HTTP on 127.0.0.1 until interrupted, driving the
// session and tools already set up in `context`. Returns a process exit code.
//
// Blocks for the lifetime of the server. Tool calls are serialised: there is
// one screen, and two clients clicking on it at once is not a thing to support.
int runMcpServer(Context& context, const McpOptions& options);

}  // namespace tapto
