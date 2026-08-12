// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB
//
// Slice-1 smoke test: connect to a VNC server, capture the screen, write a PNG.
// No model, no tools — this exists to prove the vendored RFB stack and the
// framebuffer compositing work before any of the agent plumbing is wired in.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tapto/vmware_console.h"
#include "tapto/vnc_session.h"

namespace {

void usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n\n"
        << "Direct VNC (default):\n"
        << "  --host <host>       VNC server host (default: localhost)\n"
        << "  --display <n>       Display number; port is 5900+n (default: 0)\n"
        << "  --password <pw>     VNC password (default: none)\n\n"
        << "VMware console (WebMKS over wss):\n"
        << "  --vcenter <host>    vCenter/ESXi host; enables VMware mode\n"
        << "  --vm <name>         VM name to open the console for\n"
        << "  --user <name>       vCenter username (or $TAPTO_VCENTER_USER)\n"
        << "  --insecure          Skip TLS certificate verification\n"
        << "  Password is read from $TAPTO_VCENTER_PASSWORD.\n\n"
        << "Common:\n"
        << "  --out <path>        Output PNG path (default: screenshot.png)\n"
        << "  --timeout <ms>      Capture timeout (default: 3000)\n"
        << "  --settle <ms>       Quiet period before sampling (default: 300)\n"
        << "  --encodings <list>  Comma-separated subset to offer the server,\n"
        << "                      e.g. raw | hextile | zrle,hextile,raw\n"
        << "  --zoom <x,y,w,h>    Also write <out>.zoom.png: that region only,\n"
        << "                      enlarged, exactly as the vnc_zoom tool renders it\n"
        << "  --grid <px>         Rule and grid <out> every <px> screen pixels, as\n"
        << "                      tapto-vnc --grid does. Use it to see whether the\n"
        << "                      labels stay readable before spending a run on it.\n"
        << "  -v, --verbose       Log each connection step\n"
        << "  --wake              Nudge the console first, to dismiss a blank\n"
        << "                      screen saver before capturing\n"
        << "  -h, --help          Show this help\n";
}

std::string fromEnv(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// Returns the value following `flag`, or nullptr if it is absent or unpaired.
const char* option(int argc, char** argv, int& i, const char* flag) {
    if (std::string(argv[i]) != flag) return nullptr;
    if (i + 1 >= argc) {
        std::cerr << "ERROR: " << flag << " requires a value\n";
        std::exit(2);
    }
    return argv[++i];
}

}  // namespace

int main(int argc, char** argv) {
    tapto::VncOptions options;
    tapto::VCenterCredentials vcenter;
    std::string vmName;
    std::string out = "screenshot.png";
    int timeoutMs = 3000;
    int settleMs = 300;
    bool verbose = false;
    bool wake = false;
    std::string encodings;
    std::string zoom;
    int gridStep = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--insecure") { vcenter.insecure = true; continue; }
        if (arg == "--wake") { wake = true; continue; }
        if (arg == "-v" || arg == "--verbose") { verbose = true; continue; }
        if (const char* v = option(argc, argv, i, "--host"))     { options.host = v; continue; }
        if (const char* v = option(argc, argv, i, "--display"))  { options.display = static_cast<uint16_t>(std::atoi(v)); continue; }
        if (const char* v = option(argc, argv, i, "--password")) { options.password = v; continue; }
        if (const char* v = option(argc, argv, i, "--vcenter"))  { vcenter.host = v; continue; }
        if (const char* v = option(argc, argv, i, "--vm"))       { vmName = v; continue; }
        if (const char* v = option(argc, argv, i, "--user"))     { vcenter.username = v; continue; }
        if (const char* v = option(argc, argv, i, "--encodings")) { encodings = v; continue; }
        if (const char* v = option(argc, argv, i, "--zoom"))      { zoom = v; continue; }
        if (const char* v = option(argc, argv, i, "--grid"))      { gridStep = std::atoi(v); continue; }
        if (const char* v = option(argc, argv, i, "--out"))      { out = v; continue; }
        if (const char* v = option(argc, argv, i, "--timeout"))  { timeoutMs = std::atoi(v); continue; }
        if (const char* v = option(argc, argv, i, "--settle"))   { settleMs = std::atoi(v); continue; }
        std::cerr << "ERROR: unknown argument '" << arg << "'\n\n";
        usage(argv[0]);
        return 2;
    }

    const bool vmwareMode = !vcenter.host.empty();
    vcenter.verbose = verbose;
    if (vcenter.username.empty()) vcenter.username = fromEnv("TAPTO_VCENTER_USER");
    vcenter.password = fromEnv("TAPTO_VCENTER_PASSWORD");

    if (vmwareMode) {
        if (vmName.empty()) {
            std::cerr << "ERROR: --vcenter requires --vm\n";
            return 2;
        }
        if (vcenter.username.empty()) {
            std::cerr << "ERROR: no username; pass --user or set TAPTO_VCENTER_USER\n";
            return 2;
        }
        if (vcenter.password.empty()) {
            std::cerr << "ERROR: set TAPTO_VCENTER_PASSWORD\n";
            return 2;
        }
    }

    try {
        tapto::VncSession session;

        // Comma-separated, e.g. --encodings raw. Restricting the set isolates a
        // single decoder, which is how a rendering question gets answered by
        // experiment rather than by reading the decoder and hoping.
        if (!encodings.empty()) {
            std::vector<std::string> names;
            std::stringstream parts(encodings);
            std::string name;
            while (std::getline(parts, name, ',')) {
                if (!name.empty()) names.push_back(name);
            }
            session.setEncodings(names);
        }

        if (vmwareMode) {
            const tapto::ConsoleTicket ticket =
                tapto::acquireConsoleTicket(vcenter, vmName);
            std::cout << "Console ticket acquired for '" << vmName << "'; connecting to "
                      << ticket.host << ":" << ticket.port << " ...\n";
            // The ticket is short-lived, so connect immediately after acquiring.
            session.connectWebSocket(ticket.websocketUrl());
        } else {
            std::cout << "Connecting to " << options.host << ":" << (5900 + options.display)
                      << " ...\n";
            session.connect(options);
        }

        std::cout << "Connected.\n"
                  << "  desktop: " << session.desktopName() << "\n"
                  << "  size:    " << session.width() << "x" << session.height() << "\n";

        if (wake) {
            // Shift is the conventional "wake without side effects" key: it
            // dismisses a blank screen saver but types nothing and activates
            // nothing on its own.
            constexpr uint32_t kShiftLeft = 0xFFE1;  // X11 keysym
            session.sendKey(kShiftLeft, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            session.sendKey(kShiftLeft, false);
            // Give the guest time to redraw before asking for the screen.
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            session.pump(std::chrono::milliseconds(200));
            std::cout << "Sent wake keystroke.\n";
        }

        const bool got = session.capture(std::chrono::milliseconds(timeoutMs),
                                         std::chrono::milliseconds(settleMs));
        if (!got) {
            std::cerr << "ERROR: no framebuffer data arrived within " << timeoutMs << "ms\n";
            return 1;
        }

        if (verbose) std::cout << "\n" << session.diagnostics() << "\n";

        if (gridStep > 0) {
            // The whole screen at 1:1, ruled — the same render the agent's
            // screenshots get under --grid, so this is a faithful preview.
            tapto::VncSession::Rect whole;
            const std::vector<uint8_t> png =
                session.screenshotRegionPng(whole, 1, nullptr, true, gridStep);
            FILE* file = png.empty() ? nullptr : std::fopen(out.c_str(), "wb");
            if (!file) {
                std::cerr << "ERROR: failed to write " << out << "\n";
                return 1;
            }
            std::fwrite(png.data(), 1, png.size(), file);
            std::fclose(file);
        } else if (!session.writePng(out)) {
            std::cerr << "ERROR: failed to write " << out << "\n";
            return 1;
        }

        std::cout << "Wrote " << out << " (" << session.width() << "x" << session.height() << ")";
        if (gridStep > 0) std::cout << ", gridded every " << gridStep << "px";
        std::cout << "\n";

        if (!zoom.empty()) {
            tapto::VncSession::Rect region;
            std::stringstream parts(zoom);
            std::string field;
            int values[4] = {0, 0, 0, 0};
            int count = 0;
            while (count < 4 && std::getline(parts, field, ',')) {
                values[count++] = std::atoi(field.c_str());
            }
            if (count != 4) {
                std::cerr << "ERROR: --zoom expects x,y,width,height\n";
                return 2;
            }
            region.x = values[0];
            region.y = values[1];
            region.width = values[2];
            region.height = values[3];

            // Same scale rule the vnc_zoom tool applies, so what lands on disk
            // is what the model would be shown. Keep these two in step: an
            // earlier mismatch here rendered at 1x what the tool renders at 2x,
            // which makes this a misleading way to check the tool.
            const int longest = std::max(region.width, region.height);
            int scale = longest > 0 ? (896 + longest / 2) / longest : 2;
            scale = scale < 2 ? 2 : (scale > 4 ? 4 : scale);

            const std::vector<uint8_t> png =
                session.screenshotRegionPng(region, scale, nullptr, true);
            if (png.empty()) {
                std::cerr << "ERROR: could not render that region\n";
                return 1;
            }
            const std::string zoomPath = out + ".zoom.png";
            FILE* file = std::fopen(zoomPath.c_str(), "wb");
            if (!file) {
                std::cerr << "ERROR: failed to write " << zoomPath << "\n";
                return 1;
            }
            std::fwrite(png.data(), 1, png.size(), file);
            std::fclose(file);

            std::cout << "Wrote " << zoomPath << " (region " << region.x << "," << region.y
                      << " " << region.width << "x" << region.height << " at " << scale
                      << "x = " << region.width * scale << "x" << region.height * scale
                      << ")\n";
        }

        session.disconnect();
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
