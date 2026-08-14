# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Centlake Software AB
#
# A stub RFB 3.8 server that completes the handshake and then misbehaves on
# purpose. It exists to make console failures reproducible without a VM: a real
# drop happens when it happens, which is no way to test the code that handles
# one.
#
# It speaks just enough to get a client connected -- version exchange, security
# type None, ServerInit for a 640x480 true-colour screen -- and never sends a
# framebuffer update. That is deliberate: the point is the shape of the
# disconnect, not the picture.
#
# Modes:
#   reset   close the connection abortively, so the peer gets an RST. This is
#           the fault that used to be invisible: on Windows a reset socket
#           polls as POLLHUP/POLLERR and never POLLIN, so a client watching
#           only for readable data sees an idle connection, not a dead one.
#   fin     shut the connection down cleanly, so the peer reads end-of-stream.
#           The ordinary case, and the one that always worked -- worth keeping
#           because a fix for the reset case must not break it.
#   hold    stay connected and send nothing at all. Not a disconnect: this is
#           the console that stops responding without ever closing, which
#           nothing currently detects, and it is also the healthy-server
#           control for checking that a client is not declared dead too eagerly.
#
# Usage, with the client pointed at --host 127.0.0.1 --display 99:
#   .\tools\fake-rfb.ps1 -Mode reset -After 1
#   .\tools\fake-rfb.ps1 -Mode fin
#   .\tools\fake-rfb.ps1 -Mode hold
param(
    [int]$Port = 5999,
    [ValidateSet("reset", "fin", "hold")]
    [string]$Mode = "hold",
    [int]$After = 3,          # seconds after the handshake before closing
    [int]$HoldSeconds = 120   # how long "hold" stays up before giving up
)

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
Write-Output "listening on 127.0.0.1:$Port (display $($Port - 5900)) mode=$Mode"

$client = $listener.AcceptTcpClient()
$s = $client.GetStream()
Write-Output "client connected from $($client.Client.RemoteEndPoint)"

function Read-Exact([int]$n) {
    $buf = New-Object byte[] $n
    $got = 0
    while ($got -lt $n) {
        $r = $s.Read($buf, $got, $n - $got)
        if ($r -le 0) { throw "peer closed during handshake" }
        $got += $r
    }
    return $buf
}
function Write-Bytes([byte[]]$b) { $s.Write($b, 0, $b.Length); $s.Flush() }
function Drain {
    # Read whatever is buffered. Closing a socket that still has unread data
    # makes Windows send a reset, so "fin" has to drain first to be a real FIN.
    $sink = New-Object byte[] 4096
    try { while ($s.DataAvailable) { $null = $s.Read($sink, 0, $sink.Length) } } catch {}
}

# 1. ProtocolVersion
Write-Bytes ([System.Text.Encoding]::ASCII.GetBytes("RFB 003.008`n"))
Write-Output "client version: $(([System.Text.Encoding]::ASCII.GetString((Read-Exact 12))).Trim())"

# 2. Security: offer None only, then report success
Write-Bytes ([byte[]](1, 1))
Write-Output "security chosen: $((Read-Exact 1)[0])"
Write-Bytes ([byte[]](0, 0, 0, 0))

# 3. ClientInit
Write-Output "shared flag: $((Read-Exact 1)[0])"

# 4. ServerInit: 640x480, 32bpp true colour, named "fake"
$name = [System.Text.Encoding]::ASCII.GetBytes("fake")
$init = [System.Collections.Generic.List[byte]]::new()
$init.AddRange([byte[]](2, 128))                  # width  640
$init.AddRange([byte[]](1, 224))                  # height 480
$init.AddRange([byte[]](32, 24, 0, 1))            # bpp, depth, big-endian, true-colour
$init.AddRange([byte[]](0, 255, 0, 255, 0, 255))  # red/green/blue max
$init.AddRange([byte[]](16, 8, 0))                # red/green/blue shift
$init.AddRange([byte[]](0, 0, 0))                 # padding
$init.AddRange([byte[]](0, 0, 0, $name.Length))
$init.AddRange($name)
Write-Bytes $init.ToArray()
Write-Output "server init sent; the client is now connected"

switch ($Mode) {
    "reset" {
        Start-Sleep -Seconds $After
        # SO_LINGER with a zero timeout makes Close() send a reset rather than
        # a FIN, so the test does not depend on there happening to be unread
        # data in the receive queue.
        $client.LingerState = New-Object System.Net.Sockets.LingerOption($true, 0)
        $client.Close()
        Write-Output "reset the connection"
    }
    "fin" {
        Start-Sleep -Seconds $After
        Drain
        $client.Client.Shutdown([System.Net.Sockets.SocketShutdown]::Send)
        Write-Output "sent FIN; the client should read end-of-stream"
        Start-Sleep -Seconds 2
        $client.Close()
    }
    "hold" {
        for ($i = 0; $i -lt $HoldSeconds; $i++) {
            Drain
            if (-not $client.Connected) { break }
            Start-Sleep -Seconds 1
        }
        Write-Output "held for $HoldSeconds seconds"
        $client.Close()
    }
}

$listener.Stop()
Write-Output "server done"
