// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tapto {

// RFB pointer button-mask bits. Wheel "buttons" are press+release pairs.
enum MouseButton : uint8_t {
    kMouseLeft      = 1u << 0,
    kMouseMiddle    = 1u << 1,
    kMouseRight     = 1u << 2,
    kMouseWheelUp   = 1u << 3,
    kMouseWheelDown = 1u << 4,
};

struct VncOptions {
    std::string host = "localhost";
    uint16_t    display = 0;      // port is 5900 + display
    std::string password;         // empty selects the None security type
};

// A live VNC connection plus the accumulated screen contents.
//
// The RFB protocol streams *rectangles*, not frames: the server answers each
// FramebufferUpdateRequest with the regions that changed. This class keeps the
// running composite so a screenshot can be taken at any moment, which is what
// the model needs — it sees whole screens, never deltas.
//
// Not thread-safe; drive it from one thread.
class VncSession {
public:
    VncSession();
    ~VncSession();

    VncSession(const VncSession&) = delete;
    VncSession& operator=(const VncSession&) = delete;

    // Connects over TCP to 5900+display, handshakes, and negotiates encodings.
    // Throws std::runtime_error on failure, including servers whose pixel
    // format we cannot render.
    void connect(const VncOptions& options);

    // Connects to an RFB stream tunneled over a WebSocket, given a full
    // ws:// or wss:// URL — this is how VMware WebMKS consoles are reached
    // (see tapto/vmware_console.h). Everything after the handshake is
    // ordinary RFB, so the rest of this class is transport-agnostic.
    void connectWebSocket(const std::string& url, const std::string& password = "");

    // Restricts which encodings are offered to the server. Call before
    // connect(); an empty list means "offer everything we support".
    //
    // Mainly a diagnostic lever: forcing "raw" removes every decoder from the
    // path except the trivial one, which is how you tell a decoding bug apart
    // from something the guest genuinely drew that way.
    // Names: raw, copyrect, rre, hextile, trle, zrle.
    void setEncodings(const std::vector<std::string>& names);

    void disconnect();
    bool isConnected() const;

    // Seconds since the last exchange with the server, in either direction.
    //
    // Worth having because a console dies quietly and while nothing is being
    // asked of it: the agent spends most of a run waiting on the model, so by
    // the time a socket is found dead the interesting question is how long the
    // silence before it was. A consistent interval across failures names an
    // idle timeout; a scattered one means something else broke. Zero before
    // the first exchange.
    int idleSeconds() const;

    int                width() const;
    int                height() const;
    const std::string& desktopName() const;

    // Requests the full screen, then pumps messages until the picture stops
    // changing for `settle` or `timeout` elapses. Returns false if the server
    // sent no framebuffer data at all.
    //
    // `settle` exists because a click often triggers animation; sampling the
    // screen too eagerly captures a half-drawn menu.
    bool capture(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000),
                 std::chrono::milliseconds settle  = std::chrono::milliseconds(300));

    // Applies whatever has already arrived without requesting more.
    // Returns the number of framebuffer updates applied.
    int pump(std::chrono::milliseconds budget = std::chrono::milliseconds(0));

    // PNG-encodes the current composite. Returns empty on failure.
    std::vector<uint8_t> screenshotPng() const;
    bool                 writePng(const std::string& path) const;

    // A rectangle in framebuffer pixels.
    struct Rect {
        int x = 0, y = 0, width = 0, height = 0;
    };

    // A dot drawn into a rendered screenshot, marking a point of interest —
    // in practice, where a click was aimed. Purely an annotation on the
    // output; the framebuffer itself is never modified.
    struct Marker {
        int x = 0, y = 0;    // framebuffer pixels
        int radius = 5;      // in output pixels, before the ring around it
    };

    // PNG-encodes a sub-rectangle of the composite, enlarged by an integer
    // factor.
    //
    // Enlarging is nearest-neighbour, not interpolation: it invents no
    // colours, so a magnified view shows exactly the pixels the guest drew.
    // That matters when the point of looking closely is to judge what is
    // actually on screen.
    //
    // `region` is clamped to the framebuffer and written back, so the caller
    // learns what it actually got; a zero width or height means "the whole
    // screen in that axis".
    //
    // A non-null `marker` draws a dot at that framebuffer position, clipped to
    // the region like anything else. Pass null for an unannotated render.
    //
    // `rulers` adds a numbered scale down the left edge and across the top,
    // plus a grid over the content, all labelled in **screen** coordinates.
    // The rulers live in a margin outside the picture, so no content is
    // covered; the gridlines are blended rather than painted, so text under
    // them stays readable. The point is that a position can then be read off
    // the image instead of estimated, and read in the same coordinate system
    // every other tool uses.
    //
    // `gridStep` fixes the spacing between labelled lines, in screen pixels.
    // Zero picks a spacing from the region's size, which is what a zoom wants:
    // the region varies, and the grid should stay about as fine relative to it.
    // A fixed step is for the full screen, where the size never changes and a
    // predictable grid is worth more than an adaptive one.
    std::vector<uint8_t> screenshotRegionPng(Rect& region, int scale = 1,
                                             const Marker* marker = nullptr,
                                             bool rulers = false,
                                             int gridStep = 0) const;

    // Digit size the rulers are drawn at, as a multiple of the 5x7 font.
    //
    // Larger on an unenlarged image, because that is the case where the whole
    // picture is about to be shrunk hardest: a vision model resizes a
    // screenshot to a fixed size regardless of what it started at, so a full
    // 1900px-wide screen loses more than half its width while a 3x zoom of a
    // 320px region loses none. Digits that survive one do not survive the
    // other.
    static constexpr int rulerGlyphScale(int scale) { return scale >= 2 ? 2 : 3; }

    // Output pixels the rulers occupy along the left and top edges when
    // `rulers` is set: room for a four-digit label plus padding. Content
    // therefore starts at (rulerMarginLeft(scale), rulerMarginTop(scale)),
    // which is what a caller needs to state the mapping from image pixels back
    // to screen coordinates. The 6 and 7 are the font's advance and height;
    // the rendering code static_asserts that they still match.
    static constexpr int rulerMarginLeft(int scale) {
        return rulerGlyphScale(scale) * 6 * 4 + 6;
    }
    static constexpr int rulerMarginTop(int scale) {
        return rulerGlyphScale(scale) * 7 + 5;
    }

    // Human-readable account of the negotiated pixel format and what has
    // actually reached the composite. A framebuffer update arriving does not
    // imply it carried pixels, so this distinguishes "server sent nothing"
    // from "we failed to decode what it sent".
    std::string diagnostics() const;

    // Raw input. Coordinates are framebuffer pixels; `buttonMask` is a
    // bitwise-or of MouseButton values, with 0 meaning "no buttons held".
    void sendPointer(uint8_t buttonMask, uint16_t x, uint16_t y);
    void sendKey(uint32_t keysym, bool down);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace tapto
