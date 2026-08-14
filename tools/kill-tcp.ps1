# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Centlake Software AB
#
# Abort an established IPv4 TCP connection, the way a network fault does.
#
# There is no built-in Windows command for this -- netsh cannot do it and
# neither can Get-NetTCPConnection, which only reports. The mechanism is an
# API: SetTcpEntry with MIB_TCP_STATE_DELETE_TCB (12) deletes the connection's
# TCB, so the local socket is torn down and the peer receives an RST. It is
# what Sysinternals TCPView's "Close Connection" and CurrPorts /close call.
#
# The point of having it here is that a console dropping mid-run is the failure
# this project keeps hitting, and it is otherwise only reproducible by waiting
# for it. Killing the connection under a running agent exercises the whole
# recovery path on demand: the drop being noticed while reading rather than at
# the next send, the reason reaching the trace, and the reconnect.
#
# Requires an elevated shell. IPv4 only -- there is no IPv6 equivalent.
#
#   .\tools\kill-tcp.ps1 -ProcessName tapto-vnc -List
#   .\tools\kill-tcp.ps1 -ProcessName tapto-vnc -RemotePort 443
#   .\tools\kill-tcp.ps1 -LocalPort 54321
param(
    [string]$ProcessName,
    [int]$RemotePort = 0,
    [int]$LocalPort = 0,
    [switch]$List,
    [switch]$Force
)

Add-Type -Namespace Net -Name Tcp -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct MIB_TCPROW {
    public uint dwState;
    public uint dwLocalAddr;
    public uint dwLocalPort;
    public uint dwRemoteAddr;
    public uint dwRemotePort;
}
[DllImport("iphlpapi.dll", SetLastError=true)]
public static extern int SetTcpEntry(ref MIB_TCPROW row);
'@

# MIB_TCPROW holds ports in network byte order in the low 16 bits, and
# addresses as an in_addr -- which is what GetAddressBytes already gives us.
function ToNetPort([int]$p) { return [uint32]((($p -band 0xFF) -shl 8) -bor (($p -shr 8) -band 0xFF)) }
function ToNetAddr([string]$a) {
    return [BitConverter]::ToUInt32([System.Net.IPAddress]::Parse($a).GetAddressBytes(), 0)
}

$conns = Get-NetTCPConnection -State Established -ErrorAction Stop |
         Where-Object { $_.LocalAddress -notmatch ':' }   # IPv4 only

if ($ProcessName) {
    # Not an error worth a stack trace: the agent having already exited is the
    # normal state when someone comes looking for its connection.
    $pids = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue).Id
    if (-not $pids) { Write-Output "no process named '$ProcessName' is running"; return }
    $conns = $conns | Where-Object { $pids -contains $_.OwningProcess }
}
if ($RemotePort) { $conns = $conns | Where-Object { $_.RemotePort -eq $RemotePort } }
if ($LocalPort)  { $conns = $conns | Where-Object { $_.LocalPort  -eq $LocalPort  } }

$conns = @($conns)
if (-not $conns) { Write-Output "no matching established IPv4 connection"; return }

# A filter that is too loose is easy to write and expensive to run: -RemotePort
# 443 on its own matches every HTTPS connection on the machine, and this resets
# what it matches. One match is almost always what was meant, so anything more
# has to be asked for.
if ($conns.Count -gt 1 -and -not $List -and -not $Force) {
    Write-Output "$($conns.Count) connections match; refusing to reset them all."
    Write-Output "Narrow the filter (-ProcessName with -RemotePort), or pass -Force if you meant it:"
    foreach ($c in $conns) {
        Write-Output ("  {0}:{1} -> {2}:{3} (pid {4})" -f $c.LocalAddress, $c.LocalPort, $c.RemoteAddress, $c.RemotePort, $c.OwningProcess)
    }
    return
}

foreach ($c in $conns) {
    $label = "{0}:{1} -> {2}:{3} (pid {4})" -f $c.LocalAddress, $c.LocalPort, $c.RemoteAddress, $c.RemotePort, $c.OwningProcess
    if ($List) { Write-Output $label; continue }

    $row = New-Object Net.Tcp+MIB_TCPROW
    $row.dwState      = 12                          # MIB_TCP_STATE_DELETE_TCB
    $row.dwLocalAddr  = ToNetAddr $c.LocalAddress
    $row.dwLocalPort  = ToNetPort $c.LocalPort
    $row.dwRemoteAddr = ToNetAddr $c.RemoteAddress
    $row.dwRemotePort = ToNetPort $c.RemotePort

    $rc = [Net.Tcp]::SetTcpEntry([ref]$row)
    if ($rc -eq 0) {
        Write-Output "killed  $label"
    } elseif ($rc -eq 5 -or $rc -eq 317) {
        # 5 is ERROR_ACCESS_DENIED; 317 is what an unelevated call returns here
        # instead, which is not the documented code but is the same cause.
        Write-Output "DENIED  $label  (run this shell as Administrator)"
    } else {
        Write-Output "failed  $label  (SetTcpEntry returned $rc)"
    }
}
