// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/vnc_session.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "rfb_client.hpp"
#include "rfb_pixel_format.hpp"
#include "tapto/log.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace tapto {
namespace {

using Clock = std::chrono::steady_clock;

// Poll interval while waiting for the server. Small enough that `settle`
// windows of a few hundred ms stay accurate, large enough not to spin.
constexpr auto kPollInterval = std::chrono::milliseconds(5);

// Requested in preference order; the server picks the first it supports.
// Ordered most- to least-compressed, matching the cpp-vnc client example.
const std::vector<rfb::S32> kEncodings = {
    static_cast<rfb::S32>(rfb::EncodingType::ZRLE),
    static_cast<rfb::S32>(rfb::EncodingType::TRLE),
    static_cast<rfb::S32>(rfb::EncodingType::Hextile),
    static_cast<rfb::S32>(rfb::EncodingType::RRE),
    static_cast<rfb::S32>(rfb::EncodingType::CopyRect),
    static_cast<rfb::S32>(rfb::EncodingType::Raw),
};

// Scales one channel from the server's range (redMax et al) to 8 bits.
inline uint8_t scaleChannel(rfb::U16 value, rfb::U16 max) {
    if (max == 0) return 0;
    if (max == 255) return static_cast<uint8_t>(value);
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255u + max / 2) / max);
}

}  // namespace

struct VncSession::Impl {
    // Created per connect(), because the transport kind is fixed at
    // construction time by rfb::Client's constructor.
    std::unique_ptr<rfb::Client> client;
    rfb::ServerInit info{};
    std::string     name;

    // Composite screen, one entry per pixel, still in the server's pixel
    // format. Converting to RGB is deferred to screenshot time so that
    // applying a rectangle stays a plain copy.
    std::vector<rfb::U32> pixels;
    int width  = 0;
    int height = 0;

    // Which pixels the server has actually sent at least once. A server may
    // answer a full-screen request with only part of the screen, so "an update
    // arrived" is not the same as "we have a complete picture".
    std::vector<bool> covered;
    size_t coveredCount = 0;

    bool fullyCovered() const { return coveredCount >= covered.size(); }
    void markCovered(size_t index) {
        if (!covered[index]) {
            covered[index] = true;
            ++coveredCount;
        }
    }

    bool connected     = false;
    int  pendingUpdates = 0;   // framebuffer updates seen since last checked
    std::vector<rfb::S32> encodings;  // empty = offer the full default set

    // When the server was last heard from or spoken to. A console that dies
    // does it quietly and while nothing is being asked of it — the agent spends
    // most of a run waiting on the model — so the length of the silence before
    // a socket turns out to be dead is the one measurement that separates an
    // idle timeout from a network fault or a stolen console.
    Clock::time_point lastActivity{};
    void touch() { lastActivity = Clock::now(); }

    // Why the connection ended, recorded as it ends.
    //
    // rfb::Client::processMessage() catches every exception, hands the reason
    // to onError, and then disconnects. Leaving that callback unset throws the
    // reason away, and the only evidence remaining is "Socket is not connected"
    // from whatever touches the socket next — the symptom, several seconds
    // after the fault, with nothing to say whether it was a decoder, an
    // unexpected message type or the peer simply going away.
    std::string lastError;

    // Diagnostics. An update message arriving tells us nothing about whether
    // it carried pixels, so count what actually reached the composite.
    struct Stats {
        int    updates = 0;
        int    rects = 0;
        int    rectsCopyRect = 0;
        int    rectsDecoded = 0;   // had decodedPixels to copy
        int    rectsEmpty = 0;     // carried no pixels at all
        int    rectsPseudo = 0;    // negative encoding type, skipped
        size_t pixelsWritten = 0;
        size_t nonZeroWritten = 0;
        std::map<rfb::S32, int> encodings;
    } stats;

    void applyRectangle(const rfb::Rectangle& rect);
    void applyUpdate(const rfb::FramebufferUpdateMsg& msg);

    // Builds the client and wires the framebuffer callback.
    void openTransport(rfb::TransportType transport);
    // Post-handshake setup shared by both connect paths.
    void finishHandshake();
};

void VncSession::Impl::applyRectangle(const rfb::Rectangle& rect) {
    ++stats.rects;
    ++stats.encodings[rect.encodingType];

    // Pseudo-encodings (cursor, desktop size) carry no screen content and use
    // negative type codes. We never request them, but a server may send one
    // anyway; ignoring them keeps the composite intact.
    if (rect.encodingType < 0) {
        ++stats.rectsPseudo;
        return;
    }

    if (rect.encodingType == static_cast<rfb::S32>(rfb::EncodingType::CopyRect)) {
        ++stats.rectsCopyRect;
        if (rect.pixelData.size() < 4) return;
        const rfb::U16 srcX = static_cast<rfb::U16>((rect.pixelData[0] << 8) | rect.pixelData[1]);
        const rfb::U16 srcY = static_cast<rfb::U16>((rect.pixelData[2] << 8) | rect.pixelData[3]);

        // Copy row-wise, choosing a direction that tolerates overlap between
        // source and destination (scrolling makes this the common case).
        const bool downward = rect.yPosition > srcY;
        for (rfb::U16 row = 0; row < rect.height; ++row) {
            const rfb::U16 r = downward ? static_cast<rfb::U16>(rect.height - 1 - row) : row;
            const int sy = srcY + r;
            const int dy = rect.yPosition + r;
            if (sy < 0 || sy >= height || dy < 0 || dy >= height) continue;

            const bool rightward = rect.xPosition > srcX;
            for (rfb::U16 col = 0; col < rect.width; ++col) {
                const rfb::U16 c = rightward ? static_cast<rfb::U16>(rect.width - 1 - col) : col;
                const int sx = srcX + c;
                const int dx = rect.xPosition + c;
                if (sx < 0 || sx >= width || dx < 0 || dx >= width) continue;
                const size_t dst = static_cast<size_t>(dy) * width + dx;
                pixels[dst] = pixels[static_cast<size_t>(sy) * width + sx];
                markCovered(dst);
            }
        }
        return;
    }

    if (rect.decodedPixels.empty()) {
        ++stats.rectsEmpty;
        return;
    }
    ++stats.rectsDecoded;

    for (rfb::U16 y = 0; y < rect.height; ++y) {
        const int dy = rect.yPosition + y;
        if (dy < 0 || dy >= height) continue;
        for (rfb::U16 x = 0; x < rect.width; ++x) {
            const int dx = rect.xPosition + x;
            if (dx < 0 || dx >= width) continue;
            const size_t src = static_cast<size_t>(y) * rect.width + x;
            if (src >= rect.decodedPixels.size()) continue;
            const rfb::U32 value = rect.decodedPixels[src];
            const size_t dst = static_cast<size_t>(dy) * width + dx;
            pixels[dst] = value;
            markCovered(dst);
            ++stats.pixelsWritten;
            if (value != 0) ++stats.nonZeroWritten;
        }
    }
}

void VncSession::Impl::applyUpdate(const rfb::FramebufferUpdateMsg& msg) {
    ++stats.updates;
    for (const auto& rect : msg.rectangles) {
        applyRectangle(rect);
    }
    ++pendingUpdates;
    touch();
}

void VncSession::Impl::openTransport(rfb::TransportType transport) {
    client.reset(new rfb::Client(transport));

    rfb::ClientCallbacks callbacks;
    callbacks.onFramebufferUpdate = [this](const rfb::FramebufferUpdateMsg& msg) {
        applyUpdate(msg);
    };
    // Not all of these are fatal — an undecodable rectangle is reported and
    // skipped — so they are logged rather than thrown. The one that matters is
    // the last one before the transport closes.
    callbacks.onError = [this](const std::string& message) {
        lastError = message;
        mclog("RFB error: " + message + "\n");
    };
    callbacks.onDisconnected = [this]() {
        mclog("RFB transport closed" +
              (lastError.empty() ? std::string(", peer or local shutdown")
                                 : ", last error: " + lastError) + "\n");
    };
    client->setCallbacks(callbacks);
}

void VncSession::Impl::finishHandshake() {
    info = client->serverInfo();
    name = info.name;

    // We render by extracting channels from the server's own format. A
    // palette-based server would need SetColorMapEntries tracking, which we
    // don't do — fail loudly rather than hand the model a black screen.
    if (!info.pixelFormat.trueColorFlag) {
        client->disconnect();
        throw std::runtime_error(
            "VncSession: server uses a palette pixel format; only true-color is supported");
    }

    width  = info.framebufferWidth;
    height = info.framebufferHeight;
    if (width <= 0 || height <= 0) {
        client->disconnect();
        throw std::runtime_error("VncSession: server reported an empty framebuffer");
    }

    // Opaque black, so a screenshot taken before the first update is at least
    // well-formed rather than uninitialized memory.
    pixels.assign(static_cast<size_t>(width) * height, 0u);
    covered.assign(pixels.size(), false);
    coveredCount = 0;

    // Deliberately no setPixelFormat() call: rfb::Client sends the message but
    // keeps decoding against serverInit.pixelFormat, so requesting a format
    // would desynchronize the decoder from the wire.
    client->setEncodings(encodings.empty() ? kEncodings : encodings);

    connected = true;
    pendingUpdates = 0;
    lastError.clear();
    // A completed handshake is traffic. Without this a reconnect that then
    // receives nothing would still report the *previous* silence, which is the
    // one number this is all being measured for.
    touch();
}

VncSession::VncSession() : m_impl(new Impl()) {}

VncSession::~VncSession() {
    if (m_impl && m_impl->connected && m_impl->client) {
        try {
            m_impl->client->disconnect();
        } catch (...) {
            // Destructors don't throw; a failed close is not actionable here.
        }
    }
}

void VncSession::connect(const VncOptions& options) {
    Impl& impl = *m_impl;
    if (impl.connected) throw std::runtime_error("VncSession: already connected");

    impl.openTransport(rfb::TransportType::TCP);
    impl.client->connect(options.host, options.display, options.password);
    impl.finishHandshake();
}

void VncSession::connectWebSocket(const std::string& url, const std::string& password) {
    Impl& impl = *m_impl;
    if (impl.connected) throw std::runtime_error("VncSession: already connected");
    if (url.empty()) throw std::runtime_error("VncSession: empty WebSocket URL");

    // A stray control character here silently corrupts the HTTP upgrade
    // request, because WebSocketTransport::parseUrl treats everything after
    // the host as the request path. The prototype's URL builder appended a
    // newline, which is exactly this failure.
    if (url.find_first_of("\r\n \t") != std::string::npos) {
        throw std::runtime_error("VncSession: WebSocket URL contains whitespace");
    }

    impl.openTransport(rfb::TransportType::WEBSOCKET);
    // Port 0 matters: WebSocketTransport overrides the port parsed from the
    // URL with this argument whenever it is non-zero.
    impl.client->connect(url, 0, password);
    impl.finishHandshake();
}

void VncSession::setEncodings(const std::vector<std::string>& names) {
    static const std::map<std::string, rfb::EncodingType> kByName = {
        {"raw",      rfb::EncodingType::Raw},
        {"copyrect", rfb::EncodingType::CopyRect},
        {"rre",      rfb::EncodingType::RRE},
        {"hextile",  rfb::EncodingType::Hextile},
        {"trle",     rfb::EncodingType::TRLE},
        {"zrle",     rfb::EncodingType::ZRLE},
    };

    std::vector<rfb::S32> resolved;
    for (const std::string& name : names) {
        const auto it = kByName.find(name);
        if (it == kByName.end()) {
            throw std::runtime_error("VncSession: unknown encoding '" + name + "'");
        }
        resolved.push_back(static_cast<rfb::S32>(it->second));
    }
    m_impl->encodings = std::move(resolved);
}

void VncSession::disconnect() {
    Impl& impl = *m_impl;
    if (!impl.connected) return;
    impl.client->disconnect();
    impl.connected = false;
}

bool VncSession::isConnected() const {
    return m_impl->connected && m_impl->client && m_impl->client->isConnected();
}

int VncSession::width() const { return m_impl->width; }
int VncSession::height() const { return m_impl->height; }
const std::string& VncSession::desktopName() const { return m_impl->name; }

int VncSession::pump(std::chrono::milliseconds budget) {
    Impl& impl = *m_impl;
    if (!impl.connected) return 0;

    impl.pendingUpdates = 0;
    const auto deadline = Clock::now() + budget;
    do {
        while (impl.client->processMessage()) {
            if (Clock::now() >= deadline) break;
        }
        if (Clock::now() >= deadline) break;
        std::this_thread::sleep_for(kPollInterval);
    } while (Clock::now() < deadline);

    return impl.pendingUpdates;
}

bool VncSession::capture(std::chrono::milliseconds timeout,
                         std::chrono::milliseconds settle) {
    Impl& impl = *m_impl;
    if (!impl.connected) throw std::runtime_error("VncSession: not connected");

    const auto w = static_cast<rfb::U16>(impl.width);
    const auto h = static_cast<rfb::U16>(impl.height);

    // Bounds the re-request loop below so a server that simply will not send
    // the rest of the screen cannot keep us spinning until `timeout`.
    constexpr int kMaxFullRequests = 8;

    impl.pendingUpdates = 0;
    int fullRequests = 1;
    impl.client->requestFramebufferUpdate(0, 0, w, h, false);

    const auto deadline = Clock::now() + timeout;
    auto lastChange = Clock::now();
    int  updates = 0;

    while (Clock::now() < deadline && impl.client->isConnected()) {
        if (impl.client->processMessage()) {
            if (impl.pendingUpdates > 0) {
                updates += impl.pendingUpdates;
                impl.pendingUpdates = 0;
                lastChange = Clock::now();

                // One request yields one update, and that update may cover
                // only part of the screen. Keep asking non-incrementally
                // until every pixel has arrived at least once; only then
                // switch to incremental, which reports just what changes.
                if (!impl.fullyCovered() && fullRequests < kMaxFullRequests) {
                    ++fullRequests;
                    impl.client->requestFramebufferUpdate(0, 0, w, h, false);
                } else {
                    impl.client->requestFramebufferUpdate(0, 0, w, h, true);
                }
            }
            continue;
        }

        if (updates > 0 && Clock::now() - lastChange >= settle) {
            // Settled. If the picture is still incomplete, the server is
            // waiting to be asked again rather than pushing the remainder.
            if (impl.fullyCovered() || fullRequests >= kMaxFullRequests) break;
            ++fullRequests;
            impl.client->requestFramebufferUpdate(0, 0, w, h, false);
            lastChange = Clock::now();
            continue;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    return updates > 0;
}

namespace {

// A 5x7 bitmap for the digits, one byte per row, low 5 bits used. Rulers need
// to print numbers and there is no font anywhere in this project; ten glyphs
// is the whole requirement, so this is cheaper than taking on a dependency.
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = 6;
constexpr uint8_t kDigitGlyphs[10][kGlyphHeight] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

void setPixel(std::vector<uint8_t>& rgb, int width, int height, int x, int y,
              uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const size_t index = (static_cast<size_t>(y) * width + x) * 3;
    rgb[index + 0] = r;
    rgb[index + 1] = g;
    rgb[index + 2] = b;
}

int numberWidth(int value, int glyphScale) {
    int digits = 1;
    for (int v = value / 10; v > 0; v /= 10) ++digits;
    return digits * kGlyphAdvance * glyphScale;
}

// Draws `value` with its top-left at (x,y). Digits only; values are pixel
// coordinates, so there is never a sign or a decimal point.
void drawNumber(std::vector<uint8_t>& rgb, int width, int height, int x, int y,
                int value, int glyphScale) {
    int digits[10];
    int count = 0;
    do {
        digits[count++] = value % 10;
        value /= 10;
    } while (value > 0 && count < 10);

    for (int i = 0; i < count; ++i) {
        const uint8_t* glyph = kDigitGlyphs[digits[count - 1 - i]];
        const int originX = x + i * kGlyphAdvance * glyphScale;
        for (int row = 0; row < kGlyphHeight; ++row) {
            for (int col = 0; col < kGlyphWidth; ++col) {
                if (!(glyph[row] & (1u << (kGlyphWidth - 1 - col)))) continue;
                for (int sy = 0; sy < glyphScale; ++sy) {
                    for (int sx = 0; sx < glyphScale; ++sx) {
                        setPixel(rgb, width, height,
                                 originX + col * glyphScale + sx,
                                 y + row * glyphScale + sy, 0, 0, 0);
                    }
                }
            }
        }
    }
}

// Tick spacing in screen pixels: the smallest round step that keeps the number
// of divisions readable.
int niceStep(int span) {
    static const int candidates[] = {5, 10, 20, 25, 50, 100, 200, 250, 500};
    for (int step : candidates) {
        if (span / step <= 10) return step;
    }
    return 1000;
}

// Blends a gridline colour halfway into the pixel rather than replacing it, so
// a line crossing text leaves the text readable underneath.
// `weight` is in quarters: 2 blends half and half, 1 leaves a fainter line for
// the unlabelled subdivisions.
void blendPixel(std::vector<uint8_t>& rgb, int width, int height, int x, int y,
                uint8_t r, uint8_t g, uint8_t b, int weight) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    const size_t index = (static_cast<size_t>(y) * width + x) * 3;
    const int keep = 4 - weight;
    rgb[index + 0] = static_cast<uint8_t>((rgb[index + 0] * keep + r * weight) / 4);
    rgb[index + 1] = static_cast<uint8_t>((rgb[index + 1] * keep + g * weight) / 4);
    rgb[index + 2] = static_cast<uint8_t>((rgb[index + 2] * keep + b * weight) / 4);
}

// Draws a filled dot with a light ring around it, clipped to the image. The
// ring is what keeps it findable on a red or dark background — a plain red dot
// disappears into exactly the places you most want to see it.
void drawDot(std::vector<uint8_t>& rgb, int width, int height, int cx, int cy, int radius) {
    if (radius < 1) radius = 1;
    const int outer = radius + 2;
    for (int dy = -outer; dy <= outer; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= height) continue;
        for (int dx = -outer; dx <= outer; ++dx) {
            const int x = cx + dx;
            if (x < 0 || x >= width) continue;
            const int distance = dx * dx + dy * dy;
            if (distance > outer * outer) continue;

            const size_t index = (static_cast<size_t>(y) * width + x) * 3;
            const bool core = distance <= radius * radius;
            rgb[index + 0] = 255;
            rgb[index + 1] = core ? 0 : 255;
            rgb[index + 2] = core ? 0 : 255;
        }
    }
}

}  // namespace

std::vector<uint8_t> VncSession::screenshotRegionPng(Rect& region, int scale,
                                                     const Marker* marker,
                                                     bool rulers, int gridStep) const {
    const Impl& impl = *m_impl;
    if (impl.width <= 0 || impl.height <= 0) return {};

    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;

    // Clamp rather than reject. A region that runs off an edge is a near miss,
    // and the overlapping part is more useful to a caller than an error.
    if (region.width  <= 0) region.width  = impl.width;
    if (region.height <= 0) region.height = impl.height;
    region.x = region.x < 0 ? 0 : (region.x > impl.width  - 1 ? impl.width  - 1 : region.x);
    region.y = region.y < 0 ? 0 : (region.y > impl.height - 1 ? impl.height - 1 : region.y);
    region.width  = std::min(region.width,  impl.width  - region.x);
    region.height = std::min(region.height, impl.height - region.y);
    if (region.width <= 0 || region.height <= 0) return {};

    const int glyphScale = rulerGlyphScale(scale);
    // The rulers sit in a margin rather than on the picture, so nothing they
    // draw can hide content. rulerMargin*() publishes the size so a caller can
    // state the image-to-screen mapping; these assert the font metrics it
    // assumes are still the font's.
    static_assert(kGlyphAdvance == 6, "rulerMarginLeft() assumes a 6px advance");
    static_assert(kGlyphHeight == 7, "rulerMarginTop() assumes a 7px glyph");
    const int marginLeft = rulers ? rulerMarginLeft(scale) : 0;
    const int marginTop  = rulers ? rulerMarginTop(scale) : 0;

    const rfb::PixelFormat& fmt = impl.info.pixelFormat;
    const int contentWidth  = region.width  * scale;
    const int contentHeight = region.height * scale;
    const int outWidth  = marginLeft + contentWidth;
    const int outHeight = marginTop + contentHeight;
    std::vector<uint8_t> rgb(static_cast<size_t>(outWidth) * outHeight * 3,
                             rulers ? 235 : 0);

    for (int row = 0; row < region.height; ++row) {
        const size_t sourceRow = static_cast<size_t>(region.y + row) * impl.width;
        for (int col = 0; col < region.width; ++col) {
            const size_t index = sourceRow + static_cast<size_t>(region.x + col);
            if (index >= impl.pixels.size()) continue;

            const rfb::U32 pixel = impl.pixels[index];
            const uint8_t r = scaleChannel(rfb::extractRed(pixel, fmt), fmt.redMax);
            const uint8_t g = scaleChannel(rfb::extractGreen(pixel, fmt), fmt.greenMax);
            const uint8_t b = scaleChannel(rfb::extractBlue(pixel, fmt), fmt.blueMax);

            // Replicate the source pixel into a scale x scale block.
            for (int dy = 0; dy < scale; ++dy) {
                size_t out = (static_cast<size_t>(marginTop + row * scale + dy) * outWidth +
                              static_cast<size_t>(marginLeft + col * scale)) * 3;
                for (int dx = 0; dx < scale; ++dx) {
                    rgb[out++] = r;
                    rgb[out++] = g;
                    rgb[out++] = b;
                }
            }
        }
    }

    if (rulers) {
        constexpr uint8_t kGridR = 255, kGridG = 0, kGridB = 255;  // magenta
        // An explicit step applies to both axes, which makes the equal-
        // resolution rule below hold by construction and leaves nothing for
        // the subdivisions to add.
        const int stepX = gridStep > 0 ? gridStep : niceStep(region.width);
        const int stepY = gridStep > 0 ? gridStep : niceStep(region.height);

        // Skip every other label where ticks fall too close to print one.
        // Measured against the label itself rather than a fixed number of
        // pixels, since the digits are bigger on an unenlarged image.
        const int labelWidth = kGlyphAdvance * glyphScale * 4;  // four digits
        const int labelEveryX = stepX * scale < labelWidth + 8 ? 2 : 1;
        const int labelEveryY = stepY * scale < kGlyphHeight * glyphScale + 8 ? 2 : 1;

        // Subdivisions, at the finer of the two axes' steps and applied to
        // both. Without these a wide, short region gets a grid several times
        // coarser horizontally than vertically — and a position can only be
        // read as precisely as the nearest line, so the coarse axis is the one
        // that misses. Measured: a checkbox in a 400x100 region, gridded every
        // 10px vertically and 50px horizontally, was located to the exact
        // pixel in y and missed by 75-103px in x, twice.
        const int minorStep = gridStep > 0
                                  ? gridStep
                                  : niceStep(std::min(region.width, region.height));

        if (minorStep < stepX) {
            for (int sx = ((region.x + minorStep - 1) / minorStep) * minorStep;
                 sx < region.x + region.width; sx += minorStep) {
                if (sx % stepX == 0) continue;  // a major line covers this one
                const int cx = marginLeft + (sx - region.x) * scale;
                for (int y = marginTop; y < outHeight; ++y) {
                    blendPixel(rgb, outWidth, outHeight, cx, y, kGridR, kGridG, kGridB, 1);
                }
                for (int y = marginTop - 2; y < marginTop; ++y) {
                    setPixel(rgb, outWidth, outHeight, cx, y, 150, 150, 150);
                }
            }
        }
        if (minorStep < stepY) {
            for (int sy = ((region.y + minorStep - 1) / minorStep) * minorStep;
                 sy < region.y + region.height; sy += minorStep) {
                if (sy % stepY == 0) continue;
                const int cy = marginTop + (sy - region.y) * scale;
                for (int x = marginLeft; x < outWidth; ++x) {
                    blendPixel(rgb, outWidth, outHeight, x, cy, kGridR, kGridG, kGridB, 1);
                }
                for (int x = marginLeft - 2; x < marginLeft; ++x) {
                    setPixel(rgb, outWidth, outHeight, x, cy, 150, 150, 150);
                }
            }
        }

        // A line is drawn strongly only if it carries a number. Where ticks fall
        // too close together to label every one, the unlabelled ones would
        // otherwise be indistinguishable from their neighbours, and counting
        // gridlines to find "the one after 300" is exactly how a reading ends
        // up one whole step out.
        for (int sx = ((region.x + stepX - 1) / stepX) * stepX;
             sx < region.x + region.width; sx += stepX) {
            const int cx = marginLeft + (sx - region.x) * scale;
            const bool labelled = (sx / stepX) % labelEveryX == 0;
            for (int y = marginTop; y < outHeight; ++y) {
                blendPixel(rgb, outWidth, outHeight, cx, y,
                           kGridR, kGridG, kGridB, labelled ? 2 : 1);
            }
            for (int y = marginTop - (labelled ? 4 : 2); y < marginTop; ++y) {
                setPixel(rgb, outWidth, outHeight, cx, y, 90, 90, 90);
            }
            if (labelled) {
                // Clamped to the content edge, not to 0: a label centred on the
                // leftmost gridline would otherwise print over the left ruler.
                int labelX = cx - numberWidth(sx, glyphScale) / 2;
                if (labelX < marginLeft) labelX = marginLeft;
                drawNumber(rgb, outWidth, outHeight, labelX, 2, sx, glyphScale);
            }
        }

        for (int sy = ((region.y + stepY - 1) / stepY) * stepY;
             sy < region.y + region.height; sy += stepY) {
            const int cy = marginTop + (sy - region.y) * scale;
            const bool labelled = (sy / stepY) % labelEveryY == 0;
            for (int x = marginLeft; x < outWidth; ++x) {
                blendPixel(rgb, outWidth, outHeight, x, cy,
                           kGridR, kGridG, kGridB, labelled ? 2 : 1);
            }
            for (int x = marginLeft - (labelled ? 4 : 2); x < marginLeft; ++x) {
                setPixel(rgb, outWidth, outHeight, x, cy, 90, 90, 90);
            }
            if (labelled) {
                drawNumber(rgb, outWidth, outHeight,
                           marginLeft - numberWidth(sy, glyphScale) - 5,
                           cy - kGlyphHeight * glyphScale / 2, sy, glyphScale);
            }
        }
    }

    if (marker) {
        // Centre of the scale x scale block the marked framebuffer pixel
        // became, so the dot sits on the pixel rather than at its corner.
        const int cx = marginLeft + (marker->x - region.x) * scale + scale / 2;
        const int cy = marginTop  + (marker->y - region.y) * scale + scale / 2;
        drawDot(rgb, outWidth, outHeight, cx, cy, marker->radius);
    }

    std::vector<uint8_t> png;
    auto sink = [](void* context, void* data, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<const uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    };

    if (!stbi_write_png_to_func(sink, &png, outWidth, outHeight, 3,
                                rgb.data(), outWidth * 3)) {
        return {};
    }
    return png;
}

std::vector<uint8_t> VncSession::screenshotPng() const {
    Rect whole;  // zero width/height means the full framebuffer
    return screenshotRegionPng(whole, 1);
}

std::string VncSession::diagnostics() const {
    const Impl& impl = *m_impl;
    const rfb::PixelFormat& fmt = impl.info.pixelFormat;
    const Impl::Stats& s = impl.stats;

    auto encodingName = [](rfb::S32 type) -> std::string {
        switch (static_cast<rfb::EncodingType>(type)) {
            case rfb::EncodingType::Raw:               return "Raw";
            case rfb::EncodingType::CopyRect:          return "CopyRect";
            case rfb::EncodingType::RRE:               return "RRE";
            case rfb::EncodingType::Hextile:           return "Hextile";
            case rfb::EncodingType::TRLE:              return "TRLE";
            case rfb::EncodingType::ZRLE:              return "ZRLE";
            case rfb::EncodingType::CursorPseudo:      return "Cursor(pseudo)";
            case rfb::EncodingType::DesktopSizePseudo: return "DesktopSize(pseudo)";
        }
        return "unknown(" + std::to_string(type) + ")";
    };

    std::ostringstream out;
    out << "pixel format: " << static_cast<int>(fmt.bitsPerPixel) << "bpp"
        << " depth=" << static_cast<int>(fmt.depth)
        << " trueColor=" << (fmt.trueColorFlag ? "yes" : "no")
        << " bigEndian=" << (fmt.bigEndianFlag ? "yes" : "no") << "\n"
        << "  red   max=" << fmt.redMax   << " shift=" << static_cast<int>(fmt.redShift)   << "\n"
        << "  green max=" << fmt.greenMax << " shift=" << static_cast<int>(fmt.greenShift) << "\n"
        << "  blue  max=" << fmt.blueMax  << " shift=" << static_cast<int>(fmt.blueShift)  << "\n"
        << "updates: " << s.updates << "  rectangles: " << s.rects
        << " (decoded=" << s.rectsDecoded
        << " copyrect=" << s.rectsCopyRect
        << " empty=" << s.rectsEmpty
        << " pseudo=" << s.rectsPseudo << ")\n"
        << "pixels written: " << s.pixelsWritten
        << "  non-zero: " << s.nonZeroWritten << "\n";

    const size_t total = impl.covered.size();
    const double percent = total ? (100.0 * impl.coveredCount / total) : 0.0;
    out << "screen coverage: " << impl.coveredCount << "/" << total
        << " (" << static_cast<int>(percent + 0.5) << "%)"
        << (impl.coveredCount >= total ? "" : "  <-- INCOMPLETE") << "\n";

    out << "encodings seen:";
    if (s.encodings.empty()) {
        out << " (none)";
    } else {
        for (const auto& entry : s.encodings) {
            out << " " << encodingName(entry.first) << "x" << entry.second;
        }
    }
    out << "\n";
    return out.str();
}

bool VncSession::writePng(const std::string& path) const {
    const std::vector<uint8_t> png = screenshotPng();
    if (png.empty()) return false;

    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const size_t written = std::fwrite(png.data(), 1, png.size(), file);
    const bool ok = (std::fclose(file) == 0) && (written == png.size());
    return ok;
}

void VncSession::sendPointer(uint8_t buttonMask, uint16_t x, uint16_t y) {
    if (!m_impl->connected) throw std::runtime_error("VncSession: not connected");
    m_impl->client->sendPointerEvent(buttonMask, x, y);
    m_impl->touch();
}

void VncSession::sendKey(uint32_t keysym, bool down) {
    if (!m_impl->connected) throw std::runtime_error("VncSession: not connected");
    m_impl->client->sendKeyEvent(keysym, down);
    m_impl->touch();
}

const std::string& VncSession::lastError() const { return m_impl->lastError; }

int VncSession::idleSeconds() const {
    const Impl& impl = *m_impl;
    if (impl.lastActivity == Clock::time_point{}) return 0;
    const auto gap = Clock::now() - impl.lastActivity;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(gap).count());
}

}  // namespace tapto
