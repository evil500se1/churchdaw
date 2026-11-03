[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$lineageScript = Join-Path $PSScriptRoot `
    'NewAndroidDirectApkWithSigningLineage.ps1'
$localBuilder = Join-Path $PSScriptRoot 'BuildSignedAndroidRelease.ps1'
$buildWorkflow = Join-Path $PSScriptRoot `
    '..\.github\workflows\build-android.yml'
$publishWorkflow = Join-Path $PSScriptRoot `
    '..\.github\workflows\release-android-publish.yml'
$publisher = Join-Path $PSScriptRoot 'PublishAndroidRelease.ps1'

foreach ($path in @(
    $lineageScript, $localBuilder, $buildWorkflow, $publishWorkflow, $publisher
)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $path"
    }
}

$lineage = [IO.File]::ReadAllText($lineageScript)
$builder = [IO.File]::ReadAllText($localBuilder)
$build = [IO.File]::ReadAllText($buildWorkflow)
$publish = [IO.File]::ReadAllText($publishWorkflow)
$publisherSource = [IO.File]::ReadAllText($publisher)

function Assert-Contains {
    param([string] $Source, [string] $Needle, [string] $Label)

    if (-not $Source.Contains($Needle)) {
        throw "Protecao Android ausente: $Label"
    }
}

function Assert-Count {
    param(
        [string] $Source,
        [string] $Pattern,
        [int] $Expected,
        [string] $Label
    )

    $count = [regex]::Matches($Source, $Pattern).Count
    if ($count -ne $Expected) {
        throw "$Label tem contagem $count; esperado $Expected"
    }
}

Assert-Count $lineage "'rotate'" 2 'Etapas de rotacao'
Assert-Count $lineage "'--set-installed-data', 'true'" 2 `
    'Permissoes de dados instalados'
Assert-Count $lineage "'--set-rollback', 'false'" 2 `
    'Bloqueios de rollback de assinatura'
Assert-Count $lineage "'--next-signer'" 2 'Transicoes de signatario no APK'
Assert-Contains $lineage "'--rotation-min-sdk-version', '28'" `
    'rotacao v3 com inicio na API 28'
Assert-Contains $lineage 'Assert-ApkForSdkRange $apkSignerPath $signedApk 24 27' `
    'verificacao F25 nas APIs 24-27'
Assert-Contains $lineage 'Assert-ApkForSdkRange $apkSignerPath $signedApk 28 $null' `
    'verificacao da chave atual na API 28+'

$f25Index = $lineage.IndexOf(
    "'--ks', `$f25Path",
    [StringComparison]::Ordinal)
$legacy415aIndex = $lineage.IndexOf(
    "'--ks', `$legacy415aPath",
    $f25Index + 1,
    [StringComparison]::Ordinal)
$currentIndex = $lineage.LastIndexOf(
    "'--ks', `$currentPath",
    [StringComparison]::Ordinal)
if ($f25Index -lt 0 -or $legacy415aIndex -le $f25Index -or
    $currentIndex -le $legacy415aIndex) {
    throw 'Ordem criptografica F25 -> 415A -> atual foi alterada'
}

foreach ($needle in @(
    'NewAndroidDirectApkWithSigningLineage.ps1',
    'churchdaw-signing-lineage.bin',
    'ExpectedLegacyF25CertificateSha256',
    'ExpectedLegacy415aCertificateSha256'
)) {
    Assert-Contains $builder $needle "builder local: $needle"
}

foreach ($needle in @(
    'ANDROID_LEGACY_F25_KEYSTORE_BASE64',
    'ANDROID_LEGACY_415A_KEYSTORE_BASE64',
    'ANDROID_LEGACY_STORE_PASSWORD',
    'NewAndroidDirectApkWithSigningLineage.ps1',
    'legacy_f25_certificate_sha256',
    'legacy_415a_certificate_sha256',
    'signing_lineage_sha256',
    'f25-to-415a-to-current-v3-rotation'
)) {
    Assert-Contains $build $needle "workflow de build: $needle"
}

foreach ($needle in @(
    '$allFiles.Count -ne 3',
    'Assert-ApkSignerRoute 24 27',
    'Assert-ApkSignerRoute 28 $null',
    'LINEAGE_VERIFIED_SHA256',
    'ExpectedLegacyF25CertificateSha256',
    'ExpectedLegacy415aCertificateSha256',
    'VerifiedLineageSha256'
)) {
    Assert-Contains $publish $needle "workflow de publicacao: $needle"
}

foreach ($needle in @(
    'churchdaw-signing-lineage.bin',
    'metadata.legacy_f25_certificate_sha256',
    'metadata.legacy_415a_certificate_sha256',
    'metadata.signing_lineage_sha256',
    'metadata.rotation_min_sdk_version precisa ser 28'
)) {
    Assert-Contains $publisherSource $needle "publicador: $needle"
}

Write-Host 'TEST_ANDROID_SIGNING_MIGRATION_OK'
