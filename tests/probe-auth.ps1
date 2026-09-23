<#
==========================================================================================
 probe-auth.ps1 - byte level AUTH_LOGON_CHALLENGE probe for an AzerothCore authserver
==========================================================================================

  The supervisor probes authserver with one AUTH_LOGON_CHALLENGE for a random non-existent
  account and expects a 3 byte answer (00 00 04 = WOW_FAIL_UNKNOWN_ACCOUNT).  When that
  fails, the supervisor only reports a short summary ("short/empty auth response",
  "no response to AUTH_LOGON_CHALLENGE", "connect failed").  This script does the same
  handshake and prints exactly what came back, byte by byte, plus who owns the port - which
  is what you need to tell "the authserver rejected my packet" apart from
  "something else is listening on that port".

  Usage:
    pwsh -File probe-auth.ps1
    pwsh -File probe-auth.ps1 -Target 127.0.0.1 -Port 3724 -TimeoutMs 3000
    pwsh -File probe-auth.ps1 -Account acprobe1          # fix the account name

  Verdicts:
    00 00 04      an AzerothCore authserver answered WOW_FAIL_UNKNOWN_ACCOUNT -> healthy,
                  identical to what the supervisor expects
    00 00 xx      AzerothCore answered another failure code (0x03 banned, 0x08 db busy,
                  0x09 version invalid, ...)
    00 ...        a full challenge was returned: the account EXISTS, pick another name
    0 bytes       the peer closed the connection without answering - the supervisor reports
                  this as "short/empty auth response"
    (timeout)     nothing came back within the timeout - the supervisor reports this as
                  "no response to AUTH_LOGON_CHALLENGE"
==========================================================================================
#>
[CmdletBinding()]
param(
    [string]$Target = '127.0.0.1',
    [int]$Port = 3724,
    [int]$TimeoutMs = 3000,
    [string]$Account = ''
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Account)) {
    $Account = 'acprobe{0:x6}' -f ((Get-Random -Minimum 0 -Maximum 0xFFFFFF))
}

# ---- the exact packet the supervisor sends ------------------------------------------------
# struct sAuthLogonChallenge_C (AzerothCore, #pragma pack(1), 35 bytes):
#   cmd(1) error(1) size(2, big endian) gamename[4] version1..3(3) build(2, LE)
#   platform[4] os[4] country[4] timezone_bias(4) ip(4) I_len(1) I[*]
# size = 30 + account length   (server: size - (35 - 4 - 1) == I_len)
function New-Challenge([string]$account) {
    $size = 30 + $account.Length
    $bytes = New-Object System.Collections.Generic.List[byte]
    $bytes.Add(0x00)                                  # AUTH_LOGON_CHALLENGE
    $bytes.Add(0x08)                                  # error (unused by the server)
    $bytes.Add([byte](($size -shr 8) -band 0xFF))     # size, big endian
    $bytes.Add([byte]($size -band 0xFF))
    $bytes.AddRange([Text.Encoding]::ASCII.GetBytes("WoW`0"))
    $bytes.AddRange([byte[]](3, 3, 5))                # 3.3.5
    $bytes.AddRange([byte[]]((12340 -band 0xFF), ((12340 -shr 8) -band 0xFF)))
    $bytes.AddRange([Text.Encoding]::ASCII.GetBytes("x86`0"))
    $bytes.AddRange([Text.Encoding]::ASCII.GetBytes("Win`0"))
    $bytes.AddRange([Text.Encoding]::ASCII.GetBytes('enUS'))
    $bytes.AddRange([byte[]](0, 0, 0, 0))             # timezone bias
    $bytes.AddRange([byte[]](0, 0, 0, 127))           # ip 127.0.0.1 (little endian)
    $bytes.Add([byte]$account.Length)
    $bytes.AddRange([Text.Encoding]::ASCII.GetBytes($account))
    return $bytes.ToArray()
}

$packet = New-Challenge $Account
Write-Host "probe-auth: target $Target`:$Port, account '$Account', $($packet.Length) bytes to send" -ForegroundColor Cyan

# ---- who is listening on that port (if the cmdlet is available) ---------------------------
try {
    $owners = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction Stop
    if ($owners) {
        foreach ($o in $owners) {
            $proc = Get-Process -Id $o.OwningProcess -ErrorAction SilentlyContinue
            $name = 'unknown'
            if ($proc) { $name = $proc.ProcessName }
            $path = ''
            try { if ($proc) { $path = $proc.Path } } catch { $path = '(path not readable)' }
            Write-Host ("listener   : {0}:{1} -> pid {2} ({3}) {4}" -f $o.LocalAddress, $o.LocalPort, $o.OwningProcess, $name, $path) -ForegroundColor DarkGray
        }
    } else {
        Write-Host "listener   : nothing is LISTENING on port $Port" -ForegroundColor Yellow
    }
} catch {
    Write-Host ("listener   : cannot query port owners here ({0}) - use: netstat -ano | findstr :{1}" -f $_.Exception.Message, $Port) -ForegroundColor DarkGray
}

# ---- connect / send / receive -------------------------------------------------------------
$client = New-Object System.Net.Sockets.TcpClient
$connect = $client.BeginConnect($Target, $Port, $null, $null)
if (-not $connect.AsyncWaitHandle.WaitOne($TimeoutMs)) {
    Write-Host "RESULT     : connect failed (timeout after ${TimeoutMs}ms) - supervisor says 'connect failed'" -ForegroundColor Red
    $client.Close()
    exit 2
}
try { $client.EndConnect($connect) } catch {
    Write-Host ("RESULT     : connect failed ({0}) - supervisor says 'connect failed'" -f $_.Exception.Message) -ForegroundColor Red
    $client.Close()
    exit 2
}

$stream = $client.GetStream()
$stream.Write($packet, 0, $packet.Length)
$stream.Flush()
Write-Host "sent       : $($packet.Length) bytes" -ForegroundColor DarkGray

$deadline = [datetime]::UtcNow.AddMilliseconds($TimeoutMs)
$buffer = New-Object byte[] 256
$read = 0
$timedOut = $false
$reset = $false
# A blocking read with ReadTimeout is the exact equivalent of the supervisor's select()+recv():
#    > 0  data arrived
#      0  the peer closed the connection gracefully (FIN) - no answer
#    throw timeout / connection reset - no answer either way
$stream.ReadTimeout = $TimeoutMs
try {
    $read = $stream.Read($buffer, 0, $buffer.Length)
} catch [System.IO.IOException] {
    if ($_.Exception.InnerException -and $_.Exception.InnerException -is [System.Net.Sockets.SocketException] -and
        $_.Exception.InnerException.SocketErrorCode -eq [System.Net.Sockets.SocketError]::TimedOut) {
        $timedOut = $true
    } elseif ($_.Exception.Message -match 'timed out|超时') {
        $timedOut = $true
    } else {
        $reset = $true      # connection reset: a close without data, same class as 0 bytes
    }
} catch {
    $reset = $true
}

if ($read -le 0) {
    if ($timedOut) {
        Write-Host "RESULT     : no answer within ${TimeoutMs}ms (socket still open) - supervisor says 'no response to AUTH_LOGON_CHALLENGE'" -ForegroundColor Red
        Write-Host "             => either the peer swallowed the packet, or the service behind the port hangs." -ForegroundColor Yellow
    } else {
        Write-Host ("RESULT     : peer closed without answering ({0}) - supervisor says 'short/empty auth response'" -f $(if ($reset) { 'connection reset' } else { '0 bytes' })) -ForegroundColor Red
        Write-Host "             => this is the production symptom: TCP accepts, nothing answers the AC handshake." -ForegroundColor Yellow
        Write-Host "             => check the 'listener' line above: is that PID really <authserver.exe> of THIS" -ForegroundColor Yellow
        Write-Host "                deployment, and is it an AzerothCore build whose sAuthLogonChallenge_C layout" -ForegroundColor Yellow
        Write-Host "                matches (32/64 bit and pack(1) aside, some forks add fields)?" -ForegroundColor Yellow
    }
    $client.Close()
    exit 3
}

$hex = (0..($read - 1) | ForEach-Object { '{0:X2}' -f $buffer[$_] }) -join ' '
Write-Host "received   : $read bytes: $hex" -ForegroundColor Green

if ($read -ge 2 -and $buffer[0] -eq 0x00) {
    # AzerothCore answers: [cmd=AUTH_LOGON_CHALLENGE][0x00][code] - the code is the THIRD byte
    # (the supervisor reads the second, which is always 0x00, so its detail text says
    #  "error code 0x00" for a healthy answer - only the detail text, the verdict is right).
    # A 2 byte answer is what this repository's fake server sends ([cmd][code]).
    $code = if ($read -ge 3) { $buffer[2] } else { $buffer[1] }
    $names = @{
        0x00 = 'SUCCESS - full challenge returned, this account EXISTS (fail: the probe made a real account name)'
        0x03 = 'WOW_FAIL_BANNED'
        0x04 = 'WOW_FAIL_UNKNOWN_ACCOUNT (expected for the probe)'
        0x08 = 'WOW_FAIL_DB_BUSY'
        0x09 = 'WOW_FAIL_VERSION_INVALID'
        0x0C = 'WOW_FAIL_SUSPENDED'
        0x0D = 'WOW_FAIL_BAD_LOGIN'
        0x0F = 'WOW_FAIL_LOCKED_ENFORCED'
        0x10 = 'WOW_FAIL_UNLOCKABLE_LOCK'
    }
    $what = if ($names.ContainsKey([int]$code)) { $names[[int]$code] } else { 'unknown code' }
    if ($read -ge 3) {
        Write-Host "RESULT     : AzerothCore answered AUTH_LOGON_CHALLENGE, code 0x$('{0:X2}' -f $code) = $what" -ForegroundColor Green
        Write-Host "             => this port really is an AzerothCore authserver; the supervisor reports PROBE OK" -ForegroundColor Green
    } else {
        Write-Host "RESULT     : short answer ($read bytes) - the supervisor requires at least 2 bytes and would say 'short/empty auth response'" -ForegroundColor Yellow
    }
} else {
    Write-Host "RESULT     : unexpected first byte 0x$('{0:X2}' -f $buffer[0]) - not an AzerothCore auth reply (wrong service on this port?)" -ForegroundColor Red
}

$client.Close()
