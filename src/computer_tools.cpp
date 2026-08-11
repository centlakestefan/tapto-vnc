// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/computer_tools.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

#include "tapto/input_map.h"
#include "tapto/vnc_session.h"

using nlohmann::json;

namespace tapto {
namespace {

// How long to let the screen settle after an action before sampling it. Clicks
// commonly trigger animation, and a screenshot taken too eagerly shows a
// half-drawn menu — which the model then reasons about as if it were final.
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
// why the file list was not there — eleven steps that a one-line complaint
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

// Turns "2x left-clicked at (400,300)" into "2x-left-clicked-at-400-300" so it
// can serve as a filename on any platform.
std::string slugify(const std::string& text) {
    std::string out;
    bool lastWasDash = false;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(static_cast<char>(std::tolower(u)));
            lastWasDash = false;
        } else if (!lastWasDash && !out.empty()) {
            out.push_back('-');
            lastWasDash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > 60) out.resize(60);
    return out.empty() ? "screenshot" : out;
}

// Writes the screenshot alongside the run, if a directory was configured.
// Numbered so the sequence is obvious, and labelled so a frame can be found
// without opening every file.
void saveScreenshot(Context& context, const std::vector<uint8_t>& png,
                    const std::string& label) {
    if (!context.has(keys::kScreenshotDir)) return;
    const std::string dir = context.get<std::string>(keys::kScreenshotDir);
    if (dir.empty() || png.empty()) return;

    static int sequence = 0;
    ++sequence;

    try {
        std::filesystem::create_directories(dir);
        std::ostringstream name;
        name << std::setfill('0') << std::setw(4) << sequence << "-" << slugify(label) << ".png";
        const std::filesystem::path path = std::filesystem::path(dir) / name.str();

        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
    } catch (const std::exception&) {
        // Saving is a diagnostic convenience; never fail the tool call over it.
    }
}

// The most recent vnc_zoom. A file-scope value rather than Context state
// because there is exactly one session per process, which is the same reason
// g_coordinateSpan below is one.
struct ZoomView {
    VncSession::Rect region;
    int scale = 0;      // 0 = no zoom has been taken yet
    int imageWidth = 0;
    int imageHeight = 0;
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
// wrong — a full screen is compressed to roughly a 17x17 grid of soft tokens,
// so a 21px list row is a third of one cell and simply is not there to be
// seen. Every model tried so far has been told this in the tool descriptions
// and has ignored it at least once.
bool g_requireZoom = false;

// Captures the screen and parks it for the backend to deliver.
//
// `marker`, when set, puts a dot at that point in the copy written to disk —
// and only there. The model is deliberately shown the unmarked image: these
// runs exist to measure how accurately it aims, and handing it a picture of
// where its last shot landed is feedback that would alter the behaviour being
// measured. It would also be one more thing on screen to reason about, and
// easily misread as part of the remote desktop.
std::string captureInto(Context& context, VncSession& session, const std::string& label,
                        const VncSession::Marker* marker = nullptr) {
    const bool ok = session.capture(kCaptureTimeout, kActionSettle);

    // Any action invalidates the last zoom: the screen it showed is gone.
    // Every action tool ends here, and vnc_zoom deliberately does not, so this
    // is the one place that needs to say so.
    g_lastZoom.fresh = false;

    ToolImage shot;
    shot.png = session.screenshotPng();
    shot.width = session.width();
    shot.height = session.height();
    shot.label = label;

    if (marker && context.has(keys::kScreenshotDir)) {
        // Rendered a second time rather than drawn onto shot.png, which is
        // already PNG-encoded. Only pays that cost when saving is switched on.
        VncSession::Rect whole;
        saveScreenshot(context, session.screenshotRegionPng(whole, 1, marker), label);
    } else {
        saveScreenshot(context, shot.png, label);
    }
    putToolImage(context, shot);

    std::ostringstream out;
    out << label << ". The screen is " << shot.width << "x" << shot.height
        << " pixels; the attached screenshot shows its current state.";
    if (!ok) out << " (warning: the server sent no framebuffer update, so this may be stale)";
    return out.str();
}

// Bounds on a zoom region, enforced rather than merely asked for.
//
// A zoom is for aiming; vnc_screenshot is for surveying. Measurement showed
// aiming error grows with the region's height, and a run that was told to keep
// regions short chose 400, then 300, then 200 and missed with all three before
// succeeding at 100. Guidance a model can decline is not a mechanism.
//
// kMinZoomScale exists for the same reason: a 600px-wide region worked out to
// 1x under the old rule, so the model asked for a zoom and was handed the
// region at original size — a crop wearing the name of a zoom.
constexpr int kMaxZoomWidth  = 640;
constexpr int kMaxZoomHeight = 200;
constexpr int kMinZoomWidth  = 240;
constexpr int kMinZoomHeight = 80;
constexpr int kMinZoomScale  = 2;

// When true, vnc_zoom returns an image with screen-coordinate rulers and a
// grid, and vnc_click_zoom is not offered at all: the model reads a position
// off the ruler and clicks it with vnc_click, in the one coordinate system
// everything else uses.
//
// The alternative it replaces asked the model to report a position inside the
// zoomed image, which meant two pixel grids live at once. That is what it got
// wrong — asked for a checkbox at (377,181) in a 900x300 view it answered
// (50,180): the y exact, the x collapsed to the left edge. Reading a labelled
// gridline is a different task from estimating a fraction of an image, and
// reading is what this model is good at.
//
// Set to false to go back to the relative-coordinate scheme.
constexpr bool kRulerZoom = true;

// Enlargement for a requested region: enough to be worth looking at, without
// producing an image so large it costs more tokens than it repays.
//
// Vision models compress every image to a fixed token budget regardless of its
// size, so what a crop buys is *effective resolution* — the region fills the
// whole budget instead of a corner of it. The enlargement itself is what makes
// that legible rather than blocky.
int zoomScaleFor(int width, int height) {
    // 896 is the side the vision tower resizes to, so enlarging past it buys
    // nothing. Enlarging costs no extra tokens — an image is compressed to a
    // fixed token budget whatever its size — so there is no reason to stop
    // short of it either.
    constexpr int kTargetLongestSide = 896;
    const int longest = std::max(width, height);
    if (longest <= 0) return kMinZoomScale;
    // Rounded, not truncated: flooring 1.9 to 1 hands back the region at
    // original size, which is no help at all.
    const int scale = (kTargetLongestSide + longest / 2) / longest;
    return scale < kMinZoomScale ? kMinZoomScale : (scale > 4 ? 4 : scale);
}

int g_coordinateSpan = 0;   // 0 = coordinates are already pixels

// Converts a reported coordinate pair into framebuffer pixels, then clamps.
//
// Clamping rather than failing is deliberate: a coordinate a few pixels outside
// the screen is a rounding slip, not a reason to abandon the turn.
void toScreen(const VncSession& session, int& x, int& y) {
    if (g_coordinateSpan > 0) {
        x = static_cast<int>(std::llround(static_cast<double>(x) * session.width()  / g_coordinateSpan));
        y = static_cast<int>(std::llround(static_cast<double>(y) * session.height() / g_coordinateSpan));
    }
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
// default to a normalised grid. Saying it here fixes the well-behaved ones;
// setCoordinateSpan() exists for the rest.
constexpr const char* kPixelNote =
    " Give this in actual screen pixels matching the screenshot's real size, "
    "not a normalised 0-1000 or 0-1 value.";

// The same warning for the zoomed space, where it matters more: there are two
// pixel grids in play at once, so "pixels" alone does not say which.
constexpr const char* kZoomPixelNote =
    " Measure this in the zoomed image's own pixels, from its top-left corner. The "
    "vnc_zoom result states that image's exact size — use that scale, not a "
    "normalised 0-1000 or 0-1 value, and not a screen coordinate.";

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
    toScreen(session, x, y);
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
    "effect you wanted, clicking it again will not either. Zoom in on a short region "
    "centred on your target and look at where it actually is before clicking again.";

// Delivers a click at an already-resolved screen coordinate. Returns an error
// message, or empty on success. Shared by vnc_click and vnc_click_zoom, which
// differ only in the coordinate space they start from.
std::string sendClick(VncSession& session, int x, int y,
                      const std::string& buttonName, int clicks) {
    const uint8_t mask = input::buttonMaskForName(buttonName);
    if (mask == 0) return "unknown button '" + buttonName + "'; use left, middle or right";
    if (clicks < 1 || clicks > 3) return "clicks must be between 1 and 3";

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
            << "Zoom on the area around it first. If your target was not in the last "
            << "zoom, widen the region rather than clicking past its edge.";
        return out.str();
    }
    return "";
}

std::string doClick(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int x = 0, y = 0;
    const std::string missing = requirePoint(input, "x", "y", x, y);
    if (!missing.empty()) return "ERROR: " + missing;
    toScreen(session, x, y);

    const std::string gate = zoomGateFor(x, y);
    if (!gate.empty()) return "ERROR: " + gate;

    const std::string buttonName = stringField(input, "button", "left");
    const int clicks = std::max(1, intField(input, "clicks", 1));

    const bool repeat = noteClick(x, y);
    const std::string error = sendClick(session, x, y, buttonName, clicks);
    if (!error.empty()) return "ERROR: " + error;

    std::ostringstream label;
    label << (clicks > 1 ? std::to_string(clicks) + "x " : "") << buttonName
          << "-clicked at (" << x << "," << y << ")";
    const VncSession::Marker marker{x, y, 5};
    std::string result = captureInto(context, session, label.str(), &marker);
    if (repeat) result += kRepeatClickNote;
    return result;
}

std::string doZoom(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);

    VncSession::Rect region;
    const std::string missing = requirePoint(input, "x", "y", region.x, region.y);
    if (!missing.empty()) {
        return "ERROR: " + missing +
               " They are the top-left corner of the region you want to look at; without "
               "them there is nothing to zoom on.";
    }
    // Defaults chosen from measurement, not taste. Aiming error in a zoomed
    // view scales with the region's height and with the target's distance from
    // the region's centre — measured against one local model, a target on the
    // centre line came back within a pixel, one at 0.06 of the height was off
    // by 9, and the same target in a 256px region was off by 26. What a crop
    // buys in accuracy therefore comes from being short, not from being
    // magnified. 100px keeps the error inside a single ~21px list row.
    region.width  = intField(input, "width", 320);
    region.height = intField(input, "height", 100);

    // The region is named in screen coordinates, so it goes through the same
    // rescaling as every other coordinate the model reports.
    toScreen(session, region.x, region.y);
    if (g_coordinateSpan > 0) {
        region.width  = static_cast<int>(std::llround(
            static_cast<double>(region.width)  * session.width()  / g_coordinateSpan));
        region.height = static_cast<int>(std::llround(
            static_cast<double>(region.height) * session.height() / g_coordinateSpan));
    }
    // Floors, not suggestions. A run asked repeatedly for 200x64 regions that
    // did not reach the button it was aiming at, clicked inside them anyway,
    // and hit empty panel three times running. A crop this small rarely
    // contains its target plus enough surroundings to confirm it is the right
    // one, and the cost of being slightly too generous is only grid coarseness.
    if (region.width  < kMinZoomWidth)  region.width  = kMinZoomWidth;
    if (region.height < kMinZoomHeight) region.height = kMinZoomHeight;

    std::string capped;
    if (region.width > kMaxZoomWidth) {
        region.width = kMaxZoomWidth;
        capped = "width";
    }
    if (region.height > kMaxZoomHeight) {
        region.height = kMaxZoomHeight;
        capped = capped.empty() ? "height" : "width and height";
    }

    const bool ok = session.capture(kCaptureTimeout, kActionSettle);

    const int scale = zoomScaleFor(region.width, region.height);
    std::vector<uint8_t> png =
        session.screenshotRegionPng(region, scale, nullptr, kRulerZoom);
    if (png.empty()) return "ERROR: could not render that region of the screen";

    // screenshotRegionPng clamped `region` to the framebuffer, so this records
    // the rectangle actually rendered rather than the one asked for.
    g_lastZoom.region      = region;
    g_lastZoom.fresh       = true;
    g_lastZoom.scale       = scale;
    g_lastZoom.imageWidth  = region.width  * scale;
    g_lastZoom.imageHeight = region.height * scale;

    std::ostringstream label;
    label << "Zoomed " << scale << "x on " << region.width << "x" << region.height
          << " at (" << region.x << "," << region.y << ")";

    ToolImage shot;
    shot.png    = std::move(png);
    shot.width  = g_lastZoom.imageWidth;
    shot.height = g_lastZoom.imageHeight;
    shot.label  = label.str();
    saveScreenshot(context, shot.png, shot.label);
    putToolImage(context, shot);

    std::ostringstream out;
    out << "Zoomed view of the screen region x=" << region.x << ".."
        << region.x + region.width - 1 << ", y=" << region.y << ".."
        << region.y + region.height - 1 << ", enlarged " << scale << "x.";
    if (kRulerZoom) {
        out << " The image has a numbered ruler across the top and down the left "
               "side, and a magenta grid drawn from them. Those numbers are real "
               "screen coordinates, the same ones vnc_click takes. To click "
               "something in this view, find the gridlines nearest it and read "
               "their labels off the rulers, then pass that straight to vnc_click."
            << " Be careful not to give vnc_click a position measured in this "
               "image's own pixels — the image is larger than the region it shows "
               "and starts at its top-left corner, so those numbers are much too "
               "big. If you have measured something that way, convert it first: "
            << "screen_x = " << region.x << " + (image_x - " << VncSession::kRulerMarginLeft
            << ") / " << scale
            << ", screen_y = " << region.y << " + (image_y - " << VncSession::kRulerMarginTop
            << ") / " << scale << ".";
    } else {
        out << " The attached image is " << g_lastZoom.imageWidth << "x"
            << g_lastZoom.imageHeight << " pixels and shows only that region, not "
            << "the whole screen. To click something you can see in it, use "
            << "vnc_click_zoom with coordinates measured in this image.";
    }
    if (!capped.empty()) {
        out << " NOTE: the " << capped << " you asked for was larger than a zoom allows "
            << "and was reduced, so this shows less than you requested. A zoom is for "
            << "aiming at something you have already found; use vnc_screenshot to survey.";
    }
    if (!ok) out << " (warning: the server sent no framebuffer update, so this may be stale)";
    return out.str();
}

std::string doClickZoom(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    if (g_lastZoom.scale <= 0) {
        return "ERROR: no zoomed view has been taken yet. Call vnc_zoom first, then "
               "give coordinates measured in the image it returns.";
    }

    int zx = intField(input, "x"), zy = intField(input, "y");
    if (g_coordinateSpan > 0) {
        zx = static_cast<int>(std::llround(
            static_cast<double>(zx) * g_lastZoom.imageWidth  / g_coordinateSpan));
        zy = static_cast<int>(std::llround(
            static_cast<double>(zy) * g_lastZoom.imageHeight / g_coordinateSpan));
    }
    zx = zx < 0 ? 0 : (zx > g_lastZoom.imageWidth  - 1 ? g_lastZoom.imageWidth  - 1 : zx);
    zy = zy < 0 ? 0 : (zy > g_lastZoom.imageHeight - 1 ? g_lastZoom.imageHeight - 1 : zy);

    const int x = g_lastZoom.region.x + zx / g_lastZoom.scale;
    const int y = g_lastZoom.region.y + zy / g_lastZoom.scale;

    const std::string buttonName = stringField(input, "button", "left");
    const int clicks = std::max(1, intField(input, "clicks", 1));

    const bool repeat = noteClick(x, y);
    const std::string error = sendClick(session, x, y, buttonName, clicks);
    if (!error.empty()) return "ERROR: " + error;

    std::ostringstream label;
    label << (clicks > 1 ? std::to_string(clicks) + "x " : "") << buttonName
          << "-clicked at (" << x << "," << y << ") from the zoomed view at ("
          << zx << "," << zy << ")";
    const VncSession::Marker marker{x, y, 5};
    std::string result = captureInto(context, session, label.str(), &marker);
    if (repeat) result += kRepeatClickNote;
    return result;
}

std::string doDrag(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int fromX = 0, fromY = 0, toX = 0, toY = 0;
    const std::string missingFrom = requirePoint(input, "from_x", "from_y", fromX, fromY);
    if (!missingFrom.empty()) return "ERROR: " + missingFrom;
    const std::string missingTo = requirePoint(input, "to_x", "to_y", toX, toY);
    if (!missingTo.empty()) return "ERROR: " + missingTo;
    toScreen(session, fromX, fromY);
    toScreen(session, toX, toY);

    const std::string buttonName = stringField(input, "button", "left");
    const uint8_t mask = input::buttonMaskForName(buttonName);
    if (mask == 0) return "ERROR: unknown button '" + buttonName + "'";

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
    return captureInto(context, session, label.str());
}

std::string doScroll(Context& context, const json& input) {
    VncSession& session = sessionFrom(context);
    int x = 0, y = 0;
    const std::string missing = requirePoint(input, "x", "y", x, y);
    if (!missing.empty()) return "ERROR: " + missing;
    toScreen(session, x, y);

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

json objectSchema(json properties, std::vector<std::string> required) {
    return json{
        {"type", "object"},
        {"properties", std::move(properties)},
        {"required", std::move(required)},
    };
}

}  // namespace

void setCoordinateSpan(int span) { g_coordinateSpan = span > 0 ? span : 0; }
int  coordinateSpan() { return g_coordinateSpan; }

void setRequireZoom(bool require) { g_requireZoom = require; }
bool requireZoom() { return g_requireZoom; }

VncSession& sessionFrom(Context& context) {
    return *context.get<VncSession*>(keys::kSession);
}

std::vector<ToolSpec> makeComputerTools() {
    std::vector<ToolSpec> tools;

    tools.push_back(ToolSpec{
        "vnc_screenshot",
        "Take a screenshot of the remote screen. Use this to see the current state "
        "before deciding what to do, or to confirm the result of an earlier action.",
        objectSchema(json::object(), {}),
        doScreenshot, ""});

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
        json props = json{
            {"x", {{"type", "integer"},
                   {"description", "Left edge of the region, in screen pixels"}}},
            {"y", {{"type", "integer"},
                   {"description", "Top edge of the region, in screen pixels"}}},
            {"width",  {{"type", "integer"}, {"minimum", kMinZoomWidth}, {"maximum", kMaxZoomWidth},
                        {"description", "Width of the region in screen pixels. Defaults to 320, "
                                        "maximum 640."}}},
            {"height", {{"type", "integer"}, {"minimum", kMinZoomHeight}, {"maximum", kMaxZoomHeight},
                        {"description", "Height of the region in screen pixels. Defaults to 100, "
                                        "maximum 200. "
                                        "How precisely you can aim inside the result depends on "
                                        "this: keep it small — around 100, roughly five rows of a "
                                        "list — when you need to pick one row out of several. A "
                                        "tall region is for getting your bearings, not for aiming."}}},
        };
        tools.push_back(ToolSpec{
            "vnc_zoom",
            "Look closely at one part of the screen. Returns just that region, enlarged, "
            "instead of the whole screen shrunk down.\n"
            "Use this whenever you need to tell apart things that are close together — "
            "which row of a file list or menu is which, the exact text in a small label, "
            "the state of a checkbox — and especially before clicking something in a "
            "dense list. A full screenshot has to represent the entire screen at once, so "
            "fine detail is lost; a zoom spends all of that detail on the region you name.\n"
            "The result is ruled and gridded in screen coordinates, so you do not have to "
            "judge where something is — you can read it off. Find the target, look at the "
            "gridlines that bracket it, read the numbers on the rulers, and click that "
            "position with vnc_click.\n"
            "The region must actually contain what you are aiming at, with some room around "
            "it. That comes first: a region too small to include the target is useless no "
            "matter how finely it is gridded, and clicking inside it anyway just puts the "
            "pointer on empty background. Give yourself margin — if you want one of a row of "
            "buttons, take in the whole row rather than the spot you think one of them "
            "occupies.\n"
            "Within that, smaller is better: the smaller the region, the finer the grid and "
            "the more precisely you can read a position off it. So work in two steps when it "
            "matters — one wider zoom to see where the target really is, then a tighter one "
            "centred on it. If a click lands on nothing, widen the region and look again "
            "before assuming you misread the numbers; most likely the target was never in "
            "the view.",
            objectSchema(props, {"x", "y"}), doZoom, ""});
    }

    if (!kRulerZoom) {
        json props = json{
            {"x", {{"type", "integer"},
                   {"description", std::string("Horizontal position within the zoomed image, "
                                               "0 at its left edge.") + kZoomPixelNote}}},
            {"y", {{"type", "integer"},
                   {"description", std::string("Vertical position within the zoomed image, "
                                               "0 at its top edge.") + kZoomPixelNote}}},
        };
        props["button"] = {{"type", "string"}, {"enum", {"left", "middle", "right"}},
                           {"description", "Mouse button to click. Defaults to left."}};
        props["clicks"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 3},
                           {"description", "Number of clicks; use 2 for a double-click. Defaults to 1."}};
        tools.push_back(ToolSpec{
            "vnc_click_zoom",
            "Click something visible in the most recent vnc_zoom image, giving its "
            "position within that image rather than on the screen. The enlarged view is "
            "translated back to the real screen position for you.\n"
            "Prefer this over vnc_click for anything small or tightly packed: aiming "
            "inside an enlarged view is far more accurate than aiming at a full "
            "screenshot. Returns a normal full-screen screenshot so you can confirm the "
            "result. Requires a vnc_zoom first.",
            objectSchema(props, {"x", "y"}), doClickZoom, ""});
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
        "Wait for the screen to change on its own — while an application starts, a page "
        "loads, or an installer progresses — then take a screenshot.",
        objectSchema(json{{"ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", kMaxWaitMs},
                                  {"description", "Milliseconds to wait. Defaults to 1000."}}}},
                     {}),
        doWait, ""});

    return tools;
}

}  // namespace tapto
