// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#include "tapto/input_map.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>

#include "tapto/vnc_session.h"

namespace tapto::input {
namespace {

std::string toLower(const std::string& value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// X11 keysyms. Printable ASCII maps to its own code point; everything else
// comes from the 0xFF00 "function key" page of keysymdef.h.
const std::map<std::string, uint32_t>& namedKeys() {
    static const std::map<std::string, uint32_t> kKeys = {
        {"return", 0xFF0D}, {"enter", 0xFF0D},
        {"backspace", 0xFF08}, {"bksp", 0xFF08},
        {"tab", 0xFF09},
        {"escape", 0xFF1B}, {"esc", 0xFF1B},
        {"space", 0x0020},
        {"delete", 0xFFFF}, {"del", 0xFFFF},
        {"insert", 0xFF63}, {"ins", 0xFF63},
        {"home", 0xFF50}, {"end", 0xFF57},
        {"pageup", 0xFF55}, {"pgup", 0xFF55},
        {"pagedown", 0xFF56}, {"pgdn", 0xFF56},
        {"left", 0xFF51}, {"up", 0xFF52}, {"right", 0xFF53}, {"down", 0xFF54},
        {"printscreen", 0xFF61}, {"pause", 0xFF13}, {"menu", 0xFF67},
        {"capslock", 0xFFE5}, {"numlock", 0xFF7F}, {"scrolllock", 0xFF14},
        // Modifiers, usable both as a chord prefix and on their own.
        {"shift", 0xFFE1}, {"shift_l", 0xFFE1}, {"shift_r", 0xFFE2},
        {"ctrl", 0xFFE3}, {"control", 0xFFE3}, {"ctrl_l", 0xFFE3}, {"ctrl_r", 0xFFE4},
        {"alt", 0xFFE9}, {"alt_l", 0xFFE9}, {"alt_r", 0xFFEA}, {"altgr", 0xFFEA},
        {"super", 0xFFEB}, {"win", 0xFFEB}, {"windows", 0xFFEB}, {"meta", 0xFFEB},
        {"cmd", 0xFFEB},
    };
    return kKeys;
}

// Characters a US layout produces only with Shift held.
const std::string kShiftedSymbols = "~!@#$%^&*()_+{}|:\"<>?";

// A layout entry says: to get `target` on the remote machine, press the key
// that a US keyboard would use for `usEquivalent`, optionally with AltGr.
//
// Read an entry as a statement about *key positions*. On a Swedish layout ':'
// sits at Shift+period; a US keyboard puts '>' there; so to type ':' we send
// the keysym for '>'.
//
// A `usEquivalent` of 0 marks the character unreachable: the layout puts it
// somewhere a US-position mapping cannot address. Refusing is deliberate —
// typing a silently different character is worse than not typing at all,
// because the model would have no way to tell.
struct LayoutEntry {
    char target;
    char usEquivalent;
    bool altgr;
};

// Swedish (sv-SE). Verified empirically against a Windows guest — see
// docs/keyboard-layouts.md before changing anything here.
const LayoutEntry kSwedish[] = {
    // Number row, shifted.
    {'"',  '@',  false},
    {'&',  '^',  false},
    {'/',  '&',  false},
    {'(',  '*',  false},
    {')',  '(',  false},
    {'=',  ')',  false},
    {'?',  '_',  false},
    {'+',  '-',  false},
    // Bottom row: Swedish shifts ',' and '.' into ';' and ':', and moves '-'
    // onto the US '/' position.
    {';',  '<',  false},
    {':',  '>',  false},
    {'-',  '/',  false},
    {'_',  '?',  false},
    // AltGr layer.
    {'@',  '2',  true},
    {'$',  '4',  true},
    {'{',  '7',  true},
    {'[',  '8',  true},
    {']',  '9',  true},
    {'}',  '0',  true},
    {'\\', '-',  true},
    // Home row: the apostrophe key sits where US has backslash.
    {'\'', '\\', false},
    {'*',  '|',  false},
    // Not reachable by key position. '<', '>' and '|' live on the extra ISO
    // key left of Z, which a US ANSI keyboard does not have; '~', '^' and '`'
    // are dead keys needing a following space. Verified against a Windows
    // guest: sending these as-is produced ';', ':', '*', '%' and '&'.
    //
    // On Windows the Alt+numpad fallback types all six correctly (verified),
    // so marking them here routes them there rather than losing them.
    {'<',  0,    false},
    {'>',  0,    false},
    {'|',  0,    false},
    {'~',  0,    false},
    {'^',  0,    false},
    {'`',  0,    false},
};

struct Layout {
    const char*         name;
    const LayoutEntry*  entries;
    size_t              count;
};

const Layout kLayouts[] = {
    {"us", nullptr, 0},   // identity: keysym is the character itself
    {"sv", kSwedish, sizeof(kSwedish) / sizeof(kSwedish[0])},
};

std::string g_layout = "us";
bool g_altCodeFallback = false;

const Layout* currentLayout() {
    for (const Layout& layout : kLayouts) {
        if (g_layout == layout.name) return &layout;
    }
    return &kLayouts[0];
}

}  // namespace

bool setRemoteLayout(const std::string& name) {
    const std::string lower = toLower(name);
    for (const Layout& layout : kLayouts) {
        if (lower == layout.name) {
            g_layout = lower;
            return true;
        }
    }
    return false;
}

const std::string& remoteLayout() { return g_layout; }

std::vector<std::string> availableLayouts() {
    std::vector<std::string> names;
    for (const Layout& layout : kLayouts) names.push_back(layout.name);
    return names;
}

void setAltCodeFallback(bool enabled) { g_altCodeFallback = enabled; }
bool altCodeFallback() { return g_altCodeFallback; }

TypePlan planForChar(char c) {
    TypePlan plan;
    if (strokeForChar(c, plan.stroke)) {
        plan.method = TypeMethod::Direct;
        return plan;
    }

    // Unreachable through the layout. Alt+0NNN addresses it by code point
    // instead of by key position, so the layout stops mattering.
    const unsigned char u = static_cast<unsigned char>(c);
    if (g_altCodeFallback && u >= 0x20 && u != 0x7F) {
        plan.method = TypeMethod::AltCode;
        plan.code = static_cast<int>(u);
        return plan;
    }

    plan.method = TypeMethod::Unavailable;
    return plan;
}

bool strokeForChar(char c, KeyStroke& stroke) {
    const Layout* layout = currentLayout();

    char send = c;
    bool altgr = false;
    for (size_t i = 0; i < layout->count; ++i) {
        if (layout->entries[i].target == c) {
            if (layout->entries[i].usEquivalent == 0) return false;  // unreachable
            send  = layout->entries[i].usEquivalent;
            altgr = layout->entries[i].altgr;
            break;
        }
    }

    const uint32_t keysym = keysymForChar(send);
    if (keysym == 0) return false;

    stroke.keysym = keysym;
    // AltGr selects the third level, where Shift plays no part.
    stroke.shift = !altgr && charNeedsShift(send);
    stroke.altgr = altgr;
    return true;
}

uint32_t keysymForChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    // Tab, newline and carriage return arrive inside typed text; map them to
    // the corresponding key rather than dropping them.
    if (c == '\n' || c == '\r') return 0xFF0D;
    if (c == '\t') return 0xFF09;
    if (u >= 0x20 && u <= 0x7E) return u;
    return 0;
}

bool charNeedsShift(char c) {
    if (c >= 'A' && c <= 'Z') return true;
    return kShiftedSymbols.find(c) != std::string::npos;
}

bool keysymForName(const std::string& name, uint32_t& keysym) {
    if (name.empty()) return false;

    // Single printable character, e.g. "a" or "7".
    if (name.size() == 1) {
        const uint32_t sym = keysymForChar(name[0]);
        if (sym != 0) {
            keysym = sym;
            return true;
        }
        return false;
    }

    const std::string lower = toLower(name);

    // Function keys F1..F24 occupy a contiguous range.
    if (lower.size() >= 2 && lower[0] == 'f' &&
        std::all_of(lower.begin() + 1, lower.end(),
                    [](unsigned char c) { return std::isdigit(c); })) {
        const int index = std::atoi(lower.c_str() + 1);
        if (index >= 1 && index <= 24) {
            keysym = 0xFFBE + static_cast<uint32_t>(index - 1);
            return true;
        }
        return false;
    }

    const auto it = namedKeys().find(lower);
    if (it == namedKeys().end()) return false;
    keysym = it->second;
    return true;
}

bool parseChord(const std::string& spec, Chord& chord, std::string& error) {
    chord.modifiers.clear();
    chord.key = 0;

    if (spec.empty()) {
        error = "empty key specification";
        return false;
    }

    // Split on '+', but treat a trailing '+' as the literal plus key so that
    // "ctrl++" (zoom in) is expressible.
    std::vector<std::string> parts;
    std::string current;
    for (size_t i = 0; i < spec.size(); ++i) {
        if (spec[i] == '+' && !current.empty()) {
            parts.push_back(current);
            current.clear();
        } else if (spec[i] == '+' && current.empty() && i + 1 == spec.size()) {
            current = "+";
        } else {
            current.push_back(spec[i]);
        }
    }
    if (!current.empty()) parts.push_back(current);

    if (parts.empty()) {
        error = "empty key specification";
        return false;
    }

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        uint32_t sym = 0;
        if (!keysymForName(parts[i], sym)) {
            error = "unknown modifier '" + parts[i] + "'";
            return false;
        }
        chord.modifiers.push_back(sym);
    }

    if (!keysymForName(parts.back(), chord.key)) {
        error = "unknown key '" + parts.back() + "'";
        return false;
    }
    return true;
}

uint8_t buttonMaskForName(const std::string& name) {
    const std::string lower = toLower(name);
    if (lower.empty() || lower == "left")  return kMouseLeft;
    if (lower == "middle")                 return kMouseMiddle;
    if (lower == "right")                  return kMouseRight;
    return 0;
}

}  // namespace tapto::input
