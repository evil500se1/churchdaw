[CmdletBinding()]
param(
    [string] $PlayStoreDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

if ([string]::IsNullOrWhiteSpace($PlayStoreDirectory)) {
    $PlayStoreDirectory = Join-Path $PSScriptRoot 'PlayStore'
}

function Assert-Condition {
    param(
        [bool] $Condition,
        [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-PngInfo {
    param([string] $Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $bytes = [System.IO.File]::ReadAllBytes($resolved)
    Assert-Condition ($bytes.Length -ge 33) "PNG truncado: $resolved"

    $signature = @(137, 80, 78, 71, 13, 10, 26, 10)
    for ($index = 0; $index -lt $signature.Count; ++$index) {
        Assert-Condition ($bytes[$index] -eq $signature[$index]) `
            "Assinatura PNG invalida: $resolved"
    }

    Assert-Condition (
        [System.Text.Encoding]::ASCII.GetString($bytes, 12, 4) -eq 'IHDR'
    ) "IHDR PNG ausente: $resolved"

    $width = [System.Net.IPAddress]::NetworkToHostOrder(
        [BitConverter]::ToInt32($bytes, 16)
    )
    $height = [System.Net.IPAddress]::NetworkToHostOrder(
        [BitConverter]::ToInt32($bytes, 20)
    )
    $bitDepth = [int] $bytes[24]
    $colorType = [int] $bytes[25]
    $chunks = [System.Text.Encoding]::ASCII.GetString($bytes)

    Assert-Condition ($chunks.Contains('IEND')) "IEND PNG ausente: $resolved"

    [pscustomobject]@{
        Path = $resolved
        Name = [System.IO.Path]::GetFileName($resolved)
        Width = $width
        Height = $height
        BitDepth = $bitDepth
        ColorType = $colorType
        HasTransparencyChunk = $chunks.Contains('tRNS')
        Bytes = [int64] $bytes.Length
        Sha256 = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash
    }
}

function Assert-PngSpec {
    param(
        [string] $Path,
        [int] $Width,
        [int] $Height,
        [int] $ColorType,
        [int64] $MaximumBytes
    )

    $info = Get-PngInfo $Path
    Assert-Condition ($info.Width -eq $Width) `
        "$($info.Name): largura $($info.Width), esperada $Width"
    Assert-Condition ($info.Height -eq $Height) `
        "$($info.Name): altura $($info.Height), esperada $Height"
    Assert-Condition ($info.BitDepth -eq 8) `
        "$($info.Name): PNG precisa usar 8 bits por canal"
    Assert-Condition ($info.ColorType -eq $ColorType) `
        "$($info.Name): color type $($info.ColorType), esperado $ColorType"
    Assert-Condition (-not $info.HasTransparencyChunk) `
        "$($info.Name): chunk tRNS inesperado"
    Assert-Condition ($info.Bytes -le $MaximumBytes) `
        "$($info.Name): arquivo excede $MaximumBytes bytes"
    return $info
}

$playStoreRoot = (Resolve-Path -LiteralPath $PlayStoreDirectory).Path
$validated = @()

# Google Play exige icone 512 x 512 em PNG 32-bit com alpha, ate 1024 KiB.
$validated += Assert-PngSpec `
    (Join-Path $playStoreRoot 'app-icon-512.png') `
    512 512 6 (1024 * 1024)

# Feature graphic: PNG 24-bit sem alpha, exatamente 1024 x 500.
$validated += Assert-PngSpec `
    (Join-Path $playStoreRoot 'feature-graphic-1024x500.png') `
    1024 500 2 (8 * 1024 * 1024)

$screenshots = @(
    'screenshot-01-performance-1920x1080.png',
    'screenshot-02-libraries-1920x1080.png',
    'screenshot-03-master-1920x1080.png',
    'screenshot-04-channel-focus-1920x1080.png'
)

foreach ($screenshot in $screenshots) {
    $info = Assert-PngSpec `
        (Join-Path $playStoreRoot $screenshot) `
        1920 1080 2 (8 * 1024 * 1024)
    Assert-Condition (
        ([math]::Max($info.Width, $info.Height)) -le
            (2 * [math]::Min($info.Width, $info.Height))
    ) "${screenshot}: a maior dimensao excede duas vezes a menor"
    $validated += $info
}

$listingPath = Join-Path $playStoreRoot 'listing-pt-BR.md'
$listing = [System.IO.File]::ReadAllText($listingPath)

$nameMatch = [regex]::Match(
    $listing,
    '(?ms)^## Nome do app\s*\r?\n\s*([^\r\n]+)'
)
$shortMatch = [regex]::Match(
    $listing,
    '(?ms)^## Descri\S+ curta\s*\r?\n\s*([^\r\n]+)'
)
$fullMatch = [regex]::Match(
    $listing,
    '(?ms)^## Descri\S+ completa\s*\r?\n(.*?)^## Textos alternativos'
)

Assert-Condition $nameMatch.Success 'Nome do app nao localizado na ficha'
Assert-Condition $shortMatch.Success 'Descricao curta nao localizada na ficha'
Assert-Condition $fullMatch.Success 'Descricao completa nao localizada na ficha'

$appName = $nameMatch.Groups[1].Value.Trim()
$shortDescription = $shortMatch.Groups[1].Value.Trim()
$fullDescription = $fullMatch.Groups[1].Value.Trim()

Assert-Condition ($appName.Length -le 30) `
    "Nome do app excede 30 caracteres: $($appName.Length)"
Assert-Condition ($shortDescription.Length -le 80) `
    "Descricao curta excede 80 caracteres: $($shortDescription.Length)"
Assert-Condition ($fullDescription.Length -le 4000) `
    "Descricao completa excede 4000 caracteres: $($fullDescription.Length)"

$altTexts = [regex]::Matches($listing, '(?m)^>\s*(.+)$')
Assert-Condition ($altTexts.Count -eq 5) `
    "Esperados 5 textos alternativos; encontrados $($altTexts.Count)"
foreach ($match in $altTexts) {
    $altText = $match.Groups[1].Value.Trim()
    Assert-Condition ($altText.Length -le 140) `
        "Texto alternativo excede 140 caracteres: $altText"
}

$releaseNotes = [System.IO.File]::ReadAllText(
    (Join-Path $playStoreRoot 'release-notes-pt-BR.txt')
).Trim()
Assert-Condition ($releaseNotes.Length -le 500) `
    "Notas da versao excedem 500 caracteres: $($releaseNotes.Length)"

Write-Output ((
        'PLAY_STORE_ASSETS_OK assets={0} name={1}/30 short={2}/80 ' +
        'full={3}/4000 release_notes={4}/500 alt_texts={5}'
    ) -f @(
        $validated.Count,
        $appName.Length,
        $shortDescription.Length,
        $fullDescription.Length,
        $releaseNotes.Length,
        $altTexts.Count
    ))

foreach ($asset in $validated) {
    Write-Output (
        '{0} {1}x{2} color_type={3} bytes={4} sha256={5}' -f
        $asset.Name,
        $asset.Width,
        $asset.Height,
        $asset.ColorType,
        $asset.Bytes,
        $asset.Sha256
    )
}
