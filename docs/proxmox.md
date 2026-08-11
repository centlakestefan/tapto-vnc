# Proxmox console support (planned)

Proxmox's web console is noVNC, which is structurally the same as VMware
WebMKS: RFB tunnelled over a TLS WebSocket, gated by a short-lived ticket from
a REST API. `VncSession::connectWebSocket()` already handles that shape, so the
work is a new console module plus one change to the vendored transport.

## Flow

All on port **8006**. Self-signed certificate by default — already tolerated,
since `parseUrl()` disables verification for `wss://`.

| Step | Call | Yields |
| --- | --- | --- |
| 1 | `POST /api2/json/access/ticket` (username, password) | `ticket`, `CSRFPreventionToken` |
| 2 | `POST /api2/json/nodes/{node}/qemu/{vmid}/vncproxy` with `websocket=1` | `ticket`, `port`, `user` |
| 3 | connect | `wss://{host}:8006/api2/json/nodes/{node}/qemu/{vmid}/vncwebsocket?port={port}&vncticket={urlencoded}` |

An **API token** replaces step 1 entirely and avoids the CSRF token:
`Authorization: PVEAPIToken=user@realm!tokenid=uuid`. Prefer it.

LXC containers use `/lxc/{vmid}/` in place of `/qemu/{vmid}/`.

## Blocking change: Cookie header on the WebSocket handshake

Proxmox requires `Cookie: PVEAuthCookie=<ticket>` on the upgrade request.
`WebSocketTransport::performClientHandshake()` currently writes a fixed header
list with no extension point, so it needs a `setExtraHeaders()` — a third entry
in `PATCHES.md` — and `VncSession::connectWebSocket()` needs to pass them
through.

This is the same class of problem as the WebMKS `Sec-WebSocket-Protocol: binary`
requirement: invisible from the code, and the server rejects the upgrade without
it. Probe the handshake response before assuming anything.

## Open questions — verify, do not assume

Both are recollection, not measurement:

1. **RFB-layer authentication.** Believed to be standard VNC Authentication with
   the `vncproxy` ticket as the password, which `connectWebSocket(url, password)`
   already supports. But VNC auth uses only the first 8 characters of a password
   and Proxmox tickets are long — that only works if both ends truncate the same
   way. Confirm on the wire.
2. **Node discovery.** `GET /api2/json/nodes` may make `node` discoverable
   rather than a required config key.

Settle both the way the WebMKS handshake was settled: build the URL by hand,
point `vnc-smoketest` at it, and read what comes back.

## Config keys to add

Mirroring the `vcenter-*` set:

```
proxmox-host, proxmox-node, proxmox-user, proxmox-password | proxmox-token,
proxmox-insecure
```

## Note on the nested test node

Proxmox running as a VMware guest needs **"Expose hardware assisted
virtualization to the guest OS"** enabled on the VMware VM, or its own VMs will
refuse to start (or fall back to emulation and crawl).
