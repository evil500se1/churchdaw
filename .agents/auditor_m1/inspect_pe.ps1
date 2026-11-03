$path1 = "C:\Users\Fabiopc\Downloads\Sabrina(64)\Sabrina.dll"
$path2 = "D:\ChurchDAW_Source\Plugins\Sabrina\bin\win_x64\Sabrina.dll"

$bytes1 = [System.IO.File]::ReadAllBytes($path1)
$bytes2 = [System.IO.File]::ReadAllBytes($path2)

Write-Host "File 1 Size: $($bytes1.Length) bytes"
Write-Host "File 2 Size: $($bytes2.Length) bytes"

$hash1 = (Get-FileHash -Path $path1 -Algorithm SHA256).Hash
$hash2 = (Get-FileHash -Path $path2 -Algorithm SHA256).Hash

Write-Host "File 1 SHA256: $hash1"
Write-Host "File 2 SHA256: $hash2"

# Check DOS header magic 'MZ' (0x4D, 0x5A)
$isMZ1 = ($bytes1[0] -eq 0x4D -and $bytes1[1] -eq 0x5A)
$isMZ2 = ($bytes2[0] -eq 0x4D -and $bytes2[1] -eq 0x5A)

Write-Host "File 1 MZ Header Valid: $isMZ1"
Write-Host "File 2 MZ Header Valid: $isMZ2"

# Read e_lfanew offset to PE Header (at offset 0x3C)
$peOffset1 = [System.BitConverter]::ToInt32($bytes1, 0x3C)
$peOffset2 = [System.BitConverter]::ToInt32($bytes2, 0x3C)

Write-Host "PE Header Offset: $peOffset1"

# PE signature 'PE\0\0' (0x50, 0x45, 0x00, 0x00)
$isPE1 = ($bytes1[$peOffset1] -eq 0x50 -and $bytes1[$peOffset1+1] -eq 0x45 -and $bytes1[$peOffset1+2] -eq 0 -and $bytes1[$peOffset1+3] -eq 0)
Write-Host "PE Signature Valid: $isPE1"

# Machine type (offset peOffset + 4)
# 0x8664 = x64 (AMD64)
$machine = [System.BitConverter]::ToUInt16($bytes1, $peOffset1 + 4)
Write-Host ("Machine Type: 0x{0:X4} (0x8664 = x64)" -f $machine)

# Characteristics (offset peOffset + 22)
# Bit 0x2000 = DLL
$char = [System.BitConverter]::ToUInt16($bytes1, $peOffset1 + 22)
$isDLL = (($char -band 0x2000) -ne 0)
Write-Host ("Characteristics: 0x{0:X4} (Is DLL: {1})" -f $char, $isDLL)

# Search for exported functions or VST strings in the binary bytes
$stringContent = [System.Text.Encoding]::ASCII.GetString($bytes1)
$vstExports = @("VSTPluginMain", "main", "main_macho", "PluginEntry")
foreach ($exp in $vstExports) {
    if ($stringContent.Contains($exp)) {
        Write-Host "Found VST symbol in binary: $exp"
    }
}

if ($stringContent.Contains("VST") -or $stringContent.Contains("vst") -or $stringContent.Contains("Steinberg")) {
    Write-Host "Found VST / Steinberg signature in binary."
}
