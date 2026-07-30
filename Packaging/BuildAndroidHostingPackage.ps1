param(
    [string] $SourceRoot = "$PSScriptRoot\AndroidHosting",
    [string] $Output = "$PSScriptRoot\AndroidHosting\android-hosting-upload.zip"
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$sourceRootFull = [IO.Path]::GetFullPath($SourceRoot)
$outputFull = [IO.Path]::GetFullPath($Output)
$expectedRoot = [IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot 'AndroidHosting')
)

if ($sourceRootFull -ne $expectedRoot -or
    -not $outputFull.StartsWith(
        $expectedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    ) -or
    [IO.Path]::GetFileName($outputFull) -ne 'android-hosting-upload.zip') {
    throw "Caminho de pacote fora do AndroidHosting permitido: $outputFull"
}

$entries = [ordered] @{
    'app/android/.htaccess' = 'app\android\.htaccess'
    'app/android/manifest.json' = 'app\android\manifest.json'
    'app/android/update-policy.json' = 'app\android\update-policy.json'
    'app/android/update-policy-v2.json' = 'app\android\update-policy-v2.json'
    'app/android/packs/.htaccess' = 'app\android\packs\.htaccess'
    'politica-de-privacidade.html' = 'politica-de-privacidade.html'
}

foreach ($relativePath in $entries.Values) {
    $fullPath = Join-Path $sourceRootFull $relativePath
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Arquivo obrigatorio da hospedagem ausente: $fullPath"
    }
}

if (Test-Path -LiteralPath $outputFull) {
    Remove-Item -LiteralPath $outputFull -Force
}

$archive = [IO.Compression.ZipFile]::Open(
    $outputFull,
    [IO.Compression.ZipArchiveMode]::Create
)
try {
    foreach ($entryName in $entries.Keys) {
        $sourcePath = Join-Path $sourceRootFull $entries[$entryName]
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $archive,
            $sourcePath,
            $entryName,
            [IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally {
    $archive.Dispose()
}

$verify = [IO.Compression.ZipFile]::OpenRead($outputFull)
try {
    $actualEntries = @($verify.Entries | ForEach-Object { $_.FullName })
    foreach ($entryName in $entries.Keys) {
        if ($entryName -notin $actualEntries) {
            throw "Entrada ausente do ZIP: $entryName"
        }
    }
    if ($actualEntries.Count -ne $entries.Count) {
        throw "ZIP contem entradas inesperadas: $($actualEntries -join ', ')"
    }
} finally {
    $verify.Dispose()
}

$item = Get-Item -LiteralPath $outputFull
[pscustomobject] @{
    Output = $item.FullName
    Bytes = $item.Length
    Entries = $entries.Count
    Sha256 = (Get-FileHash -LiteralPath $outputFull -Algorithm SHA256).Hash
}
