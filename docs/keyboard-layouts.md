# Remote keyboard layouts

## Why a table is needed

RFB carries X11 **keysyms**, not scancodes. VMware WebMKS resolves each keysym
to a **US key position**, and the guest OS then applies its own layout to that
position. So the character that arrives is whatever the guest's layout puts
where a US keyboard puts the one we asked for.

Letters and digits sit in the same place on every Latin layout, so they are
always fine. Punctuation is not: typing `:` on a Swedish guest produced `Ö`,
because US `Shift+;` is Swedish `Shift+ö`.

## How an entry is read

Each entry answers: *to get this character on the guest, which key would a US
keyboard use?*

```
{':',  '>',  false},   // Swedish ':' is Shift+period; US puts '>' there
{'@',  '2',  true},    // Swedish '@' is AltGr+2
{'<',  0,    false},   // unreachable — see below
```

A `usEquivalent` of `0` marks the character **not reachable by key position**.
`vnc_type` then falls back to Alt+numpad (below), or — if that is disabled —
skips it and says so. It never types a *different* character silently: the
model has no way to detect a substitution, so a wrong character is worse than a
missing one.

## The Alt+numpad fallback

Windows resolves `Alt+0NNN` (decimal ANSI code, typed on the numeric keypad) in
the keyboard driver, **before** the layout is applied. The result is therefore
the same on any layout, which makes it a clean escape hatch for characters the
position mapping cannot reach.

Controlled by `--altcode` / config key `altcode-fallback`:

| Mode | Behaviour |
| --- | --- |
| `auto` (default) | Enabled when the layout is not `us` — where nothing is stranded anyway |
| `on` | Always enabled |
| `off` | Never; unreachable characters are skipped |

Requirements and limits:

- **Windows only.** On many Linux desktops `Alt`+digit is a window-manager
  shortcut, so firing it would be worse than typing nothing. Use `off` there.
- **NumLock must be on.** With NumLock off the keypad sends Home/End/arrows
  instead of digits.
- Uses **left** Alt. AltGr is Ctrl+Alt on these layouts and does not trigger
  alt codes.
- Costs 5 keystrokes per character, so it is a fallback, not the main path.

## Verifying a layout

Use `--type-test`, which drives `vnc_type` directly with no model involved:

```
tapto-vnc --vm <VM> --layout sv --type-test "a:b;c-d_e+f?g(h)i=j/k&l@m"
```

It types into whatever has focus and writes `layout-test.png`. Open a text
editor on the guest first, then compare the image against the string you sent.

Note that the shell will mangle some characters before they reach the program.
PowerShell strips `"` even after `--%`, and treats `|`, `<`, `>` as operators;
Git Bash single quotes pass `"` through correctly.

## Swedish (`sv`) — verified against a Windows guest

Direct, by key position: `! " # % & / ( ) = ? + - _ : ; * ' @ $ { } [ ] \`

Via Alt+numpad: `< > | ~ ^ ` ` — all six verified typing correctly into
Notepad on a Windows guest.

`<`, `>` and `|` live on the extra ISO key left of Z, which a US ANSI keyboard
does not have, so no US position addresses them. `~`, `^` and `` ` `` are dead
keys on this layout, needing a following space to emit the bare character.
Sending them unmapped produced `;`, `:`, `*`, `%` and `&` respectively — which
is why they are marked and routed to the fallback instead.

With `altcode-fallback = off`, those six are skipped and reported.

## Adding a layout

1. Add a `LayoutEntry` array in `src/input_map.cpp` next to `kSwedish`.
2. Register it in `kLayouts`.
3. Verify with `--type-test` against a real guest and record the result here.

Derive entries by asking, for each character, which physical key and modifier
produces it on the target layout, then which character a US keyboard puts at
that same key and modifier. That US character is the `usEquivalent`.
