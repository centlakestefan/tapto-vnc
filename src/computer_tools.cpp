// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/computer_tools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "tapto/frame_index.h"
#include "tapto/input_map.h"
#include "tapto/log.h"
#include "tapto/vnc_session.h"

using nlohmann::json;

namespace tapto {
namespace {

// How long to let the screen settle after an action before sampling it. Clicks
// commonly trigger animation, and a screenshot taken too eagerly shows a
// half-drawn menu â€” which the model then reasons about as if it were final.
constexpr auto kActionSettle = std::chrono::milliseconds(400);
constexpr auto kCaptureTimeout = std::chrono::milliseconds(4000);

// Upper bound on a single vnc_wait, so the model cannot stall the loop.
constexpr int kMaxWaitMs = 10000;

int intField(const json& input, const char* name, int fallback = 0) {
    if (!input.contains(name)) return fallback;
    const json& value = input[name];
    if (value.is_number_integer()) return value.get<int>();
    if (value.is_number_float())   return static_cast<int>(value.get<double>());
    if (value.is_string()) {
        try { return std::stoi(value.get<std::string>()); } catch (...) { return fallback; }
    }
    return fallback;
}

// A field the caller must supply. Returns an error message when it is absent
// or unreadable, rather than quietly falling back to a default.
//
// intField's default is right for optional parameters and wrong for required
// ones: a missing coordinate becomes 0, so a malformed call turns into a real
// click at the top-left corner of the screen. Observed as a model omitting x
// and y, zooming repeatedly on the Recycle Bin, and reasoning at length about
// why the file list was not there â€” eleven steps that a one-line complaint
// would have ended.
std::string requireInt(const json& input, const char* name, int& out) {
    if (!input.is_object() || !input.contains(name) || input[name].is_null()) {
        return std::string("'") + name + "' is required and was not supplied.";
    }
    const json& value = input[name];
    if (value.is_number_integer()) { out = value.get<int>(); return ""; }
    if (value.is_number_float())   { out = static_cast<int>(value.get<double>()); return ""; }
    if (value.is_string()) {
        try { out = std::stoi(value.get<std::string>()); return ""; }
        catch (...) { /* fall through to the error below */ }
    }
    return std::string("'") + name + "' must be a number; received " + value.dump() + ".";
}

// Reports every missing coordinate at once, so a caller that omitted both is
// not corrected one field per round trip.
std::string requirePoint(const json& input, const char* xName, const char* yName,
                         int& x, int& y) {
    const std::string xError = requireInt(input, xName, x);
    const std::string yError = requireInt(input, yName, y);
    if (xError.empty() && yError.empty()) return "";
    std::string message = xError;
    if (!message.empty() && !yError.empty()) message += " ";
    message += yError;
    return message + " Give both as whole numbers of screen pixels.";
}

std::string stringField(const json& input, const char* name, const std::string& fallback = "") {
    if (input.contains(name) && input[name].is_string()) {
        return input[name].get<std::string>();
    }
    return fallback;
}

// The frame index owns the numbering, the naming, the timestamps and the file
// itself (tapto/frame_index.h). It lives outside this file because the words
// that go into it — prompts, replies, the model's reasoning — are recorded
// where they happen, which is nowhere near a tool call.
using frames::Meta;

// The most recent vnc_zoom. A file-scope value rather than Context state
// because there is exactly one session per process.
struct ZoomView {
    VncSession::Rect region;
    int scale = 0;      // 0 = no zoom has been taken yet
    // Cleared by every action, because an action changes the screen and a zoom
    // of the previous screen is not evidence about the current one.
    bool fresh = false;
};
ZoomView g_lastZoom;

// When set, vnc_click refuses a coordinate the model has not just looked at
// closely: there must be a zoom since the last action, and the point must lie
// inside it.
//
// Off by default, because a model that grounds well does not need it and the
// extra step costs a round trip. On for local models, where guessing a
// position from a full screenshot is the single most common way a run goes
// wrong. How badly depends on the model and by more than it looks: Gemma 4
// compresses any image to ~17x17 tokens, so a 21px list row is a third of one
// cell and is not there to be seen, while Qwen3-VL tokenises 32x32 blocks and
// sees the same screen 5.6x finer. Every model tried so far has been told to
// zoom first in the tool descriptions and has ignored it at least once, which
// is why this is a gate rather than more prose.
bool g_requireZoom = false;

// Gridline spacing, in screen pixels, for the full screenshots â€” 0 leaves them
// plain, which is the default.
//
// The zoom's rulers work because reading a labelled line is easier than
// estimating a fraction of an image. Whether the same holds at 1:1 is a
// separate question, and not an obvious one: a full screenshot is where the
// vision tower's downscaling bites hardest, so the labels are at their least
// legible exactly where the picture is at its least detailed. A grid also adds
// lines across every piece of text on the screen.
//
// So it is off unless asked for, and the value is the experiment's parameter
// rather than a constant.
int g_screenshotGrid = 0;

// Whether a full screenshot carries rulers. Reads better at the call sites
// than comparing the pixel value to zero.
bool gridOn() { return g_screenshotGrid > 0; }

// Saves the screen an action is about to change, with a dot on the point it is
// aimed at â€” the "before" half of a pair whose "after" half is the capture that
// follows the action.
//
// Deliberately not a fresh capture. This renders the composite as it already
// stands, which is the picture the model was looking at when it chose the
// coordinate, and it is that picture the dot has to sit on: a click that misses
// misses the button *where the model saw it*, and re-capturing first would show
// the dot against a screen it never reasoned about. Rendering costs no round
// trip and no settle, so an action is not slowed down by being filmed.
//
// The frame is written to disk and never shown to the model. These runs exist
// to measure how accurately it aims, and handing it a picture of where its last
// shot landed is feedback that would alter the behaviour being measured; it
// would also be one more thing on screen to reason about, and easily misread as
// part of the remote desktop.
void saveAimFrame(Context& context, VncSession& session, int x, int y,
                  const std::string& label) {
    if (!frames::enabled()) return;
    const VncSession::Marker marker{x, y, 5};
    VncSession::Rect whole;
    // Gridded to match what the model saw, so the frame on disk and the frame
    // it reasoned about are the same picture apart from the dot.
    std::vector<uint8_t> png =
        session.screenshotRegionPng(whole, 1, &marker, gridOn(), g_screenshotGrid);
    Meta meta;
    meta.phase   = "before";
    meta.has_aim = true;
    meta.aim_x   = x;
    meta.aim_y   = y;
    meta.width  = (gridOn() ? VncSession::rulerMarginLeft(1) : 0) + session.width();
    meta.height = (gridOn() ? VncSession::rulerMarginTop(1)  : 0) + session.height();
    frames::save(png, label, meta);
}

// Captures the screen and parks it for the backend to deliver.
std::string captureInto(Context& context, VncSession& session, const std::string& label,
                        const char* phase = "") {
    const bool ok = session.capture(kCaptureTimeout, kActionSettle);

    // Any action invalidates the last zoom: the screen it showed is gone.
    // Every action tool ends here, and vnc_zoom deliberately does not, so this
    // is the one place that needs to say so.
    g_lastZoom.fresh = false;

    ToolImage shot;
    if (gridOn()) {
        VncSession::Rect whole;
        shot.png = session.screenshotRegionPng(whole, 1, nullptr, true, g_screenshotGrid);
        // Margins included: the rulers are part of the image, and a size that
        // omitted them would describe a picture nobody is looking at.
        shot.width  = VncSession::rulerMarginLeft(1) + session.width();
        shot.height = VncSession::rulerMarginTop(1)  + session.height();
    } else {
        shot.png = session.screenshotPng();
        shot.width  = session.width();
        shot.height = session.height();
    }
    shot.label = label;

    Meta meta;
    meta.phase  = phase;
    meta.width  = shot.width;
    meta.height = shot.height;
    frames::save(shot.png, label, meta);
    putToolImage(context, shot);

    std::ostringstream out;
    // The screen's size, not the image's: this is the number the model needs
    // for a coordinate, and with rulers on the two differ by the margins.
    out << label << ". The screen is " << session.width() << "x" << session.height()
        << " pixels; the attached screenshot shows its current state.";
    if (gridOn()) {
        out << " It is ruled and gridded every " << g_screenshotGrid
            << " pixels in screen coordinates, the same ones vnc_click takes â€” read a "
               "position off the rulers rather than judging it. The grid does not make "
               "the picture any sharper, so use vnc_zoom when you need to tell similar "
               "things apart.";
    }
    if (!ok) out << " (warning: the server sent no framebuffer update, so this may be stale)";
    return out.str();
}

// One zoom, always the same. The model says where to look; it does not say how
// much or how close.
//
// The sizes are the ones the old adjustable version defaulted to, kept because
// they were measured: aiming error in a zoomed view grows with the region's
// height, and 100px keeps it inside a single ~21px list row. 320 across at 3x
// puts the long side at 960, near the 896 a fixed-budget vision tower resizes
// to â€” past that such a model gains nothing, and short of it the crop wastes
// budget it has already paid for.
//
// What enlarging costs depends on the model, and the two answers are far
// apart: Gemma 4 spends 280 tokens on any image at all, so 3x is free, while
// Qwen3-VL bills by area and the same zoom costs 320 tokens against about 30.
// It earns that either way. Replicating pixels adds no information, but it
// changes how the image is cut into patches, and a 32x32 patch holding three
// rows of text becomes three patches holding one row each.
//
// Making them constants rather than defaults is the point of the change.
// Adjustable, they were not used: across one 17-zoom run the model asked for
// eleven different rectangles, converged on roughly this one by itself, and
// spent four of those calls being clamped up from something too small to
// contain its target. What that cost was a ruler whose spacing changed with
// every call â€” niceStep() runs on whatever rectangle arrives â€” so the model
// had to work out the pitch afresh each time, and demonstrably got it wrong.
// Fixed, the ruler is identical in every zoom the model will ever see.
constexpr int kZoomWidth  = 320;
constexpr int kZoomHeight = 100;
constexpr int kZoomScale  = 3;

// Holds a reported coordinate pair inside the framebuffer.
//
// Clamping rather than failing is deliberate: a coordinate a few pixels outside
// the screen is a rounding slip, not a reason to abandon the turn.
//
// There is no coordinate conversion here, and deliberately so. Positions are
// screen pixels everywhere â€” in the tool schemas, in the zoom rulers, in the
// status line and in the saved filenames â€” so a number the model reads off a
// ruler is the same number vnc_click receives. An earlier build could rescale
// from a normalised 0..N grid for models trained that way; it made the pixel
// contract conditional on a flag, and every accuracy problem worth solving
// turned out to be about what the model could see, not about which grid it was
// answering on.
void clampToScreen(const VncSession& session, int& x, int& y) {
    const int maxX = session.width()  > 0 ? session.width()  - 1 : 0;
    const int maxY = session.height() > 0 ? session.height() - 1 : 0;
    x = x < 0 ? 0 : (x > maxX ? maxX : x);
    y = y < 0 ? 0 : (y > maxY ? maxY : y);
}

json coordinateSchema(const char* xDesc, const char* yDesc) {
    return json{
        {"x", {{"type", "integer"}, {"description", xDesc}}},
        {"y", {{"type", "integer"}, {"description", yDesc}}},
    };
}

// Spelled out in every coordinate description because some vision models
// default to a normalised grid â€” the Gemma/PaliGemma lineage is trained on
// <locNNNN> tokens over a 0..1024 space, and will answer that way unasked. The
// tools take pixels and only pixels, so the schema has to say so.
constexpr const char* kPixelNote =
    " Give this in actual screen pixels matching the screenshot's real size, "
    "not a normalised 0-1000 or 0-1 value.";

// --- executors -------------------------------------------------------------

std::string doScreenshot(Context& context, const json&) {
    VncSession& session = sessionFrom(context);
    return captureInto(context, session, "Captured the screen");
}

std::string doMove(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int x = 0, y = 0;
    const std::string missing = requirePoint(input, "x", "y", x, y);
    if (!missing.empty()) return "ERROR: " + missing;
    clampToScreen(session, x, y);
    session.sendPointer(0, static_cast<uint16_t>(x), static_cast<uint16_t>(y));
    std::ostringstream label;
    label << "Moved pointer to (" << x << "," << y << ")";
    return captureInto(context, session, label.str());
}

// The previous click's screen position, so an exactly repeated click can be
// called out. Observed in a real run: the same coordinate clicked twice in a
// row, 60px below the button it was aiming at, with nothing on screen changing
// in between. The system prompt already says not to do this; a model that does
// it anyway needs to be told in the tool result, where it cannot be missed.
struct LastClick {
    int  x = 0, y = 0;
    bool valid = false;
};
LastClick g_lastClick;

bool noteClick(int x, int y) {
    const bool repeat = g_lastClick.valid && g_lastClick.x == x && g_lastClick.y == y;
    g_lastClick = LastClick{x, y, true};
    return repeat;
}

constexpr const char* kRepeatClickNote =
    " NOTE: this is the same point you clicked last time. If that did not have the "
    "effect you wanted, clicking it again will not either. Zoom on your target and look "
    "at where it actually is before clicking again.";

// Checks the button and repeat count. Separate from delivery because the
// "before" frame is written first, and a frame of a click that was never
// delivered would leave a pair with no second half.
std::string clickArgsError(const std::string& buttonName, int clicks) {
    if (input::buttonMaskForName(buttonName) == 0) {
        return "unknown button '" + buttonName + "'; use left, middle or right";
    }
    if (clicks < 1 || clicks > 3) return "clicks must be between 1 and 3";
    return {};
}

// Delivers a click at an already-resolved screen coordinate. Returns an error
// message, or empty on success.
std::string sendClick(VncSession& session, int x, int y,
                      const std::string& buttonName, int clicks) {
    if (const std::string error = clickArgsError(buttonName, clicks); !error.empty()) {
        return error;
    }
    const uint8_t mask = input::buttonMaskForName(buttonName);

    const auto px = static_cast<uint16_t>(x);
    const auto py = static_cast<uint16_t>(y);

    // Move first so the guest sees a hover before the press; some UIs only
    // arm a control on mouse-over.
    session.sendPointer(0, px, py);
    for (int i = 0; i < clicks; ++i) {
        session.sendPointer(mask, px, py);
        session.sendPointer(0, px, py);
        if (i + 1 < clicks) std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    return {};
}

// Enforces the zoom-before-click rule. Returns an error message, or empty when
// the click may proceed.
std::string zoomGateFor(int x, int y) {
    if (!g_requireZoom) return "";
    if (!g_lastZoom.fresh) {
        return "you have not zoomed since your last action, so this position is a guess "
               "from a full screenshot. A full screen carries too little detail to place "
               "anything smaller than a large button. Call vnc_zoom on the area around "
               "your target, read the position off its rulers, then click that.";
    }
    const VncSession::Rect& r = g_lastZoom.region;
    if (x < r.x || x >= r.x + r.width || y < r.y || y >= r.y + r.height) {
        std::ostringstream out;
        out << "(" << x << "," << y << ") is outside the area you last zoomed on (x="
            << r.x << ".." << r.x + r.width - 1 << ", y=" << r.y << ".."
            << r.y + r.height - 1 << "), so you have not actually looked at that point. "
            << "Zoom on it first. If your target was not in the last zoom, zoom again "
            << "centred on where it actually is, rather than clicking past the edge of "
            << "what you looked at.";
        return out.str();
    }
    return "";
}

std::string doClick(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int x = 0, y = 0;
    const std::string missing = requirePoint(input, "x", "y", x, y);
    if (!missing.empty()) return "ERROR: " + missing;
    clampToScreen(session, x, y);

    const std::string gate = zoomGateFor(x, y);
    if (!gate.empty()) return "ERROR: " + gate;

    const std::string buttonName = stringField(input, "button", "left");
    const int clicks = std::max(1, intField(input, "clicks", 1));

    if (const std::string bad = clickArgsError(buttonName, clicks); !bad.empty()) {
        return "ERROR: " + bad;
    }

    const bool repeat = noteClick(x, y);

    std::ostringstream aim;
    aim << "About to " << buttonName << "-click (" << x << "," << y << ")";
    saveAimFrame(context, session, x, y, aim.str());

    const std::string error = sendClick(session, x, y, buttonName, clicks);
    if (!error.empty()) return "ERROR: " + error;

    std::ostringstream label;
    label << (clicks > 1 ? std::to_string(clicks) + "x " : "") << buttonName
          << "-clicked at (" << x << "," << y << ")";
    std::string result = captureInto(context, session, label.str(), "after");
    if (repeat) result += kRepeatClickNote;
    return result;
}

std::string doZoom(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);

    int centreX = 0, centreY = 0;
    const std::string missing = requirePoint(input, "x", "y", centreX, centreY);
    if (!missing.empty()) {
        return "ERROR: " + missing +
               " They are the point you want to look at, and the view is centred on "
               "them; without them there is nothing to zoom on.";
    }
    clampToScreen(session, centreX, centreY);

    // Centred on the point asked for, then slid back inside the screen â€” slid,
    // not shrunk. A region that lost its edge would render at a different size,
    // and every zoom having the same size is the whole reason this is fixed:
    // the ruler's spacing is derived from the region, so a smaller region means
    // a different grid to read.
    VncSession::Rect region;
    region.width  = kZoomWidth;
    region.height = kZoomHeight;
    region.x = centreX - kZoomWidth  / 2;
    region.y = centreY - kZoomHeight / 2;
    if (region.x + region.width  > session.width())  region.x = session.width()  - region.width;
    if (region.y + region.height > session.height()) region.y = session.height() - region.height;
    if (region.x < 0) region.x = 0;
    if (region.y < 0) region.y = 0;

    const bool ok = session.capture(kCaptureTimeout, kActionSettle);

    std::vector<uint8_t> png =
        session.screenshotRegionPng(region, kZoomScale, nullptr, /*rulers=*/true);
    if (png.empty()) return "ERROR: could not render that region of the screen";

    // screenshotRegionPng clamps `region` to the framebuffer and writes it
    // back, so this records what was rendered. It only differs from the above
    // on a screen smaller than the region itself.
    g_lastZoom.region = region;
    g_lastZoom.fresh  = true;
    g_lastZoom.scale  = kZoomScale;

    std::ostringstream label;
    label << "Zoomed " << kZoomScale << "x on " << region.width << "x" << region.height
          << " at (" << region.x << "," << region.y << ")";

    ToolImage shot;
    shot.png    = std::move(png);
    // The rendered image is the enlarged region plus the ruler margins, so
    // these are the PNG's real dimensions rather than the region's.
    shot.width  = VncSession::rulerMarginLeft(kZoomScale) + region.width  * kZoomScale;
    shot.height = VncSession::rulerMarginTop(kZoomScale)  + region.height * kZoomScale;
    shot.label  = label.str();
    Meta meta;
    meta.kind   = "zoom";
    meta.width  = shot.width;
    meta.height = shot.height;
    frames::save(shot.png, shot.label, meta);
    putToolImage(context, shot);

    std::ostringstream out;
    out << "Zoomed view of the screen region x=" << region.x << ".."
        << region.x + region.width - 1 << ", y=" << region.y << ".."
        << region.y + region.height - 1 << ", enlarged " << kZoomScale << "x.";
    if (region.x != centreX - kZoomWidth / 2 || region.y != centreY - kZoomHeight / 2) {
        out << " That is not quite centred on (" << centreX << "," << centreY
            << ") because the view would have run off the edge of the screen, so it was "
               "moved inside; the point you asked about is still in it.";
    }
    out << " The image has a numbered ruler across the top and down the left "
           "side, and a magenta grid drawn from them. Those numbers are real "
           "screen coordinates, the same ones vnc_click takes. To click "
           "something in this view, find the gridlines nearest it and read "
           "their labels off the rulers, then pass that straight to vnc_click."
        << " Be careful not to give vnc_click a position measured in this "
           "image's own pixels â€” the image is larger than the region it shows "
           "and starts at its top-left corner, so those numbers are much too "
           "big. If you have measured something that way, convert it first: "
        << "screen_x = " << region.x << " + (image_x - "
        << VncSession::rulerMarginLeft(kZoomScale) << ") / " << kZoomScale
        << ", screen_y = " << region.y << " + (image_y - "
        << VncSession::rulerMarginTop(kZoomScale) << ") / " << kZoomScale << ".";
    if (!ok) out << " (warning: the server sent no framebuffer update, so this may be stale)";
    return out.str();
}

std::string doDrag(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int fromX = 0, fromY = 0, toX = 0, toY = 0;
    const std::string missingFrom = requirePoint(input, "from_x", "from_y", fromX, fromY);
    if (!missingFrom.empty()) return "ERROR: " + missingFrom;
    const std::string missingTo = requirePoint(input, "to_x", "to_y", toX, toY);
    if (!missingTo.empty()) return "ERROR: " + missingTo;
    clampToScreen(session, fromX, fromY);
    clampToScreen(session, toX, toY);

    const std::string buttonName = stringField(input, "button", "left");
    const uint8_t mask = input::buttonMaskForName(buttonName);
    if (mask == 0) return "ERROR: unknown button '" + buttonName + "'";

    // Marked at the point it grabs: that is the position that has to be right
    // for the drag to pick up the thing it meant to, and the one the result
    // frame can no longer show.
    std::ostringstream aim;
    aim << "About to drag from (" << fromX << "," << fromY << ") to ("
        << toX << "," << toY << ")";
    saveAimFrame(context, session, fromX, fromY, aim.str());

    session.sendPointer(0, static_cast<uint16_t>(fromX), static_cast<uint16_t>(fromY));
    session.sendPointer(mask, static_cast<uint16_t>(fromX), static_cast<uint16_t>(fromY));

    // Interpolate: a single jump from press to release reads as a teleport and
    // many drag handlers simply ignore it.
    constexpr int kSteps = 8;
    for (int step = 1; step <= kSteps; ++step) {
        const int x = fromX + (toX - fromX) * step / kSteps;
        const int y = fromY + (toY - fromY) * step / kSteps;
        session.sendPointer(mask, static_cast<uint16_t>(x), static_cast<uint16_t>(y));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    session.sendPointer(0, static_cast<uint16_t>(toX), static_cast<uint16_t>(toY));

    std::ostringstream label;
    label << "Dragged from (" << fromX << "," << fromY << ") to (" << toX << "," << toY << ")";
    return captureInto(context, session, label.str(), "after");
}

std::string doScroll(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int x = 0, y = 0;
    const std::string missing = requirePoint(input, "x", "y", x, y);
    if (!missing.empty()) return "ERROR: " + missing;
    clampToScreen(session, x, y);

    const std::string direction = stringField(input, "direction", "down");
    uint8_t mask = 0;
    if (direction == "up")        mask = kMouseWheelUp;
    else if (direction == "down") mask = kMouseWheelDown;
    else return "ERROR: direction must be 'up' or 'down'";

    int amount = intField(input, "amount", 3);
    if (amount < 1) amount = 1;
    if (amount > 20) amount = 20;

    const auto px = static_cast<uint16_t>(x);
    const auto py = static_cast<uint16_t>(y);
    session.sendPointer(0, px, py);
    // The wheel is modelled as a button that is pressed and released once per
    // notch; there is no scroll message in RFB.
    for (int i = 0; i < amount; ++i) {
        session.sendPointer(mask, px, py);
        session.sendPointer(0, px, py);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::ostringstream label;
    label << "Scrolled " << direction << " " << amount << " notches at (" << x << "," << y << ")";
    return captureInto(context, session, label.str());
}

std::string doType(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    const std::string text = stringField(input, "text");
    if (text.empty()) return "ERROR: 'text' is required and must be non-empty";
    if (text.size() > 4096) return "ERROR: 'text' is too long (max 4096 characters)";

    constexpr uint32_t kShiftLeft = 0xFFE1;
    constexpr uint32_t kAltGr     = 0xFFEA;  // Alt_R / ISO_Level3_Shift
    constexpr uint32_t kAltLeft   = 0xFFE9;  // Alt codes need the *left* Alt
    constexpr uint32_t kKeypadZero = 0xFFB0; // XK_KP_0; digits are contiguous
    size_t sent = 0, skipped = 0, viaAltCode = 0;

    for (char c : text) {
        const input::TypePlan plan = input::planForChar(c);

        if (plan.method == input::TypeMethod::Direct) {
            const input::KeyStroke& stroke = plan.stroke;
            if (stroke.shift) session.sendKey(kShiftLeft, true);
            if (stroke.altgr) session.sendKey(kAltGr, true);
            session.sendKey(stroke.keysym, true);
            session.sendKey(stroke.keysym, false);
            if (stroke.altgr) session.sendKey(kAltGr, false);
            if (stroke.shift) session.sendKey(kShiftLeft, false);
            ++sent;
        } else if (plan.method == input::TypeMethod::AltCode) {
            // Alt held down for the whole sequence; Windows emits the
            // character on release. The leading zero selects the ANSI code
            // page, which is the well-defined form.
            char digits[5];
            std::snprintf(digits, sizeof(digits), "0%03d", plan.code);

            session.sendKey(kAltLeft, true);
            for (const char* d = digits; *d; ++d) {
                const uint32_t key = kKeypadZero + static_cast<uint32_t>(*d - '0');
                session.sendKey(key, true);
                session.sendKey(key, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            session.sendKey(kAltLeft, false);
            ++sent;
            ++viaAltCode;
        } else {
            ++skipped;
            continue;
        }

        // Guests drop keystrokes delivered faster than their input queue
        // drains, especially through a remote console.
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
    }

    std::ostringstream label;
    label << "Typed " << sent << " character" << (sent == 1 ? "" : "s");
    if (viaAltCode > 0) {
        label << " (" << viaAltCode << " via Alt+numpad, which needs NumLock on)";
    }
    if (skipped > 0) label << " (" << skipped << " character(s) not reachable on the "
                           << input::remoteLayout() << " layout, skipped)";
    return captureInto(context, session, label.str());
}

std::string doKey(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    const std::string spec = stringField(input, "keys");
    if (spec.empty()) return "ERROR: 'keys' is required, e.g. \"ctrl+alt+delete\"";

    input::Chord chord;
    std::string error;
    if (!input::parseChord(spec, chord, error)) return "ERROR: " + error;

    // Press modifiers outermost-first and release in reverse, so the guest
    // never observes a modifier outliving the key it applied to.
    for (uint32_t mod : chord.modifiers) session.sendKey(mod, true);
    session.sendKey(chord.key, true);
    session.sendKey(chord.key, false);
    for (auto it = chord.modifiers.rbegin(); it != chord.modifiers.rend(); ++it) {
        session.sendKey(*it, false);
    }

    return captureInto(context, session, "Pressed " + spec);
}

std::string doWait(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int ms = intField(input, "ms", 1000);
    if (ms < 0) ms = 0;
    if (ms > kMaxWaitMs) ms = kMaxWaitMs;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        // Keep draining the connection while waiting; otherwise updates queue
        // up and the screen we finally sample is stale.
        session.pump(std::chrono::milliseconds(50));
    }

    return captureInto(context, session, "Waited " + std::to_string(ms) + "ms");
}

// Turns a dead connection into a ConnectionLost, before the tool runs and
// again if it dies mid-call.
//
// Checked here rather than in each executor because every one of them touches
// the session, and because the failure is the same whichever one noticed it.
// The idle gap goes in the trace: it is the measurement that tells an idle
// timeout apart from a network fault, and it is only available at the moment
// the loss is discovered.
[[noreturn]] void reportLost(const std::string& tool, int idle, const std::string& detail) {
    mclog("VNC connection lost during '" + tool + "' after " + std::to_string(idle) +
          "s idle, not recovered" + (detail.empty() ? "" : ": " + detail) + "\n");

    std::ostringstream out;
    out << "the connection to the remote screen was lost";
    if (idle > 0) out << " after " << idle << "s with no traffic";
    if (!detail.empty()) out << " (" << detail << ")";
    out << ", and could not be re-established. Nothing further can be done with it "
           "in this run.";
    throw ConnectionLost(out.str());
}

// One attempt, using whatever the caller left in the context. Absent when the
// session was opened by something that cannot reopen it, in which case a drop
// is simply fatal.
//
// The reason is logged here and not only when recovery fails, because a
// recovered drop is now the common case and it is the one still being
// diagnosed: a reconnect that works leaves no other trace of what went wrong,
// so the evidence would be discarded exactly when the run carries on long
// enough to hit the fault again.
bool tryReconnect(Context& context, const std::string& tool, int idle,
                  const std::string& detail) {
    mclog("VNC connection lost during '" + tool + "' after " + std::to_string(idle) +
          "s idle" + (detail.empty() ? "" : " (" + detail + ")") + "; reconnecting\n");
    if (!context.has(keys::kReconnect)) return false;
    const bool ok = context.get<std::function<bool()>>(keys::kReconnect)();
    mclog(ok ? "VNC reconnected\n" : "VNC reconnect failed\n");
    return ok;
}

// Re-running a tool after a reconnect is only safe if running it twice is the
// same as running it once. Looking at the screen is; changing it is not â€” a
// click that died between press and release may or may not have reached the
// guest, and repeating an action has already cost this project a duplicated
// installation. A pointer move is idempotent by position, so it qualifies.
bool retrySafeTool(const std::string& name) {
    return name == "vnc_screenshot" || name == "vnc_zoom" ||
           name == "vnc_wait" || name == "vnc_move";
}

ToolExecutorFn guardConnection(std::string name, ToolExecutorFn inner) {
    return [name = std::move(name), inner = std::move(inner)](
               Context& context, const json& input) -> std::string {
        VncSession& session = sessionFrom(context);

        // Already dead before this call. Nothing is half-done, so a successful
        // reconnect lets it proceed as though the drop had never happened.
        if (!session.isConnected()) {
            const int idle = session.idleSeconds();
            const std::string detail = session.lastError();
            if (!tryReconnect(context, name, idle, detail)) reportLost(name, idle, detail);
            return inner(context, input);
        }

        try {
            return inner(context, input);
        } catch (const ConnectionLost&) {
            throw;
        } catch (const std::exception& e) {
            // A tool that failed for its own reasons keeps the old behaviour:
            // the model is told and can try something else. Only a failure that
            // took the connection with it gets here.
            if (session.isConnected()) throw;

            // Read before reconnecting â€” the handshake counts as traffic and
            // clears the error, resetting the two things worth recording.
            const int idle = session.idleSeconds();
            // The RFB layer's own account beats the symptom the caller saw:
            // "Socket is not connected" only says somebody touched a closed
            // socket, whereas this says what closed it.
            const std::string detail =
                session.lastError().empty() ? std::string(e.what()) : session.lastError();
            if (!tryReconnect(context, name, idle, detail)) reportLost(name, idle, detail);

            if (retrySafeTool(name)) return inner(context, input);

            // An action that died mid-flight. The screen is trustworthy again;
            // what the action did to it is not, and only the model can judge
            // that from the picture.
            std::ostringstream out;
            out << "ERROR: the connection dropped while " << name << " was running and "
                   "has been re-established. Whether it reached the machine at all is "
                   "unknown. Do not simply repeat it â€” look at the screen first and "
                   "work out whether it happened. ";
            out << captureInto(context, session, "Reconnected after a dropped connection");
            return out.str();
        }
    };
}

json objectSchema(json properties, std::vector<std::string> required) {
    return json{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
    };
}

}  // namespace

void setRequireZoom(bool require) { g_requireZoom = require; }
bool requireZoom() { return g_requireZoom; }

void setScreenshotGrid(int step) { g_screenshotGrid = step > 0 ? step : 0; }
int  screenshotGrid() { return g_screenshotGrid; }

VncSession& sessionFrom(Context& context) {
    return *context.get<VncSession*>(keys::kSession);
}

std::vector<ToolSpec> makeComputerTools() {
    std::vector<ToolSpec> tools;

    {
        std::string description =
            "Take a screenshot of the remote screen. Use this to see the current state "
            "before deciding what to do, or to confirm the result of an earlier action.";
        if (gridOn()) {
            description +=
                "\nThe screenshot carries a numbered ruler along its top and left edges "
                "and a grid drawn every " + std::to_string(g_screenshotGrid) +
                " pixels, labelled in screen coordinates â€” the same ones vnc_click "
                "takes. Use it to read a position rather than estimate one.\n"
                "It shows you where things are, not what they are: the whole screen "
                "still has to fit into one image, so anything small stays hard to "
                "identify however finely it is gridded. Zoom before clicking something "
                "you could confuse with its neighbour.";
        }
        tools.push_back(ToolSpec{
            "vnc_screenshot", description, objectSchema(json::object(), {}),
            doScreenshot, ""});
    }

    {
        json props = coordinateSchema((std::string("Horizontal pixel coordinate, 0 at the left edge") + kPixelNote).c_str(),
                                      (std::string("Vertical pixel coordinate, 0 at the top edge") + kPixelNote).c_str());
        props["button"] = {{"type", "string"}, {"enum", {"left", "middle", "right"}},
                           {"description", "Mouse button to click. Defaults to left."}};
        props["clicks"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 3},
                           {"description", "Number of clicks; use 2 for a double-click. Defaults to 1."}};
        tools.push_back(ToolSpec{
            "vnc_click",
            "Click the mouse at a screen coordinate. Returns a screenshot taken after "
            "the click so you can confirm what happened.",
            objectSchema(props, {"x", "y"}), doClick, ""});
    }

    {
        const std::string point =
            " Give the point you want to look at, not a corner: the view is centred on it.";
        json props = json{
            {"x", {{"type", "integer"},
                   {"description", "Horizontal screen coordinate to centre the view on." + point}}},
            {"y", {{"type", "integer"},
                   {"description", "Vertical screen coordinate to centre the view on." + point}}},
        };
        tools.push_back(ToolSpec{
            "vnc_zoom",
            "Look closely at one part of the screen. Returns the " +
            std::to_string(kZoomWidth) + "x" + std::to_string(kZoomHeight) +
            " pixel area around the point you name, enlarged " + std::to_string(kZoomScale) +
            "x â€” about five rows of a list. The size and the magnification are always "
            "these; you only choose where to look.\n"
            "Use it whenever you need to tell apart things that are close together â€” which "
            "row of a file list or menu is which, the exact text in a small label, the "
            "state of a checkbox â€” and always before clicking something in a dense list. A "
            "full screenshot has to represent the entire screen at once, so fine detail is "
            "lost; a zoom spends all of it on this one area.\n"
            "The result is ruled and gridded in screen coordinates, so you do not have to "
            "judge where something is â€” you can read it off. Find the target, look at the "
            "gridlines that bracket it, read the numbers on the rulers, and click that "
            "position with vnc_click.\n"
            "If what you wanted is not in the view, zoom again on where it actually is. A "
            "click aimed at something you cannot see in the picture lands on empty "
            "background, so move the view rather than guessing past its edge.",
            objectSchema(props, {"x", "y"}), doZoom, ""});
    }

    tools.push_back(ToolSpec{
        "vnc_move",
        "Move the mouse pointer without clicking. Useful to reveal hover states, "
        "tooltips, or menus that open on mouse-over.",
        objectSchema(coordinateSchema("Horizontal pixel coordinate", "Vertical pixel coordinate"),
                     {"x", "y"}),
        doMove, ""});

    {
        json props = json{
            {"from_x", {{"type", "integer"}, {"description", "Starting horizontal coordinate"}}},
            {"from_y", {{"type", "integer"}, {"description", "Starting vertical coordinate"}}},
            {"to_x",   {{"type", "integer"}, {"description", "Ending horizontal coordinate"}}},
            {"to_y",   {{"type", "integer"}, {"description", "Ending vertical coordinate"}}},
            {"button", {{"type", "string"}, {"enum", {"left", "middle", "right"}},
                        {"description", "Button held during the drag. Defaults to left."}}},
        };
        tools.push_back(ToolSpec{
            "vnc_drag",
            "Press the mouse button at one point, move to another, and release. Use for "
            "selecting text, moving windows, or dragging sliders.",
            objectSchema(props, {"from_x", "from_y", "to_x", "to_y"}), doDrag, ""});
    }

    {
        json props = coordinateSchema("Horizontal coordinate to scroll at",
                                      "Vertical coordinate to scroll at");
        props["direction"] = {{"type", "string"}, {"enum", {"up", "down"}},
                              {"description", "Scroll direction"}};
        props["amount"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 20},
                           {"description", "Number of wheel notches. Defaults to 3."}};
        tools.push_back(ToolSpec{
            "vnc_scroll",
            "Scroll the mouse wheel at a screen coordinate.",
            objectSchema(props, {"x", "y", "direction"}), doScroll, ""});
    }

    tools.push_back(ToolSpec{
        "vnc_type",
        "Type literal text, as if entered on the keyboard. Use this for ordinary text "
        "entry; for named keys and shortcuts use vnc_key instead. Click the target field "
        "first so the text goes where you intend.\n"
        "Text is delivered by key position and translated for the remote machine's "
        "configured keyboard layout, so punctuation is normally correct. It can still "
        "go wrong if that layout is set incorrectly. When punctuation matters, check it "
        "in the returned screenshot, and if a character came out wrong, correct that "
        "character rather than retyping the whole string. The result also reports any "
        "characters that could not be typed at all.",
        objectSchema(json{{"text", {{"type", "string"},
                                    {"description", "The text to type"}}}},
                     {"text"}),
        doType, ""});

    tools.push_back(ToolSpec{
        "vnc_key",
        "Press a single key or a key combination. Modifiers are joined with '+'. "
        "Examples: \"Return\", \"Escape\", \"F5\", \"ctrl+c\", \"alt+F4\", "
        "\"ctrl+alt+delete\", \"win\".",
        objectSchema(json{{"keys", {{"type", "string"},
                                    {"description", "Key or combination, e.g. \"ctrl+s\""}}}},
                     {"keys"}),
        doKey, ""});

    tools.push_back(ToolSpec{
        "vnc_wait",
        "Wait for the screen to change on its own â€” while an application starts, a page "
        "loads, or an installer progresses â€” then take a screenshot.",
        objectSchema(json{{"ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxWaitMs},
                                  {"description", "Milliseconds to wait. Defaults to 1000."}}}},
                     {}),
        doWait, ""});

    // Applied here rather than at each call site so a tool added later cannot
    // forget it: every one of these needs the session, and none of them can do
    // anything useful without it.
    for (ToolSpec& tool : tools) {
        tool.executor = guardConnection(tool.name, std::move(tool.executor));
    }

    return tools;
}

}  // namespace tapto
