# Sobe TODOS os timbres ainda nao ready, um a um, com partes de 500 MB.
# Uso: powershell -ExecutionPolicy Bypass -File .\UploadAllTimbres.ps1

$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot

$catalogUrl = 'https://churchdaw.com/app/android/timbres/timbre-catalog-v1.json'
curl.exe -s $catalogUrl -o "$env:TEMP\timbre-cat.json"
$j = Get-Content "$env:TEMP\timbre-cat.json" -Raw | ConvertFrom-Json
$pending = @($j.instruments | Where-Object { -not $_.ready } | ForEach-Object { $_.raw_label })
Write-Host ("Pendentes: {0}" -f $pending.Count)

$i = 0
foreach ($name in $pending) {
    $i++
    Write-Host ""
    Write-Host ("==== [{0}/{1}] {2} ====" -f $i, $pending.Count, $name)
    $escaped = $name.Replace("'", "''")
    & powershell -NoProfile -ExecutionPolicy Bypass -Command `
        "& { .\BuildTimbreCatalog.ps1 -Upload -MaxChunkMb 500 -Only @('$escaped') }"
    $code = $LASTEXITCODE
    Write-Host ("exit=$code")
    Start-Sleep -Seconds 10
}

curl.exe -s $catalogUrl -o "$env:TEMP\timbre-cat-final.json"
$f = Get-Content "$env:TEMP\timbre-cat-final.json" -Raw | ConvertFrom-Json
$ready = @($f.instruments | Where-Object { $_.ready })
Write-Host ""
Write-Host ("PRONTO: {0} / {1}" -f $ready.Count, $f.instruments.Count)
$ready | ForEach-Object { Write-Host ("  {0}  {1:N1} MB" -f $_.name, ($_.download_bytes / 1MB)) }
if ($ready.Count -lt $f.instruments.Count) {
    Write-Host "AINDA PENDENTES:"
    $f.instruments | Where-Object { -not $_.ready } | ForEach-Object { Write-Host ("  {0}" -f $_.raw_label) }
}
