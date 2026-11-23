<#
Sends a new build to a Pico game that is running behind a boot loader (the
PicoCalc's menu), over its USB serial: the game takes it, checks it, writes
it over itself and restarts into it. Nothing to press on the board.

    pwsh tools/pico-usb-update.ps1 -File generated/build/picocalc_pico/DekiGame.bin -Port COM8

The game must be one built with this package's USB updater (pico/PicoUsbUpdate)
and must be running: the first build with it goes on the usual way.
#>
param(
    [Parameter(Mandatory = $true)][string]$File,
    [string]$Port = 'COM8'
)

# The checksum in C#: a byte at a time in PowerShell would take minutes.
Add-Type -TypeDefinition @'
public static class PicoUsbUpdateCrc
{
    public static uint Crc32(byte[] data)
    {
        uint crc = 0xFFFFFFFFu;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        return ~crc;
    }
}
'@

$command = 'DEKI-UPDATE '

# The next reply that starts with $Word, the game's log skipped: the reply
# without the command, 'ERR ...' when the game refused, $null on a timeout.
function Wait-Reply([System.IO.Ports.SerialPort]$Serial, [string]$Word, [int]$Milliseconds) {
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    while ($clock.ElapsedMilliseconds -lt $Milliseconds) {
        try { $line = $Serial.ReadLine().Trim() } catch [System.TimeoutException] { continue }
        if (-not $line.StartsWith($command)) { continue }
        $reply = $line.Substring($command.Length)
        if ($reply.StartsWith('ERR') -or $reply.StartsWith($Word)) { return $reply }
        # Shown, not returned: the caller gets the reply alone.
        Write-Host "  $reply"
    }
    return $null
}

$image = [System.IO.File]::ReadAllBytes((Resolve-Path $File))
$crc = [PicoUsbUpdateCrc]::Crc32($image)
$serial = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$serial.DtrEnable = $true
$serial.RtsEnable = $true
$serial.ReadTimeout = 500
$serial.WriteTimeout = 10000
$serial.NewLine = "`n"
$serial.Open()
try {
    $serial.DiscardInBuffer()
    Write-Output ("Sending {0} bytes (crc {1:x8}) to {2}" -f $image.Length, $crc, $Port)
    $serial.Write("`n$command$($image.Length) $('{0:x8}' -f $crc)`n")

    $ready = Wait-Reply $serial 'READY' 30000
    if (-not $ready -or $ready.StartsWith('ERR')) {
        if ($ready) { Write-Output $ready } else { Write-Output 'No answer: is the game running, built with the USB updater?' }
        exit 1
    }
    $chunk = [int]$ready.Substring('READY '.Length)
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $sent = 0
    $shown = 0
    while ($sent -lt $image.Length) {
        $count = [Math]::Min($chunk, $image.Length - $sent)
        $serial.Write($image, $sent, $count)
        $sent += $count
        $ok = Wait-Reply $serial 'OK' 15000
        if (-not $ok -or $ok.StartsWith('ERR')) {
            if ($ok) { Write-Output $ok } else { Write-Output "No answer after $sent bytes." }
            exit 1
        }
        $tenth = [int][Math]::Floor($sent * 10 / $image.Length)
        if ($tenth -gt $shown) {
            $shown = $tenth
            Write-Output ("  {0}% ({1:F1} s)" -f ($shown * 10), $clock.Elapsed.TotalSeconds)
        }
    }
    $done = Wait-Reply $serial 'INSTALLING' 30000
    if (-not $done -or $done.StartsWith('ERR')) {
        if ($done) { Write-Output $done } else { Write-Output 'No answer after the last chunk.' }
        exit 1
    }
    Write-Output ("{0} after {1:F1} s: the board restarts into the new build." -f $done, $clock.Elapsed.TotalSeconds)
}
finally {
    $serial.Close()
}
