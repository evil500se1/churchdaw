param(
    [string] $Manifest = "$PSScriptRoot\AndroidHosting\app\android\manifest.json",
    [string] $PackDirectory = "$PSScriptRoot\AndroidPacks",
    [string] $Bundle = "$PSScriptRoot\..\Releases\Android\ChurchDAW-1.3.0-signed.aab",
    [switch] $SkipRemote
)

$ErrorActionPreference = 'Stop'

function Assert-ReleaseCondition {
    param([bool] $Condition, [string] $Message)

    if (-not $Condition) {
        throw $Message
    }
}

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configurationVerifier = Join-Path $PSScriptRoot 'AssertAndroidBuildConfiguration.ps1'
Assert-ReleaseCondition (
    Test-Path -LiteralPath $configurationVerifier
) "Verificador Android ausente: $configurationVerifier"
& $configurationVerifier -ProjectRoot $projectRoot -Bundle $Bundle -CatalogManifest $Manifest

$catalog = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
Assert-ReleaseCondition ($catalog.schema_version -eq 1) 'Schema do manifesto inesperado'
Assert-ReleaseCondition ($catalog.package_id -eq 'com.churchdaw.app') 'Package ID inesperado'
Assert-ReleaseCondition ($catalog.minimum_version_code -le $catalog.latest_version_code) 'Faixa de versoes invalida'

$churchLive = @($catalog.libraries | Where-Object { $_.id -eq 'church-live' })
Assert-ReleaseCondition ($churchLive.Count -eq 1) 'Church Live ausente ou duplicado'
$churchLive = $churchLive[0]
$parts = @($churchLive.parts)
Assert-ReleaseCondition ($parts.Count -gt 0 -and $parts.Count -le 128) 'Quantidade de partes invalida'

[long] $downloadTotal = 0
foreach ($part in $parts) {
    Assert-ReleaseCondition ($part.download_url -match '^https://') "URL nao HTTPS: $($part.download_url)"
    Assert-ReleaseCondition ($part.sha256 -match '^[0-9a-f]{64}$') "SHA-256 invalido: $($part.download_url)"
    Assert-ReleaseCondition ([long] $part.download_bytes -gt 0) "Tamanho invalido: $($part.download_url)"

    $fileName = [Uri] $part.download_url
    $localFile = Join-Path $PackDirectory ([IO.Path]::GetFileName($fileName.LocalPath))
    Assert-ReleaseCondition (Test-Path -LiteralPath $localFile) "Pack local ausente: $localFile"

    $item = Get-Item -LiteralPath $localFile
    Assert-ReleaseCondition ($item.Length -eq [long] $part.download_bytes) "Tamanho local divergente: $($item.Name)"

    $hash = (Get-FileHash -LiteralPath $localFile -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-ReleaseCondition ($hash -eq $part.sha256) "Hash local divergente: $($item.Name)"

    if (-not $SkipRemote) {
        $headers = (& curl.exe -sS -I -A 'ChurchDAW/ReleaseVerifier' --max-time 30 $part.download_url) -join "`n"
        Assert-ReleaseCondition ($headers -match 'HTTP/\S+ 200') "HEAD remoto falhou: $($item.Name)"
        Assert-ReleaseCondition ($headers -match "(?im)^Content-Length:\s*$($item.Length)\s*$") "Tamanho remoto divergente: $($item.Name)"
        Assert-ReleaseCondition ($headers -match '(?im)^Accept-Ranges:\s*bytes\s*$') "Servidor sem retomada: $($item.Name)"

        $range = (& curl.exe -sS -r 0-0 -D - -o NUL -A 'ChurchDAW/ReleaseVerifier' --max-time 30 $part.download_url) -join "`n"
        Assert-ReleaseCondition ($range -match 'HTTP/\S+ 206') "Range remoto falhou: $($item.Name)"
        Assert-ReleaseCondition ($range -match "(?im)^content-range:\s*bytes 0-0/$($item.Length)\s*$") "Content-Range divergente: $($item.Name)"
    }

    $downloadTotal += $item.Length
    Write-Host "OK  $($item.Name)"
}

Assert-ReleaseCondition ($downloadTotal -eq [long] $churchLive.download_bytes) 'Soma dos downloads diverge do manifesto'
Assert-ReleaseCondition ([long] $churchLive.installed_bytes -gt 0) 'Tamanho instalado ausente'
Assert-ReleaseCondition (Test-Path -LiteralPath $Bundle) "AAB ausente: $Bundle"

$bundleItem = Get-Item -LiteralPath $Bundle
$bundleHash = (Get-FileHash -LiteralPath $Bundle -Algorithm SHA256).Hash

Write-Host ''
Write-Host "Partes: $($parts.Count)"
Write-Host "Download total: $downloadTotal bytes"
Write-Host "Instalado: $($churchLive.installed_bytes) bytes"
Write-Host "AAB: $($bundleItem.FullName)"
Write-Host "AAB bytes: $($bundleItem.Length)"
Write-Host "AAB SHA-256: $bundleHash"
Write-Host 'VERIFICACAO_ANDROID_OK'
