# Local patches to the vendored RFB sources

These files are copied from the `cpp-vnc` project (client-side sources only —
the VNC server, its tests, and its example binaries are not vendored). Changes
made here are listed below so they can be re-applied after a re-vendor, and
ideally upstreamed.

---

## 1. `rfb_websocket.cpp` — send `Sec-WebSocket-Protocol: binary`

**Location:** `WebSocketTransport::performClientHandshake()`

**Change:** add `Sec-WebSocket-Protocol: binary` to the HTTP upgrade request.

**Why:** RFB over WebSocket is always a binary stream, and a server that
negotiates subprotocols will refuse an upgrade that offers none. VMware WebMKS
(ESXi, port 443, path `/ticket/<ticket>`) closes the TCP connection with zero
bytes rather than returning an HTTP error, so the failure surfaced as an opaque
`Failed to receive TLS data` thrown by the first read after a *successful* TLS
handshake.

Verified against ESXi directly:

| Upgrade request | Server response |
| --- | --- |
| without the header | connection closed, 0 bytes |
| with `Sec-WebSocket-Protocol: binary` | `HTTP/1.1 101 Switching Protocols`, then the `RFB 003.008` banner |

---

## 2. `rfb_encoding_hextile.cpp` — clip subrects in x and y

**Location:** `decodeHextile()`, the subrectangle write loop.

**Change:** bound each write by `x < width` and `y < height` separately, instead
of only checking the linear index.

**Why:** `if (index < pixel_count)` alone lets a subrect that overruns the right
edge of the rectangle wrap onto the next row, painting its colour into unrelated
pixels. The encoded nibbles permit `sx + sw` up to 31, past the 16-pixel tile.

**This is hardening, not a bug fix.** It was measured against a real server and
changes nothing there, because a conforming server keeps subrects inside their
tile. Keep that in mind when investigating a rendering problem — this patch is
not a candidate explanation, and it was temporarily reverted once to establish
exactly that:

| Comparison | Differing pixels |
| --- | --- |
| raw vs hextile, fix applied | 0 |
| raw vs hextile, fix reverted | 0 |
| hextile with fix vs without | 1584 (0.097%) |
| raw vs raw, same interval (control) | 1584 (0.097%) |

The last two rows are the same number: that difference is the taskbar clock
ticking between captures, not the patch.

If a rendering question comes up again, revert this patch first so the decoder
is byte-for-byte upstream — an unexplained local change is one more thing to
rule out, and this one demonstrably explains nothing.

(The colour artifacts that prompted the original investigation turned out to be
ClearType subpixel antialiasing on the guest: at full resolution 67% of text
pixels carry a colour spread above 40. That is the guest's font rendering
faithfully transmitted, not a decoding fault.)

---

## Upstream issues found but *not* patched here

Worked around in `src/vnc_session.cpp` instead, to keep the vendored diff small.

- **`Client::setPixelFormat()` desynchronizes the decoder.** It sends the
  SetPixelFormat message but never updates `m_serverInit.pixelFormat`, which
  every decode path reads for bytes-per-pixel. Calling it against a server that
  honours the request corrupts all subsequent rectangles. `VncSession` therefore
  never calls it and decodes in the server's advertised format.

- **`WebSocketTransport::parseUrl()` treats everything after the host as the
  request path**, so a stray control character in the URL silently corrupts the
  HTTP upgrade request. `VncSession::connectWebSocket()` rejects URLs containing
  whitespace up front.

- **`parseUrl()` hardcodes `m_tlsVerify = false` for every `wss://` URL**
  (with a `// testing` comment), so TLS certificates are never verified and
  `setTLSVerify()` cannot re-enable it for URL-based connections.
