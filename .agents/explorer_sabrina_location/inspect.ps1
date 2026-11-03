$path = "C:\Users\Fabiopc\Downloads\Sabrina(64)\Sabrina.dll"
Write-Host "=== FILE INFO ==="
$fileInfo = Get-Item $path
Write-Host "Path: $($fileInfo.FullName)"
Write-Host "Size: $($fileInfo.Length) bytes ($([math]::Round($fileInfo.Length / 1MB, 2)) MB)"
Write-Host "LastWriteTime: $($fileInfo.LastWriteTime)"

Write-Host "`n=== VERSION INFO ==="
$vi = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
Write-Host "CompanyName: $($vi.CompanyName)"
Write-Host "FileDescription: $($vi.FileDescription)"
Write-Host "FileVersion: $($vi.FileVersion)"
Write-Host "InternalName: $($vi.InternalName)"
Write-Host "OriginalFilename: $($vi.OriginalFilename)"
Write-Host "ProductName: $($vi.ProductName)"
Write-Host "ProductVersion: $($vi.ProductVersion)"
Write-Host "Comments: $($vi.Comments)"

Write-Host "`n=== PE HEADER & ARCHITECTURE ==="
$bytes = [System.IO.File]::ReadAllBytes($path)
$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
$machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
$timestamp = [BitConverter]::ToUInt32($bytes, $peOffset + 8)
$dt = (Get-Date "1970-01-01 00:00:00Z").AddSeconds($timestamp)

Write-Host ("PE Header Offset: 0x{0:X}" -f $peOffset)
Write-Host ("Machine Code: 0x{0:X4}" -f $machine)
if ($machine -eq 0x8664) { Write-Host "Architecture: x86_64 (64-bit AMD64)" }
elseif ($machine -eq 0x014c) { Write-Host "Architecture: x86 (32-bit x86)" }
elseif ($machine -eq 0xAA64) { Write-Host "Architecture: ARM64" }
else { Write-Host "Architecture: Unknown ($machine)" }
Write-Host "TimeDateStamp: $dt UTC"

Write-Host "`n=== EXPORT TABLE ANALYSIS ==="
# Parse PE export directory
$optionalHeaderOffset = $peOffset + 24
$magic = [BitConverter]::ToUInt16($bytes, $optionalHeaderOffset)
Write-Host ("Optional Header Magic: 0x{0:X4} ({1})" -f $magic, $(if($magic -eq 0x20b){"PE32+"}else{"PE32"}))

$exportRva = 0
$exportSize = 0
if ($magic -eq 0x20b) { # PE32+
    $exportRva = [BitConverter]::ToUInt32($bytes, $optionalHeaderOffset + 112)
    $exportSize = [BitConverter]::ToUInt32($bytes, $optionalHeaderOffset + 116)
} else { # PE32
    $exportRva = [BitConverter]::ToUInt32($bytes, $optionalHeaderOffset + 96)
    $exportSize = [BitConverter]::ToUInt32($bytes, $optionalHeaderOffset + 100)
}
Write-Host ("Export RVA: 0x{0:X}, Size: {1}" -f $exportRva, $exportSize)

# Convert RVA to File Offset via Section Headers
$numSections = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
$sizeOfOptionalHeader = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
$sectionHeaderOffset = $optionalHeaderOffset + $sizeOfOptionalHeader

function RvaToOffset([uint32]$rva, [byte[]]$b, [int]$secOffset, [int]$secCount) {
    for ($i = 0; $i -lt $secCount; $i++) {
        $cur = $secOffset + ($i * 40)
        $vSize = [BitConverter]::ToUInt32($b, $cur + 8)
        $vAddr = [BitConverter]::ToUInt32($b, $cur + 12)
        $rSize = [BitConverter]::ToUInt32($b, $cur + 16)
        $rAddr = [BitConverter]::ToUInt32($b, $cur + 20)
        if ($rva -ge $vAddr -and $rva -lt ($vAddr + $vSize)) {
            return $rAddr + ($rva - $vAddr)
        }
    }
    return 0
}

if ($exportRva -gt 0) {
    $exportOffset = RvaToOffset $exportRva $bytes $sectionHeaderOffset $numSections
    Write-Host ("Export File Offset: 0x{0:X}" -f $exportOffset)
    if ($exportOffset -gt 0) {
        $numNames = [BitConverter]::ToUInt32($bytes, $exportOffset + 24)
        $namesRva = [BitConverter]::ToUInt32($bytes, $exportOffset + 32)
        $namesOffset = RvaToOffset $namesRva $bytes $sectionHeaderOffset $numSections
        Write-Host "Exported Functions Count: $numNames"
        for ($n = 0; $n -lt [Math]::Min($numNames, 50); $n++) {
            $nameRva = [BitConverter]::ToUInt32($bytes, $namesOffset + ($n * 4))
            $nameOff = RvaToOffset $nameRva $bytes $sectionHeaderOffset $numSections
            $sb = [System.Text.StringBuilder]::new()
            $idx = $nameOff
            while ($bytes[$idx] -ne 0 -and $idx -lt $bytes.Length) {
                [void]$sb.Append([char]$bytes[$idx])
                $idx++
            }
            Write-Host "  Export [$n]: $($sb.ToString())"
        }
    }
} else {
    Write-Host "No Export Table found in PE Header."
}

Write-Host "`n=== ASCII / UNICODE STRING SEARCH ==="
$content = [System.IO.File]::ReadAllBytes($path)
$strAscii = [System.Text.Encoding]::ASCII.GetString($content)

$keywords = @(
    "VSTPluginMain", "main", "GetPluginFactory", "JUCE", "Juce", "AudioProcessor", 
    "Steinberg", "Sabrina", "VST", "VST3", "CLAP", "AAX", "Synth", "Effect",
    "Filter", "Reverb", "Delay", "Compressor", "Gain", "Cutoff", "Resonance",
    "Volume", "Frequency", "Mix", "Bypass"
)

foreach ($kw in $keywords) {
    $count = ([regex]::Matches($strAscii, [regex]::Escape($kw))).Count
    Write-Host ("Keyword '$kw': $count occurrence(s)")
}
