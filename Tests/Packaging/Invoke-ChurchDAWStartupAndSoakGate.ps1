<#
Windows-only runtime gate for two requirements: usable-main-window startup and
a four-hour MIDI soak. It deliberately does not claim preset-switch audio
continuity, because process responsiveness and clean logs cannot prove that.
#>
[CmdletBinding()]
param(
    [string] $CandidateExe,
    [string] $ExpectedSha256,
    [string] $OutputDirectory,
    [ValidateRange(1, 604800)]
    [int] $DurationSeconds = 14400,
    [ValidateRange(100, 120000)]
    [int] $StartupLimitMs = 2000,
    [ValidateRange(1, 600)]
    [int] $HeartbeatSeconds = 60,
    [string] $MidiDevicePattern = 'loopMIDI Port*',
    [string] $ChurchDawLog = "$env:APPDATA\ChurchDAW\ChurchDAW.log",
    [switch] $AllowShortDiagnostic,
    [switch] $SelfTest
)

$ErrorActionPreference = 'Stop'
$minimumQualifyingSeconds = 4 * 60 * 60
$minimumCoverageRatio = 0.95
$nominalMidiCycleSeconds = 1.5
$minimumHeartbeatCount = [Math]::Max(1, [int] [Math]::Floor(
    ($minimumQualifyingSeconds / [double] $HeartbeatSeconds) * $minimumCoverageRatio))
$minimumMidiCycles = [Math]::Max(1, [int] [Math]::Floor(
    ($minimumQualifyingSeconds / $nominalMidiCycleSeconds) * $minimumCoverageRatio))

function Test-StartupAndSoakResult
{
    param([hashtable] $Result)

    return $Result.StartupReady `
        -and $Result.StartupMs -le $Result.StartupLimitMs `
        -and $Result.RequestedDurationSeconds -ge $minimumQualifyingSeconds `
        -and $Result.ActualDurationSeconds -ge $minimumQualifyingSeconds `
        -and $Result.HeartbeatCount -ge $Result.MinimumHeartbeatCount `
        -and $Result.Cycles -ge $Result.MinimumMidiCycles `
        -and $Result.AllHeartbeatsResponding `
        -and -not $Result.ExitedEarly `
        -and $Result.AppLogRead `
        -and $Result.SuspiciousLogLines -eq 0 `
        -and $Result.StderrBytes -eq 0 `
        -and $Result.MidiStopComplete `
        -and $Result.NormalExit
}

if ($SelfTest)
{
    $passing = @{
        StartupReady = $true
        StartupMs = 1999
        StartupLimitMs = 2000
        RequestedDurationSeconds = 14400
        ActualDurationSeconds = 14400
        HeartbeatCount = $minimumHeartbeatCount + 12
        MinimumHeartbeatCount = $minimumHeartbeatCount
        Cycles = $minimumMidiCycles + 480
        MinimumMidiCycles = $minimumMidiCycles
        AllHeartbeatsResponding = $true
        ExitedEarly = $false
        AppLogRead = $true
        SuspiciousLogLines = 0
        StderrBytes = 0
        MidiStopComplete = $true
        NormalExit = $true
    }

    $cases = @(
        @{ Name = 'qualifying'; Expected = $true; Value = $passing.Clone() },
        @{ Name = 'startup-over-limit'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'short-duration'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'insufficient-heartbeats'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'insufficient-midi-cycles'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'unresponsive'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'missing-app-log'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'suspicious-log'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'stderr'; Expected = $false; Value = $passing.Clone() },
        @{ Name = 'abnormal-exit'; Expected = $false; Value = $passing.Clone() }
    )

    $cases[1].Value.StartupMs = 2001
    $cases[2].Value.ActualDurationSeconds = 14399
    $cases[3].Value.HeartbeatCount = $minimumHeartbeatCount - 1
    $cases[4].Value.Cycles = $minimumMidiCycles - 1
    $cases[5].Value.AllHeartbeatsResponding = $false
    $cases[6].Value.AppLogRead = $false
    $cases[7].Value.SuspiciousLogLines = 1
    $cases[8].Value.StderrBytes = 1
    $cases[9].Value.NormalExit = $false

    foreach ($case in $cases)
    {
        $actual = Test-StartupAndSoakResult $case.Value

        if ($actual -ne $case.Expected)
        {
            Write-Error "SELFTEST_FAIL case=$($case.Name) expected=$($case.Expected) actual=$actual"
            exit 90
        }
    }

    Write-Output "STARTUP_SOAK_GATE_SELFTEST_OK cases=$($cases.Count)"
    exit 0
}

if ([string]::IsNullOrWhiteSpace($CandidateExe) `
     -or [string]::IsNullOrWhiteSpace($ExpectedSha256))
{
    throw 'CandidateExe and ExpectedSha256 are required outside SelfTest.'
}

$candidate = [IO.Path]::GetFullPath($CandidateExe)

if (-not (Test-Path -LiteralPath $candidate -PathType Leaf))
{
    throw "Candidate not found: $candidate"
}

if ($ExpectedSha256 -notmatch '^[0-9A-Fa-f]{64}$')
{
    throw 'ExpectedSha256 must contain exactly 64 hexadecimal characters.'
}

if ($DurationSeconds -lt $minimumQualifyingSeconds -and -not $AllowShortDiagnostic)
{
    throw "A qualifying soak requires at least $minimumQualifyingSeconds seconds. Use -AllowShortDiagnostic only for harness diagnostics."
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory))
{
    $repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $OutputDirectory = Join-Path $repositoryRoot `
        ('dist\stability-gates\startup-soak-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

$evidenceDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $evidenceDirectory -Force | Out-Null

$actualSha256 = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash.ToUpperInvariant()

if ($actualSha256 -ne $ExpectedSha256.ToUpperInvariant())
{
    throw "Candidate hash mismatch. expected=$ExpectedSha256 actual=$actualSha256"
}

$preexisting = @(Get-CimInstance Win32_Process -Filter "Name='ChurchDAW.exe'" -ErrorAction Stop)

if ($preexisting.Count -ne 0)
{
    throw "Refusing to run while ChurchDAW is already open. pids=$($preexisting.ProcessId -join ',')"
}

$nativeSource = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class ChurchDawStabilityNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    public sealed class WindowInfo
    {
        public long Handle;
        public int Width;
        public int Height;
    }

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    public static WindowInfo[] GetVisibleWindows(int processId)
    {
        var result = new List<WindowInfo>();
        EnumWindows(delegate (IntPtr window, IntPtr ignored)
        {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            Rect rect;

            if (owner == (uint) processId && IsWindowVisible(window)
                && GetWindowRect(window, out rect)
                && rect.Right > rect.Left && rect.Bottom > rect.Top)
            {
                result.Add(new WindowInfo
                {
                    Handle = window.ToInt64(),
                    Width = rect.Right - rect.Left,
                    Height = rect.Bottom - rect.Top
                });
            }

            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    public struct MidiCaps
    {
        public ushort Mid, Pid;
        public uint DriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string Name;
        public ushort Technology, Voices, Notes, ChannelMask;
        public uint Support;
    }

    [DllImport("winmm.dll")]
    public static extern uint midiOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Auto)]
    public static extern uint midiOutGetDevCaps(UIntPtr id, out MidiCaps caps, uint size);
    [DllImport("winmm.dll")]
    public static extern uint midiOutOpen(out IntPtr handle, uint deviceId, IntPtr callback, IntPtr instance, uint flags);
    [DllImport("winmm.dll")]
    public static extern uint midiOutShortMsg(IntPtr handle, uint message);
    [DllImport("winmm.dll")]
    public static extern uint midiOutReset(IntPtr handle);
    [DllImport("winmm.dll")]
    public static extern uint midiOutClose(IntPtr handle);
}
'@

if ($null -eq ('ChurchDawStabilityNative' -as [type]))
{
    Add-Type -TypeDefinition $nativeSource
}

$stdoutPath = Join-Path $evidenceDirectory 'churchdaw.stdout.log'
$stderrPath = Join-Path $evidenceDirectory 'churchdaw.stderr.log'
$gateLogPath = Join-Path $evidenceDirectory 'startup-soak.log'
$evidencePath = Join-Path $evidenceDirectory 'startup-soak.json'

function Write-GateLine([string] $Message)
{
    $line = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff') + ' ' + $Message
    $line | Tee-Object -FilePath $gateLogPath -Append
}

function Send-Midi([IntPtr] $Handle, [int] $Status, [int] $Data1, [int] $Data2)
{
    $message = [uint32] ($Status -bor ($Data1 -shl 8) -bor ($Data2 -shl 16))
    $rc = [ChurchDawStabilityNative]::midiOutShortMsg($Handle, $message)

    if ($rc -ne 0)
    {
        throw "midiOutShortMsg failed rc=$rc"
    }
}

$result = @{
    StartupReady = $false
    StartupMs = [long]::MaxValue
    StartupLimitMs = $StartupLimitMs
    RequestedDurationSeconds = $DurationSeconds
    ActualDurationSeconds = 0.0
    HeartbeatCount = 0
    MinimumHeartbeatCount = $minimumHeartbeatCount
    Cycles = 0
    MinimumMidiCycles = $minimumMidiCycles
    AllHeartbeatsResponding = $true
    ExitedEarly = $false
    AppLogRead = $false
    SuspiciousLogLines = 0
    StderrBytes = 0
    MidiStopComplete = $false
    NormalExit = $false
}

$heartbeats = [Collections.Generic.List[object]]::new()
$application = $null
$midiHandle = [IntPtr]::Zero
$midiOpened = $false
$midiDeviceName = ''
$cycles = 0
$failure = ''
$logStartLength = if (Test-Path -LiteralPath $ChurchDawLog) { (Get-Item -LiteralPath $ChurchDawLog).Length } else { 0 }

try
{
    $startupWatch = [Diagnostics.Stopwatch]::StartNew()
    $application = Start-Process -FilePath $candidate `
        -WorkingDirectory (Split-Path -Parent $candidate) `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(120)

    while ([DateTime]::UtcNow -lt $readyDeadline)
    {
        $application.Refresh()

        if ($application.HasExited)
        {
            $result.ExitedEarly = $true
            throw "ChurchDAW exited before its main performance window was ready. exit=$($application.ExitCode)"
        }

        $windows = @([ChurchDawStabilityNative]::GetVisibleWindows($application.Id))
        $largeWindows = @($windows | Where-Object { $_.Width -ge 900 -and $_.Height -ge 480 })

        if ($application.Responding -and $largeWindows.Count -eq 1 -and $windows.Count -eq 1)
        {
            $startupWatch.Stop()
            $result.StartupReady = $true
            $result.StartupMs = $startupWatch.ElapsedMilliseconds
            Write-GateLine "STARTUP_READY elapsed_ms=$($result.StartupMs) window=$($largeWindows[0].Width)x$($largeWindows[0].Height) limit_ms=$StartupLimitMs"
            break
        }

        Start-Sleep -Milliseconds 20
    }

    if (-not $result.StartupReady)
    {
        throw 'Main performance window did not become uniquely visible and responsive within 120 seconds.'
    }

    if ($result.StartupMs -gt $StartupLimitMs)
    {
        throw "Startup exceeded the limit. elapsed_ms=$($result.StartupMs) limit_ms=$StartupLimitMs"
    }

    $midiCapsSize = [uint32] [Runtime.InteropServices.Marshal]::SizeOf([type] [ChurchDawStabilityNative+MidiCaps])
    $midiDeviceId = $null

    for ($index = 0; $index -lt [ChurchDawStabilityNative]::midiOutGetNumDevs(); ++$index)
    {
        $caps = New-Object ChurchDawStabilityNative+MidiCaps
        $rc = [ChurchDawStabilityNative]::midiOutGetDevCaps([UIntPtr]::new([uint64] $index), [ref] $caps, $midiCapsSize)

        if ($rc -eq 0 -and $caps.Name -like $MidiDevicePattern)
        {
            $midiDeviceId = [uint32] $index
            $midiDeviceName = $caps.Name
            break
        }
    }

    if ($null -eq $midiDeviceId)
    {
        throw "No MIDI output matched '$MidiDevicePattern'."
    }

    $openRc = [ChurchDawStabilityNative]::midiOutOpen([ref] $midiHandle, $midiDeviceId, [IntPtr]::Zero, [IntPtr]::Zero, 0)

    if ($openRc -ne 0)
    {
        throw "midiOutOpen failed rc=$openRc"
    }

    $midiOpened = $true
    $soakStarted = [DateTime]::UtcNow
    $nextHeartbeat = $soakStarted
    Write-GateLine "SOAK_START duration_s=$DurationSeconds qualifying=$($DurationSeconds -ge $minimumQualifyingSeconds) midi='$midiDeviceName'"

    while (([DateTime]::UtcNow - $soakStarted).TotalSeconds -lt $DurationSeconds)
    {
        $application.Refresh()

        if ($application.HasExited)
        {
            $result.ExitedEarly = $true
            throw "ChurchDAW exited during soak. cycle=$cycles exit=$($application.ExitCode)"
        }

        foreach ($note in 60, 64, 67) { Send-Midi $midiHandle 0x90 $note 104 }
        Start-Sleep -Milliseconds 650
        foreach ($note in 60, 64, 67) { Send-Midi $midiHandle 0x80 $note 0 }
        Start-Sleep -Milliseconds 850
        ++$cycles
        $result.Cycles = $cycles

        if ([DateTime]::UtcNow -ge $nextHeartbeat)
        {
            $application.Refresh()
            $elapsed = ([DateTime]::UtcNow - $soakStarted).TotalSeconds
            $heartbeat = [ordered]@{
                elapsedSeconds = [Math]::Round($elapsed, 3)
                responding = [bool] $application.Responding
                workingSetBytes = [long] $application.WorkingSet64
                privateBytes = [long] $application.PrivateMemorySize64
                cpuSeconds = [Math]::Round($application.CPU, 3)
            }
            $heartbeats.Add([pscustomobject] $heartbeat)
            $result.HeartbeatCount = $heartbeats.Count
            $result.AllHeartbeatsResponding = $result.AllHeartbeatsResponding -and $heartbeat.responding
            Write-GateLine "HEARTBEAT elapsed_s=$($heartbeat.elapsedSeconds) responding=$($heartbeat.responding) working_set_bytes=$($heartbeat.workingSetBytes) private_bytes=$($heartbeat.privateBytes) cpu_s=$($heartbeat.cpuSeconds)"

            if (-not $heartbeat.responding)
            {
                throw 'ChurchDAW stopped responding during soak.'
            }

            $nextHeartbeat = [DateTime]::UtcNow.AddSeconds($HeartbeatSeconds)
        }
    }

    $result.ActualDurationSeconds = ([DateTime]::UtcNow - $soakStarted).TotalSeconds
}
catch
{
    $failure = $_.Exception.Message
    Write-GateLine "GATE_FAILURE message='$failure'"
}
finally
{
    if ($midiOpened)
    {
        try
        {
            foreach ($channel in 0..15)
            {
                Send-Midi $midiHandle (0xB0 + $channel) 120 0
                Send-Midi $midiHandle (0xB0 + $channel) 121 0
                Send-Midi $midiHandle (0xB0 + $channel) 123 0
            }

            [ChurchDawStabilityNative]::midiOutReset($midiHandle) | Out-Null
            $result.MidiStopComplete = $true
            Write-GateLine 'MIDI_STOP all_sound_off=16 reset_controllers=16 all_notes_off=16'
        }
        catch
        {
            if ([string]::IsNullOrEmpty($failure)) { $failure = $_.Exception.Message }
            Write-GateLine "MIDI_STOP_FAILURE message='$($_.Exception.Message)'"
        }
        finally
        {
            [ChurchDawStabilityNative]::midiOutClose($midiHandle) | Out-Null
        }
    }

    try
    {
        if (Test-Path -LiteralPath $ChurchDawLog)
        {
            $stream = [IO.File]::Open($ChurchDawLog, 'Open', 'Read', 'ReadWrite')

            try
            {
                $seekOffset = if ($stream.Length -ge $logStartLength) { $logStartLength } else { 0 }
                $stream.Seek($seekOffset, [IO.SeekOrigin]::Begin) | Out-Null
                $reader = [IO.StreamReader]::new($stream)
                $newLog = $reader.ReadToEnd()
                $suspects = @($newLog -split "`r?`n" | Where-Object {
                    $_ -match '(?i)crash|dropout|xrun|underflow|audio.{0,24}lost|not responding|exception|fatal'
                })
                $result.AppLogRead = $true
                $result.SuspiciousLogLines = $suspects.Count
                Write-GateLine "APP_LOG new_bytes=$([Text.Encoding]::UTF8.GetByteCount($newLog)) suspicious_lines=$($suspects.Count)"
                $suspects | Set-Content -LiteralPath (Join-Path $evidenceDirectory 'suspicious-lines.log') -Encoding UTF8
            }
            finally
            {
                $stream.Dispose()
            }
        }
    }
    catch
    {
        if ([string]::IsNullOrEmpty($failure)) { $failure = $_.Exception.Message }
        Write-GateLine "APP_LOG_FAILURE message='$($_.Exception.Message)'"
    }

    if ($null -ne $application)
    {
        $application.Refresh()

        if (-not $application.HasExited)
        {
            $application.CloseMainWindow() | Out-Null

            if (-not $application.WaitForExit(15000))
            {
                Stop-Process -Id $application.Id -Force
                $application.WaitForExit()
                if ([string]::IsNullOrEmpty($failure)) { $failure = 'ChurchDAW did not close normally after the gate.' }
            }
        }

        $application.Refresh()
        $result.NormalExit = $application.HasExited -and $application.ExitCode -eq 0
    }

    if (Test-Path -LiteralPath $stderrPath)
    {
        $result.StderrBytes = (Get-Item -LiteralPath $stderrPath).Length
    }
}

$qualified = Test-StartupAndSoakResult $result
$evidence = [ordered]@{
    schema = 'churchdaw-startup-soak-v1'
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    candidate = [ordered]@{
        path = $candidate
        sha256 = $actualSha256
        bytes = (Get-Item -LiteralPath $candidate).Length
    }
    harness = [ordered]@{
        path = $PSCommandPath
        sha256 = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    }
    startup = [ordered]@{
        definition = 'Only one visible responsive top-level process window, at least 900x480; startup/login/splash windows do not qualify.'
        ready = $result.StartupReady
        elapsedMs = $result.StartupMs
        limitMs = $StartupLimitMs
        pass = $result.StartupReady -and $result.StartupMs -le $StartupLimitMs
    }
    soak = [ordered]@{
        minimumQualifyingSeconds = $minimumQualifyingSeconds
        requestedSeconds = $DurationSeconds
        actualSeconds = [Math]::Round($result.ActualDurationSeconds, 3)
        diagnosticOnly = $DurationSeconds -lt $minimumQualifyingSeconds
        midiDevice = $midiDeviceName
        cycles = $cycles
        minimumMidiCycles = $result.MinimumMidiCycles
        heartbeats = $heartbeats
        minimumHeartbeatCount = $result.MinimumHeartbeatCount
        allHeartbeatsResponding = $result.AllHeartbeatsResponding
        appLogRead = $result.AppLogRead
        suspiciousLogLines = $result.SuspiciousLogLines
        stderrBytes = $result.StderrBytes
        midiStopComplete = $result.MidiStopComplete
        normalExit = $result.NormalExit
        pass = $qualified
        audioOutputCaptured = $false
    }
    presetContinuity = [ordered]@{
        status = 'not-measured'
        pass = $false
        reason = 'This harness does not capture the audio stream and cannot prove zero dropout during a preset/timbre switch.'
    }
    failure = $failure
    verdict = if ($qualified) { 'STARTUP_AND_4H_SOAK_PASS' } elseif ($DurationSeconds -lt $minimumQualifyingSeconds) { 'DIAGNOSTIC_ONLY' } else { 'FAIL' }
}

$evidence | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $evidencePath -Encoding UTF8
Write-GateLine "EVIDENCE path='$evidencePath' verdict=$($evidence.verdict)"

if ($qualified) { exit 0 }
exit 1
