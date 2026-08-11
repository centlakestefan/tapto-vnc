// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Centlake Software AB

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tapto::input {

// RFB carries X11 keysyms, not scancodes, so every key the model names has to
// be translated into one before it can be sent.

// Keysym for a printable ASCII character, or 0 if it has none.
uint32_t keysymForChar(char c);

// True when a US-layout keyboard produces `c` only with Shift held. RFC 6143
// leaves it to the client to send the modifier, and Windows guests in
// particular will otherwise receive the unshifted character.
bool charNeedsShift(char c);

// --- Remote keyboard layout ------------------------------------------------
//
// RFB carries keysyms, but VMware WebMKS resolves each keysym to a *US key
// position* and the guest then applies its own layout. On a guest configured
// for anything other than US, punctuation therefore arrives as whatever that
// layout puts at the same position: typing ':' on a Swedish guest yields 'Ö'.
//
// The fix is to send the keysym whose US position coincides with the position
// that produces the wanted character on the guest's layout. Letters and digits
// sit in the same place on all the Latin layouts, so only punctuation needs a
// translation table.

// One physical keypress: which keysym to send and which modifiers to hold.
struct KeyStroke {
    uint32_t keysym = 0;
    bool     shift  = false;
    bool     altgr  = false;   // right Alt / ISO_Level3_Shift
};

// Selects the layout the *remote* machine is configured for. Returns false and
// leaves the current layout untouched if the name is unknown.
bool setRemoteLayout(const std::string& name);

// Name of the layout currently selected.
const std::string& remoteLayout();

// Every layout this build knows about, "us" first.
std::vector<std::string> availableLayouts();

// Keypress that produces `c` on the selected remote layout. Returns false when
// the character is not reachable — either it has no keysym, or the layout puts
// it on a key that a US-position mapping cannot address.
bool strokeForChar(char c, KeyStroke& stroke);

// How a character can be delivered.
enum class TypeMethod {
    Direct,       // one keypress, per the layout table
    AltCode,      // hold left Alt, type the decimal code on the numeric keypad
    Unavailable,  // no way to produce it
};

struct TypePlan {
    TypeMethod method = TypeMethod::Unavailable;
    KeyStroke  stroke;      // Direct only
    int        code = 0;    // AltCode only: the Windows ANSI decimal code
};

// Enables the Alt+numpad fallback for characters the layout cannot reach.
//
// Windows resolves Alt+0NNN in the keyboard driver, *before* the layout is
// applied, so it produces the same character on any layout. That makes it the
// natural escape hatch for the handful of characters stranded on ISO-only or
// dead keys.
//
// It only ever applies to characters that would otherwise be skipped, so on a
// Windows guest it is strictly an improvement. It is off for non-Windows
// guests because Alt+digit is a window-manager shortcut on many Linux desktops,
// where firing it would be worse than typing nothing. It also requires NumLock
// to be on: with NumLock off the keypad sends Home/End/arrows instead of digits.
void setAltCodeFallback(bool enabled);
bool altCodeFallback();

// Decides how to type `c` on the current layout.
TypePlan planForChar(char c);

// Resolves a key name to a keysym: "a", "Return", "esc", "F5", "pagedown".
// Matching is case-insensitive. Returns false if the name is unknown.
bool keysymForName(const std::string& name, uint32_t& keysym);

// A key combination such as "ctrl+alt+delete": zero or more modifiers held
// while one main key is pressed and released.
struct Chord {
    std::vector<uint32_t> modifiers;
    uint32_t              key = 0;
};

// Parses "ctrl+c", "alt+F4", "Return". On failure returns false and sets
// `error` to a message naming the offending component.
bool parseChord(const std::string& spec, Chord& chord, std::string& error);

// Button-mask bit for "left" / "middle" / "right". Returns 0 if unknown.
uint8_t buttonMaskForName(const std::string& name);

}  // namespace tapto::input
