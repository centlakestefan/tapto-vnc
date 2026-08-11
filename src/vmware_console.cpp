// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// Ported from the testvnc prototype (vmware_auth.cpp / vmware_ticket.cpp).
// Behavioural differences from that original are called out inline.

#include "tapto/vmware_console.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace tapto {
namespace {

constexpr int kVCenterPort = 443;
constexpr int kTimeoutSeconds = 15;

void logStep(bool verbose, const std::string& message) {
    if (verbose) std::fprintf(stderr, "[vmware] %s\n", message.c_str());
}

// Percent-encodes everything outside the RFC 3986 unreserved set. The original
// pasted VM names straight into the query string, so any name with a space or
// '&' produced a malformed request.
std::string urlEncode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// Escapes XML character data. The original interpolated the username and
// password into the SOAP envelope raw, so a password containing '&' or '<'
// produced invalid XML and an opaque login failure.
std::string xmlEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out.push_back(c);
        }
    }
    return out;
}

std::string extractTag(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const size_t start = xml.find(open);
    if (start == std::string::npos) return {};
    const size_t from = start + open.size();
    const size_t end = xml.find(close, from);
    if (end == std::string::npos) return {};
    return xml.substr(from, end - from);
}

std::unique_ptr<httplib::SSLClient> makeClient(const VCenterCredentials& credentials) {
    auto client = std::make_unique<httplib::SSLClient>(credentials.host, kVCenterPort);
    client->set_connection_timeout(kTimeoutSeconds, 0);
    client->set_read_timeout(kTimeoutSeconds, 0);
    client->set_write_timeout(kTimeoutSeconds, 0);
    if (credentials.insecure) {
        client->enable_server_certificate_verification(false);
    }
    return client;
}

[[noreturn]] void fail(const std::string& step, const std::string& detail) {
    throw std::runtime_error("vmware: " + step + ": " + detail);
}

std::string describe(const httplib::Result& res) {
    if (!res) return httplib::to_string(res.error());
    return "HTTP " + std::to_string(res->status) + " " + res->body;
}

// vim25 reports application errors as SOAP faults carrying HTTP 500, and its
// <faultstring> is often empty. The fault's real name is the first element
// inside <detail> — "ToolsUnavailableFault", "InvalidPowerStateFault" — which
// is the part worth putting in front of a user.
std::string describeFault(const httplib::Result& res) {
    if (!res) return httplib::to_string(res.error());

    const std::string& body = res->body;
    const size_t detail = body.find("<detail>");
    if (detail != std::string::npos) {
        const size_t open = body.find('<', detail + 8);
        if (open != std::string::npos) {
            const size_t end = body.find_first_of(" >/", open + 1);
            if (end != std::string::npos && end > open + 1) {
                return body.substr(open + 1, end - open - 1);
            }
        }
    }
    const std::string message = extractTag(body, "faultstring");
    if (!message.empty()) return message;
    return "HTTP " + std::to_string(res->status);
}

// Extracts a session token from either response shape, or returns empty.
//
// On vSphere 7.0+ /api/session answers with a bare JSON string. On older
// deployments the same path is the legacy vAPI *JSON-RPC* endpoint, which
// answers HTTP 200 carrying {"jsonrpc":..,"error":{..}} — a success status
// wrapping a failure. Status alone therefore cannot decide whether login
// worked; the body has to be inspected.
std::string tokenFromBody(const std::string& body) {
    try {
        const auto json = nlohmann::json::parse(body);
        if (json.is_string()) return json.get<std::string>();
        if (json.contains("jsonrpc") || json.contains("error")) return {};
        if (json.contains("value") && json["value"].is_string()) {
            return json["value"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // Not JSON, or not a shape we recognise — treat as no token.
    }
    return {};
}

// Step 1 — REST session token, used only to look the VM up by name.
std::string restLogin(httplib::SSLClient& client, const VCenterCredentials& credentials) {
    client.set_basic_auth(credentials.username.c_str(), credentials.password.c_str());

    static const char* kSessionPaths[] = {
        "/api/session",                      // vSphere 7.0+
        "/rest/com/vmware/cis/session",      // legacy
    };

    std::string lastError = "no endpoint answered";
    for (const char* path : kSessionPaths) {
        auto res = client.Post(path, "", "application/json");
        if (!res) {
            lastError = std::string(path) + ": " + httplib::to_string(res.error());
            continue;
        }
        if (res->status != 200 && res->status != 201) {
            lastError = std::string(path) + ": HTTP " + std::to_string(res->status);
            continue;
        }
        std::string token = tokenFromBody(res->body);
        if (!token.empty()) return token;
        lastError = std::string(path) + ": HTTP 200 without a session token";
    }
    fail("REST login", lastError);
}

// Step 2 — resolve the VM name to a managed object id such as "vm-123".
std::string findVmId(httplib::SSLClient& client,
                     const std::string& sessionToken,
                     const std::string& vmName) {
    const httplib::Headers headers = {{"vmware-api-session-id", sessionToken}};
    const std::string query = "?filter.names=" + urlEncode(vmName);

    auto res = client.Get(("/api/vcenter/vm" + query).c_str(), headers);
    if (res && (res->status == 404 || res->status == 405)) {
        res = client.Get(("/rest/vcenter/vm" + query).c_str(), headers);
    }
    if (!res || res->status != 200) {
        fail("VM lookup", describe(res));
    }

    try {
        const auto json = nlohmann::json::parse(res->body);
        // Same shape split as above: /api is a bare array.
        const auto& list = json.is_array() ? json : json["value"];
        if (!list.is_array() || list.empty()) {
            fail("VM lookup", "no VM named '" + vmName + "'");
        }
        if (list.size() > 1) {
            fail("VM lookup", "name '" + vmName + "' matched " +
                                  std::to_string(list.size()) + " VMs; use an exact name");
        }
        if (!list[0].contains("vm")) {
            fail("VM lookup", "response has no 'vm' field");
        }
        return list[0]["vm"].get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        fail("VM lookup", std::string("bad JSON: ") + e.what());
    }
}

// Step 3a — the SOAP endpoint keeps its own session, independent of REST.
std::string soapLogin(httplib::SSLClient& client, const VCenterCredentials& credentials) {
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "xmlns:vim25=\"urn:vim25\">"
        "<soapenv:Body><vim25:Login>"
        "<_this type=\"SessionManager\">SessionManager</_this>"
        "<userName>" + xmlEscape(credentials.username) + "</userName>"
        "<password>" + xmlEscape(credentials.password) + "</password>"
        "</vim25:Login></soapenv:Body></soapenv:Envelope>";

    // Deliberately not logged: this envelope contains the password in the
    // clear, which the original printed to stdout at [DEBUG].
    const httplib::Headers headers = {{"SOAPAction", "urn:vim25/7.0"}};
    auto res = client.Post("/sdk", headers, body, "text/xml; charset=utf-8");

    if (!res || res->status != 200) {
        fail("SOAP login", res && res->status != 200 ? "HTTP " + std::to_string(res->status)
                                                     : describe(res));
    }

    const auto it = res->headers.find("Set-Cookie");
    if (it == res->headers.end()) fail("SOAP login", "no Set-Cookie header");

    const std::string& cookie = it->second;
    const std::string key = "vmware_soap_session=";
    const size_t start = cookie.find(key);
    if (start == std::string::npos) fail("SOAP login", "no vmware_soap_session cookie");

    const size_t from = start + key.size();
    const size_t end = cookie.find(';', from);
    return end == std::string::npos ? cookie.substr(from) : cookie.substr(from, end - from);
}

// Step 3b — AcquireTicket("webmks") returns the console host, port and ticket.
ConsoleTicket acquireTicket(httplib::SSLClient& client,
                            const std::string& soapCookie,
                            const std::string& vmId) {
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "xmlns:vim25=\"urn:vim25\">"
        "<soapenv:Body><vim25:AcquireTicket>"
        "<_this type=\"VirtualMachine\">" + xmlEscape(vmId) + "</_this>"
        "<ticketType>webmks</ticketType>"
        "</vim25:AcquireTicket></soapenv:Body></soapenv:Envelope>";

    const httplib::Headers headers = {
        {"SOAPAction", "urn:vim25/7.0"},
        {"Cookie", "vmware_soap_session=" + soapCookie},
    };
    auto res = client.Post("/sdk", headers, body, "text/xml; charset=utf-8");

    if (!res || res->status != 200) {
        fail("AcquireTicket", describe(res));
    }

    ConsoleTicket ticket;
    ticket.ticket = extractTag(res->body, "ticket");
    ticket.host   = extractTag(res->body, "host");
    const std::string port = extractTag(res->body, "port");

    // `host` is optional in the vSphere API: when absent, the console lives on
    // the same host the request was made to.
    if (ticket.ticket.empty() || port.empty()) {
        fail("AcquireTicket", "could not parse ticket/port from SOAP response");
    }
    try {
        ticket.port = std::stoi(port);
    } catch (const std::exception&) {
        fail("AcquireTicket", "non-numeric port '" + port + "'");
    }
    return ticket;
}

// An authenticated connection with the VM already resolved. Both public entry
// points need exactly this much state, and getting there costs two logins, so
// it is worth naming.
struct VmSession {
    std::unique_ptr<httplib::SSLClient> client;
    std::string vmId;
    std::string soapCookie;
};

VmSession openVmSession(const VCenterCredentials& credentials, const std::string& vmName) {
    if (credentials.host.empty())     fail("config", "vCenter host is empty");
    if (credentials.username.empty()) fail("config", "username is empty");
    if (vmName.empty())               fail("config", "VM name is empty");

    VmSession session;
    session.client = makeClient(credentials);

    logStep(credentials.verbose, "authenticating to " + credentials.host);
    const std::string sessionToken = restLogin(*session.client, credentials);

    logStep(credentials.verbose, "resolving VM '" + vmName + "'");
    session.vmId = findVmId(*session.client, sessionToken, vmName);
    logStep(credentials.verbose, "VM id " + session.vmId);

    logStep(credentials.verbose, "SOAP login");
    session.soapCookie = soapLogin(*session.client, credentials);
    return session;
}

httplib::Result soapCall(VmSession& session, const std::string& body) {
    const httplib::Headers headers = {
        {"SOAPAction", "urn:vim25/7.0"},
        {"Cookie", "vmware_soap_session=" + session.soapCookie},
    };
    return session.client->Post("/sdk", headers, body, "text/xml; charset=utf-8");
}

// Reads guest.screen — what VMware Tools inside the guest reports the display
// to be. This is the only trustworthy confirmation that a resize took effect;
// SetScreenResolution returning cleanly only means the request was delivered.
ScreenSize readGuestScreen(VmSession& session) {
    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "xmlns:vim25=\"urn:vim25\">"
        "<soapenv:Body><vim25:RetrievePropertiesEx>"
        "<_this type=\"PropertyCollector\">propertyCollector</_this>"
        "<specSet>"
        "<propSet><type>VirtualMachine</type><pathSet>guest.screen</pathSet></propSet>"
        "<objectSet><obj type=\"VirtualMachine\">" + xmlEscape(session.vmId) + "</obj></objectSet>"
        "</specSet>"
        "<options/>"
        "</vim25:RetrievePropertiesEx></soapenv:Body></soapenv:Envelope>";

    auto res = soapCall(session, body);
    if (!res || res->status != 200) return {};

    // guest.screen is the only property requested, so its <width>/<height> are
    // the only ones in the response.
    ScreenSize size;
    try {
        const std::string width  = extractTag(res->body, "width");
        const std::string height = extractTag(res->body, "height");
        if (width.empty() || height.empty()) return {};
        size.width  = std::stoi(width);
        size.height = std::stoi(height);
    } catch (const std::exception&) {
        return {};
    }
    return size;
}

}  // namespace

std::string ConsoleTicket::websocketUrl() const {
    std::ostringstream url;
    url << "wss://" << host << ":" << port << "/ticket/" << ticket;
    return url.str();
}

ConsoleTicket acquireConsoleTicket(const VCenterCredentials& credentials,
                                   const std::string& vmName) {
    VmSession session = openVmSession(credentials, vmName);

    logStep(credentials.verbose, "acquiring webmks ticket");
    ConsoleTicket ticket = acquireTicket(*session.client, session.soapCookie, session.vmId);
    if (ticket.host.empty()) ticket.host = credentials.host;

    // The ticket itself is a bearer credential — log where, never what.
    logStep(credentials.verbose,
            "console at " + ticket.host + ":" + std::to_string(ticket.port));
    return ticket;
}

ScreenSize setScreenResolution(const VCenterCredentials& credentials,
                               const std::string& vmName,
                               int width, int height,
                               int timeoutSeconds) {
    VmSession session = openVmSession(credentials, vmName);

    const std::string requested = std::to_string(width) + "x" + std::to_string(height);
    logStep(credentials.verbose, "requesting screen resolution " + requested);

    const std::string body =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<soapenv:Envelope xmlns:soapenv=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "xmlns:vim25=\"urn:vim25\">"
        "<soapenv:Body><vim25:SetScreenResolution>"
        "<_this type=\"VirtualMachine\">" + xmlEscape(session.vmId) + "</_this>"
        "<width>" + std::to_string(width) + "</width>"
        "<height>" + std::to_string(height) + "</height>"
        "</vim25:SetScreenResolution></soapenv:Body></soapenv:Envelope>";

    auto res = soapCall(session, body);
    if (!res || res->status != 200) {
        fail("SetScreenResolution", describeFault(res));
    }

    // The call returns as soon as the request reaches VMware Tools; the guest
    // then reconfigures its display in its own time. Poll guest.screen rather
    // than sleeping a fixed interval and hoping.
    constexpr int kPollSeconds = 2;
    ScreenSize seen;
    for (int waited = 0; ; waited += kPollSeconds) {
        seen = readGuestScreen(session);
        if (seen.width == width && seen.height == height) {
            logStep(credentials.verbose,
                    "guest reports " + requested + " after " + std::to_string(waited) + "s");
            return seen;
        }
        if (waited >= timeoutSeconds) break;
        std::this_thread::sleep_for(std::chrono::seconds(kPollSeconds));
    }

    logStep(credentials.verbose,
            "guest has not reported " + requested + " within " +
                std::to_string(timeoutSeconds) + "s");
    return seen;
}

}  // namespace tapto
