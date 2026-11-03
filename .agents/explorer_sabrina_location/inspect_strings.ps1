$path = "C:\Users\Fabiopc\Downloads\Sabrina(64)\Sabrina.dll"
$bytes = [System.IO.File]::ReadAllBytes($path)

# Extract printable ASCII strings of length >= 4
$strings = [System.Text.RegularExpressions.Regex]::Matches([System.Text.Encoding]::ASCII.GetString($bytes), "[\x20-\x7E]{4,}")

Write-Host "Total ASCII printable strings found: $($strings.Count)"

Write-Host "`n=== TOP INTERESTING STRINGS (Filter, Effect, Author, Params) ==="
$interesting = $strings | Where-Object { 
    $_.Value -match "sabrina" -or 
    $_.Value -match "vst" -or 
    $_.Value -match "plugin" -or 
    $_.Value -match "vendor" -or 
    $_.Value -match "author" -or 
    $_.Value -match "copyright" -or 
    $_.Value -match "audio" -or
    $_.Value -match "effect" -or
    $_.Value -match "preset" -or
    $_.Value -match "chunk" -or
    $_.Value -match "param" -or
    $_.Value -match "sdk" -or
    $_.Value -match "msvcp" -or
    $_.Value -match "vcruntime"
} | Select-Object -ExpandProperty Value -Unique

foreach ($s in $interesting) {
    Write-Host "  $s"
}

Write-Host "`n=== SAMPLE OF LONGER STRINGS (Possible parameters, UI labels, configs) ==="
$longer = $strings | Where-Object { $_.Value.Length -ge 8 -and $_.Value.Length -le 40 } | Select-Object -ExpandProperty Value -Unique | Select-Object -First 60
foreach ($s in $longer) {
    Write-Host "  $s"
}
