// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/mcp_server.h"

#include <algorithm>
#include <cctype>
#include <csignal>
#include <mutex>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "tapto/base64.h"
#include "tapto/context.h"
#include "tapto/log.h"
#include "tapto/tool_image.h"
#include "tapto/tool_registry.h"
#include "tapto/ui.h"
#include "tapto/version.h"

namespace tapto {
namespace {

using nlohmann::json;

// The single endpoint of the Streamable HTTP transport. One path, POST only:
// the GET half of that transport exists to open a server-to-client SSE stream,
// and this server never speaks first.
constexpr const char* kEndpoint = "/mcp";

// Revisions of the MCP spec this server can speak. Newest first, because that
// is also the order of preference when a client asks for one we do not know.
//
// The three differ in ways that do not reach a tools-only server — batching,
// session headers, output schemas — so claiming all three costs nothing and
// keeps older clients working.
const char* const kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

// JSON-RPC 2.0, which MCP carries verbatim.
constexpr int kParseError     = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;
constexpr int kInternalError  = -32603;

// A failure of the protocol rather than of the screen. Thrown by the handlers
// and turned into a JSON-RPC error object; a tool that merely did not work is
// not one of these, see callTool().
struct RpcError {
    int code;
    std::string message;
};

std::string lowercased(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Whether a browser-supplied Origin may talk to this server.
//
// This is the whole defence, and it is why it is here rather than left out of
// a "small" server. The listener is unauthenticated and bound to loopback,
// which stops another machine from reaching it directly but not from asking
// the user's own browser to: any page the user visits can POST to
// http://127.0.0.1:8722/mcp, and a DNS rebinding attack turns a hostname the
// browser thinks is the attacker's into this address. What that page cannot do
// is forge the Origin header. So a request either carries a loopback Origin,
// or carries none at all — which is what a native client such as an editor or
// a CLI sends, since Origin is a browser concept.
bool originAllowed(const std::string& origin) {
    if (origin.empty()) return true;  // not a browser

    const std::string separator = "://";
    const size_t schemeEnd = origin.find(separator);
    if (schemeEnd == std::string::npos) return false;

    const std::string scheme = lowercased(origin.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") return false;

    std::string host = origin.substr(schemeEnd + separator.size());
    // A bracketed IPv6 literal keeps its colons; anything else is cut at the
    // port separator.
    if (!host.empty() && host.front() == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos) return false;
        host = host.substr(1, close - 1);
    } else if (const size_t colon = host.find(':'); colon != std::string::npos) {
        host = host.substr(0, colon);
    }
    host = lowercased(host);

    return host == "localhost" || host == "127.0.0.1" || host == "::1";
}

json rpcResult(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpcError(const json& id, int code, const std::string& message) {
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", json{{"code", code}, {"message", message}}}};
}

// The server proper. One instance per process; the mutex below is the reason
// it is an object at all.
class McpServer {
public:
    McpServer(Context& context, const McpOptions& options)
        : m_context(context), m_options(options) {}

    int run();

private:
    // Returns the reply to send, or a null json for a notification, which
    // JSON-RPC says is answered with nothing at all.
    json dispatch(const json& message);

    json initialize(const json& params) const;
    json listTools() const;
    json callTool(const json& params);

    Context&          m_context;
    const McpOptions& m_options;

    // One screen, one mouse, one keyboard. httplib answers each request on its
    // own thread, and VncSession is not thread-safe — but even if it were,
    // two clients interleaving a click and a screenshot would be describing a
    // screen neither of them caused. Held across the executor *and* the image
    // handover below, because the image is passed through the shared Context
    // and a second caller would otherwise collect the first one's screenshot.
    std::mutex m_screen;
};

json McpServer::initialize(const json& params) const {
    // Echo the client's version when it is one we know, so a client pinned to
    // an older revision is not told to speak a newer one. Otherwise name our
    // newest and let it decide whether it can live with that, which is what
    // the spec asks a server to do.
    std::string version = kProtocolVersions[0];
    if (params.is_object() && params.contains("protocolVersion") &&
        params["protocolVersion"].is_string()) {
        const std::string asked = params["protocolVersion"].get<std::string>();
        for (const char* known : kProtocolVersions) {
            if (asked == known) { version = asked; break; }
        }
    }

    json result{
        {"protocolVersion", version},
        // No listChanged: the tool list is built once at startup and cannot
        // change while the process runs.
        {"capabilities", json{{"tools", json::object()}}},
        {"serverInfo", json{{"name", "tapto-vnc"}, {"version", TAPTO_VNC_VERSION}}},
    };
    if (!m_options.instructions.empty()) result["instructions"] = m_options.instructions;
    return result;
}

json McpServer::listTools() const {
    json tools = json::array();
    for (const ToolSpec& tool : m_context.tools) {
        tools.push_back(tool_definition_to_json(tool, ToolFormat::Mcp));
    }
    return json{{"tools", std::move(tools)}};
}

json McpServer::callTool(const json& params) {
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
        throw RpcError{kInvalidParams, "tools/call needs a string 'name'"};
    }
    const std::string name = params["name"].get<std::string>();

    // Absent arguments are an empty object, not an error: a tool whose schema
    // has no required fields — vnc_screenshot — is legitimately called with
    // nothing.
    json arguments = json::object();
    if (params.contains("arguments") && params["arguments"].is_object()) {
        arguments = params["arguments"];
    }

    const ToolSpec* spec = nullptr;
    for (const ToolSpec& tool : m_context.tools) {
        if (tool.name == name) { spec = &tool; break; }
    }
    if (spec == nullptr) {
        // A protocol error rather than a tool result. The distinction is the
        // client's to act on: a tool that failed is something for the model to
        // read and work around, whereas a tool that does not exist means the
        // client is calling something it was never offered.
        std::string available;
        for (const ToolSpec& tool : m_context.tools) available += " " + tool.name;
        throw RpcError{kInvalidParams,
                       "Unknown tool '" + name + "'. Available tools:" + available};
    }

    // What the model is about to do to the screen, in the same words the
    // built-in agent's status line uses, so somebody watching this terminal
    // can follow a run driven from somewhere else.
    const std::string label = getToolDisplayName(m_context.tools, name, arguments);

    std::string text;
    ToolImage image;
    bool failed = false;
    {
        std::lock_guard<std::mutex> lock(m_screen);
        // Inside the lock, not before it: two clients would otherwise interleave
        // their lines, and a call that waited for the screen would be announced
        // when it arrived rather than when it happened. The order printed here
        // is the order the screen actually saw.
        ui::print_line(label);
        mclog("MCP tools/call " + name + ": " + label + "\n");
        try {
            text = spec->executor(m_context, arguments);
        } catch (const ConnectionLost& e) {
            // The tools already tried to reconnect and could not. The built-in
            // agent lets this end the run, because a model that cannot see the
            // failure just burns API calls retrying; an MCP client is somebody
            // else's loop, so it is told plainly and the server stays up for
            // whoever fixes the console and comes back.
            text = std::string("ERROR: ") + e.what();
            failed = true;
        } catch (const std::exception& e) {
            // An ordinary tool failure: bad arguments, something not where the
            // model thought it was. isError lets the model read it and try
            // something else, which is exactly what it is for.
            text = std::string("ERROR: ") + e.what();
            failed = true;
        }
        takeToolImage(m_context, image);
    }

    // These tools report a bad argument by *returning* "ERROR: ..." — there
    // are seventeen such returns against a single throw — because the agent
    // loop feeds the string straight back to the model and a thrown exception
    // would have ended the turn. MCP has somewhere better to put that: isError
    // tells the client this was a failed call rather than a description of a
    // screen, which is the difference between a model reading the message and
    // a client counting the call as progress. Prefix only, so a result that
    // merely mentions the word is not mistaken for one.
    if (!failed && text.rfind("ERROR:", 0) == 0) failed = true;

    json content = json::array();
    content.push_back(json{{"type", "text"}, {"text", text}});
    if (!image.empty()) {
        content.push_back(json{{"type", "image"},
                               {"data", base64Encode(image.png)},
                               {"mimeType", "image/png"}});
    }
    return json{{"content", std::move(content)}, {"isError", failed}};
}

json McpServer::dispatch(const json& message) {
    if (!message.is_object()) {
        return rpcError(nullptr, kInvalidRequest, "expected a JSON-RPC object");
    }

    // Absent id means a notification: it gets no reply, whatever happens.
    const bool notification = !message.contains("id") || message["id"].is_null();
    const json id = notification ? json(nullptr) : message["id"];

    if (!message.contains("method") || !message["method"].is_string()) {
        return notification ? json() : rpcError(id, kInvalidRequest, "missing 'method'");
    }
    const std::string method = message["method"].get<std::string>();
    const json params = message.contains("params") ? message["params"] : json::object();

    try {
        json result;
        if (method == "initialize") {
            result = initialize(params);
        } else if (method == "tools/list") {
            result = listTools();
        } else if (method == "tools/call") {
            result = callTool(params);
        } else if (method == "ping") {
            result = json::object();
        } else if (method.rfind("notifications/", 0) == 0) {
            // initialized, cancelled, and anything else a client announces.
            // Nothing here acts on them, and a notification is unanswerable by
            // definition, so accepting them silently is the whole handling.
            return json();
        } else {
            if (notification) return json();
            // resources/* and prompts/* land here. This server advertises
            // neither capability, so a client asking for them is asking for
            // something it was told does not exist.
            throw RpcError{kMethodNotFound, "unknown method '" + method + "'"};
        }
        return notification ? json() : rpcResult(id, std::move(result));
    } catch (const RpcError& e) {
        return notification ? json() : rpcError(id, e.code, e.message);
    } catch (const std::exception& e) {
        mclog(std::string("MCP internal error in '") + method + "': " + e.what() + "\n");
        return notification ? json() : rpcError(id, kInternalError, e.what());
    }
}

// httplib::listen() blocks, so stopping is somebody else's job. The server
// outlives every handler, and stop() is the one method documented as callable
// from another thread.
httplib::Server* g_running = nullptr;

void onInterrupt(int) {
    if (g_running != nullptr) g_running->stop();
}

int McpServer::run() {
    httplib::Server server;

    server.Post(kEndpoint, [this](const httplib::Request& req, httplib::Response& res) {
        if (!originAllowed(req.get_header_value("Origin"))) {
            mclog("MCP rejected a request from origin '" +
                  req.get_header_value("Origin") + "'\n");
            res.status = 403;
            res.set_content(rpcError(nullptr, kInvalidRequest, "origin not allowed").dump(),
                            "application/json");
            return;
        }

        json message;
        try {
            message = json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(rpcError(nullptr, kParseError, e.what()).dump(),
                            "application/json");
            return;
        }

        // A batch is a JSON array. Dropped from the spec in 2025-06-18 but
        // legal in the revisions before it, and cheap enough to keep: dispatch
        // each element and return the replies that are not notifications.
        json reply;
        if (message.is_array()) {
            reply = json::array();
            for (const json& one : message) {
                json answer = dispatch(one);
                if (!answer.is_null()) reply.push_back(std::move(answer));
            }
            if (reply.empty()) reply = json();
        } else {
            reply = dispatch(message);
        }

        // Nothing to say: every message in the request was a notification.
        // 202 is what the transport specifies for that, and a body would be a
        // reply to something that did not ask for one.
        if (reply.is_null()) {
            res.status = 202;
            return;
        }
        res.status = 200;
        res.set_content(reply.dump(), "application/json");
    });

    // The transport allows a client to open an SSE stream here for messages
    // the server starts. This one never does, and saying so plainly beats
    // leaving the client to time out waiting for a stream that will stay
    // silent.
    server.Get(kEndpoint, [](const httplib::Request&, httplib::Response& res) {
        res.status = 405;
        res.set_content(
            rpcError(nullptr, kMethodNotFound, "this server sends no unsolicited messages")
                .dump(),
            "application/json");
    });

    // Undo one of cpp-httplib's defaults. On anything but Windows it sets
    // SO_REUSEPORT on the listening socket, which is not the "let me rebind
    // after a restart" option it looks like: it lets a *second* process bind
    // the same port and have the kernel share incoming connections between the
    // two. For a web server serving identical content from a worker pool that
    // is the point. Here the two processes are two consoles on two different
    // machines, and a client would get its screenshot from one and have its
    // click land on the other — the model aiming at a screen it never saw, with
    // nothing in either log to say so. A second instance must fail to start
    // instead, which is what Windows already does via SO_EXCLUSIVEADDRUSE, so
    // only the POSIX branch changes here.
    //
    // SO_REUSEADDR is kept: it permits rebinding a port still in TIME_WAIT from
    // a previous run, so stopping and restarting works, and it does not permit
    // joining a socket that is actively listening.
    server.set_socket_options([](socket_t sock) {
        int yes = 1;
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const void*>(&yes), sizeof(yes));
#endif
    });

    g_running = &server;
    std::signal(SIGINT, onInterrupt);
#ifndef _WIN32
    std::signal(SIGTERM, onInterrupt);
#endif

    // Bound to the loopback address itself, not merely defaulted to it: the
    // server has no authentication, so being unreachable from another machine
    // is a property it should not be able to lose to a config mistake.
    //
    // Binding and listening are two calls rather than one so that a port
    // conflict is known before anything is printed. listen() would block, so
    // the only place left to announce the server would be ahead of the thing
    // that can fail — which is how the earlier version came to print a URL
    // that was never served.
    if (!server.bind_to_port("127.0.0.1", m_options.port)) {
        ui::print_error("could not bind 127.0.0.1:" + std::to_string(m_options.port) +
                        " — the port is probably already in use, perhaps by another "
                        "tapto-vnc. Pick another with --mcp-port.");
        mclog("MCP bind to port " + std::to_string(m_options.port) + " failed\n");
        g_running = nullptr;
        return 1;
    }

    const std::string url =
        "http://127.0.0.1:" + std::to_string(m_options.port) + kEndpoint;
    ui::print_line("MCP server on " + url);
    ui::print_line("Add it with: claude mcp add --transport http tapto-vnc " + url);
    ui::print_line("Ctrl-C to stop.");
    mclog("MCP server listening on " + url + "\n");

    const bool clean = server.listen_after_bind();
    g_running = nullptr;

    if (!clean) {
        ui::print_error("the MCP server stopped unexpectedly.");
        mclog("MCP server stopped unexpectedly\n");
        return 1;
    }

    ui::print_line("MCP server stopped.");
    mclog("MCP server stopped\n");
    return 0;
}

}  // namespace

int runMcpServer(Context& context, const McpOptions& options) {
    return McpServer(context, options).run();
}

}  // namespace tapto
