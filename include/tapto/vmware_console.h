// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <string>

namespace tapto {

struct VCenterCredentials {
    std::string host;              // vCenter / ESXi host; HTTPS on 443 is assumed
    std::string username;
    std::string password;
    bool        insecure = false;  // skip TLS certificate verification
    bool        verbose  = false;  // log each API step (never logs credentials)
};

// Where a VM's console lives, and the short-lived credential to reach it.
struct ConsoleTicket {
    std::string host;
    int         port = 0;
    std::string ticket;

    // wss://<host>:<port>/ticket/<ticket> — the WebMKS endpoint.
    std::string websocketUrl() const;
};

// Authenticates to vCenter, resolves `vmName`, and acquires a WebMKS console
// ticket for it.
//
// The VM console is not a plain VNC listener: it is RFB tunneled over a
// TLS WebSocket on the ESXi host, gated by a ticket that expires quickly
// (single-digit minutes). Acquire it immediately before connecting.
//
// Throws std::runtime_error naming the step that failed.
ConsoleTicket acquireConsoleTicket(const VCenterCredentials& credentials,
                                   const std::string& vmName);

// A screen size in pixels, or {0,0} when the guest did not report one.
struct ScreenSize {
    int width  = 0;
    int height = 0;
    bool valid() const { return width > 0 && height > 0; }
};

// Asks the guest to change its screen resolution, through VMware Tools.
//
// The console framebuffer is simply whatever the guest's display is set to,
// which is why a VM last opened in the vSphere web client comes up at an odd
// size like 1904x861: the browser asked VMware Tools to match its viewport and
// the guest kept that setting afterwards. Call this before acquiring a console
// ticket — resizing an established RFB session would mean handling a desktop
// size change mid-stream, which VncSession does not do.
//
// This is a request to software inside the guest, not a hardware setting. It
// needs the VM powered on with VMware Tools running, and the guest applies it
// asynchronously: about ten seconds on the Windows guest this was measured on,
// so a readback taken immediately still shows the old size.
//
// Returns the size the guest reports once it settles. If it never reaches the
// requested size within `timeoutSeconds` this returns the last size seen
// rather than throwing, so the caller can decide whether to carry on — a guest
// that refuses a mode is not a reason to abandon the session. An invalid
// ScreenSize means the guest reported nothing at all, which usually means
// VMware Tools is not running.
//
// Throws std::runtime_error only if the request itself was rejected.
ScreenSize setScreenResolution(const VCenterCredentials& credentials,
                               const std::string& vmName,
                               int width, int height,
                               int timeoutSeconds = 30);

}  // namespace tapto
