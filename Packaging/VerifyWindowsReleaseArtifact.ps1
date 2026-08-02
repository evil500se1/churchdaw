[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ArtifactRoot,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v[0-9]+\.[0-9]+\.[0-9]+$')]
    [string] $ReleaseTag,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string] $SourceCommit,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $ExpectedPublisher,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40,128}$')]
    [string] $ExpectedThumbprint,

    [string] $MetadataOutput = '',
    [string] $GitHubOutput = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')

function Get-StrictJson {
    param([Parameter(Mandatory = $true)][string] $Path)

    $encoding = [Text.UTF8Encoding]::new($false, $true)
    try {
        $text = [IO.File]::ReadAllText($Path, $encoding)
        $python = Get-Command python.exe -CommandType Application `
            -ErrorAction Stop | Select-Object -First 1
        $duplicateCheck = @'
import json
import pathlib
import sys

def strict_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON key: {key}")
        value[key] = item
    return value

raw = pathlib.Path(sys.argv[1]).read_bytes()
if raw.startswith(b"\xef\xbb\xbf"):
    raise ValueError("UTF-8 BOM is not allowed")
json.loads(raw.decode("utf-8", errors="strict"), object_pairs_hook=strict_object)
'@
        & $python.Source -c $duplicateCheck $Path
        if ($LASTEXITCODE -ne 0) {
            throw 'Python recusou chaves duplicadas/UTF-8 invalido'
        }
        return $text | ConvertFrom-Json
    }
    catch {
        throw "JSON invalido em '$Path': $($_.Exception.Message)"
    }
}

function Get-Property {
    param(
        [Parameter(Mandatory = $true)] $Object,
        [Parameter(Mandatory = $true)][string] $Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Campo obrigatorio ausente: $Name"
    }
    return $property.Value
}

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)] $Actual,
        [Parameter(Mandatory = $true)] $Expected,
        [Parameter(Mandatory = $true)][string] $Label
    )

    if ($Expected -is [string]) {
        if ($Actual -isnot [string] -or
            -not $Actual.Equals(
                $Expected,
                [StringComparison]::Ordinal)) {
            throw "$Label diverge: '$Actual' != '$Expected'"
        }
        return
    }
    if ($Expected -is [bool]) {
        if ($Actual -isnot [bool] -or $Actual -ne $Expected) {
            throw "$Label diverge ou tem tipo invalido: '$Actual'"
        }
        return
    }
    if ($Expected -is [int] -or $Expected -is [long]) {
        if (($Actual -isnot [int] -and $Actual -isnot [long]) -or
            [long] $Actual -ne [long] $Expected) {
            throw "$Label diverge ou tem tipo invalido: '$Actual'"
        }
        return
    }
    if ($Actual -ne $Expected) {
        throw "$Label diverge: '$Actual' != '$Expected'"
    }
}

function Assert-PropertySet {
    param(
        [Parameter(Mandatory = $true)] $Object,
        [Parameter(Mandatory = $true)][string[]] $Expected,
        [Parameter(Mandatory = $true)][string] $Label
    )

    $actual = @($Object.PSObject.Properties.Name | Sort-Object)
    $wanted = @($Expected | Sort-Object)
    if (($actual -join "`n") -cne ($wanted -join "`n")) {
        throw ("$Label tem campos inesperados. Atual=[$($actual -join ',')]; " +
               "esperado=[$($wanted -join ',')]")
    }
}

function Resolve-UniqueFile {
    param([string] $Root, [string] $Name)

    $matches = @(Get-ChildItem -LiteralPath $Root -Recurse -File |
        Where-Object { $_.Name.Equals($Name, [StringComparison]::Ordinal) })
    if ($matches.Count -ne 1) {
        throw "Esperava exatamente um '$Name'; encontrei $($matches.Count)"
    }
    return $matches[0].FullName
}

function Get-VersionCode {
    param([string] $Version)

    $parts = @($Version.Split('.') | ForEach-Object { [int] $_ })
    if ($parts.Count -ne 3 -or $parts[0] -gt 9999 -or
        $parts[1] -gt 99 -or $parts[2] -gt 99) {
        throw "Versao fora da faixa suportada: $Version"
    }
    return [int] ($parts[0] * 10000 + $parts[1] * 100 + $parts[2])
}

function Assert-Checksums {
    param([string] $Root, [string] $Checksums)

    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $records = @{}
    $lineNumber = 0
    foreach ($line in [IO.File]::ReadAllLines($Checksums)) {
        ++$lineNumber
        if ($line -notmatch '^([0-9a-f]{64})  ([^\r\n]+)$') {
            throw "SHA256SUMS linha $lineNumber invalida"
        }
        $hash = $matches[1]
        $relative = $matches[2]
        if ($relative.Contains('\') -or $relative.StartsWith('/') -or
            $relative -match '(^|/)\.\.(/|$)' -or
            $relative.IndexOfAny([IO.Path]::GetInvalidPathChars()) -ge 0 -or
            $records.ContainsKey($relative)) {
            throw "Caminho inseguro ou duplicado em SHA256SUMS: $relative"
        }
        $candidate = [IO.Path]::GetFullPath(
            (Join-Path $Root ($relative.Replace('/', '\'))))
        if (-not $candidate.StartsWith(
                $rootFull,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            throw "Arquivo de SHA256SUMS ausente ou fora da raiz: $relative"
        }
        $actual = (Get-FileHash -LiteralPath $candidate `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -cne $hash) {
            throw "Hash divergente em SHA256SUMS: $relative"
        }
        $records[$relative] = $true
    }

    $actualFiles = @(Get-ChildItem -LiteralPath $Root -Recurse -File |
        Where-Object { $_.FullName -ne $Checksums } |
        ForEach-Object {
            $_.FullName.Substring($rootFull.Length).Replace('\', '/')
        })
    if ($actualFiles.Count -ne $records.Count) {
        throw ('SHA256SUMS nao cobre exatamente o artefato: ' +
               "$($actualFiles.Count) arquivos, $($records.Count) registros")
    }
    foreach ($relative in $actualFiles) {
        if (-not $records.ContainsKey($relative)) {
            throw "Arquivo nao coberto por SHA256SUMS: $relative"
        }
    }
}

$root = [IO.Path]::GetFullPath($ArtifactRoot)
if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "Raiz do artefato Windows ausente: $root"
}

$version = $ReleaseTag.Substring(1)
$versionCode = Get-VersionCode $version
$manifestPath = Resolve-UniqueFile $root 'release-manifest.json'
$checksumsPath = Resolve-UniqueFile $root 'SHA256SUMS.txt'
$installerName = "ChurchDAW-$version-Setup.exe"
$installerPath = Resolve-UniqueFile $root $installerName
$appPath = Resolve-UniqueFile $root 'ChurchDAW.exe'
$extractedPath = Resolve-UniqueFile $root `
    'ChurchDAW-installer-payload.extracted.exe'

$artifactDirectory = Split-Path -Parent $manifestPath
foreach ($path in @($checksumsPath, $installerPath, $appPath, $extractedPath)) {
    if (-not (Split-Path -Parent $path).Equals(
            $artifactDirectory,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Arquivos principais nao pertencem ao mesmo diretorio do manifest'
    }
}

Assert-Checksums $artifactDirectory $checksumsPath
$manifest = Get-StrictJson $manifestPath
Assert-PropertySet $manifest @(
    'schema_version', 'product_id', 'product_name', 'platform',
    'architecture', 'release_mode', 'distribution_approved',
    'version_name', 'version_code', 'generated_at_utc', 'source',
    'toolchain', 'gates', 'signing', 'embedded_payload', 'update_policy',
    'artifacts', 'logs_directory'
) 'manifest'
Assert-Equal (Get-Property $manifest 'schema_version') 1 'schema_version'
Assert-Equal (Get-Property $manifest 'product_id') `
    'com.churchdaw.app' 'product_id'
Assert-Equal (Get-Property $manifest 'product_name') 'ChurchDAW' 'product_name'
Assert-Equal (Get-Property $manifest 'platform') 'windows' 'platform'
Assert-Equal (Get-Property $manifest 'architecture') 'x64' 'architecture'
Assert-Equal (Get-Property $manifest 'release_mode') 'commercial' 'release_mode'
Assert-Equal (Get-Property $manifest 'distribution_approved') $true `
    'distribution_approved'
Assert-Equal (Get-Property $manifest 'version_name') $version 'version_name'
Assert-Equal (Get-Property $manifest 'version_code') $versionCode 'version_code'
Assert-Equal (Get-Property $manifest 'logs_directory') 'logs' 'logs_directory'
$generatedAt = Get-Property $manifest 'generated_at_utc'
if ($generatedAt -isnot [string] -or $generatedAt -notmatch
        '^20[0-9]{2}-[0-9]{2}-[0-9]{2}T') {
    throw 'generated_at_utc ausente ou invalido'
}

$source = Get-Property $manifest 'source'
Assert-PropertySet $source @(
    'git_commit', 'git_branch', 'clean_at_start', 'clean_at_end',
    'dirty_entries_at_start'
) 'source'
Assert-Equal (Get-Property $source 'git_commit') `
    $SourceCommit.ToLowerInvariant() 'source.git_commit'
Assert-Equal (Get-Property $source 'clean_at_start') $true `
    'source.clean_at_start'
Assert-Equal (Get-Property $source 'clean_at_end') $true 'source.clean_at_end'
if (@(Get-Property $source 'dirty_entries_at_start').Count -ne 0) {
    throw 'Manifest declara worktree sujo no inicio do build'
}

$toolchain = Get-Property $manifest 'toolchain'
Assert-PropertySet $toolchain @(
    'cmake', 'ctest', 'generator', 'toolset', 'configuration',
    'architecture', 'parallel'
) 'toolchain'
Assert-Equal (Get-Property $toolchain 'generator') `
    'Visual Studio 17 2022' 'toolchain.generator'
Assert-Equal (Get-Property $toolchain 'toolset') 'v143' 'toolchain.toolset'
Assert-Equal (Get-Property $toolchain 'configuration') `
    'Release' 'toolchain.configuration'
Assert-Equal (Get-Property $toolchain 'architecture') `
    'x64' 'toolchain.architecture'
if ((Get-Property $toolchain 'parallel') -isnot [int] -and
    (Get-Property $toolchain 'parallel') -isnot [long]) {
    throw 'toolchain.parallel precisa ser inteiro'
}

$gates = Get-Property $manifest 'gates'
Assert-PropertySet $gates @(
    'licensing_enabled', 'http_localhost_disabled',
    'update_localhost_disabled', 'license_tests', 'audio_tests',
    'reverb_bounds_asan', 'update_policy_tests', 'pe_x64',
    'version_match', 'intermediate_payload_sha256_match',
    'extracted_rcdata_sha256_match', 'signatures_required',
    'timestamp_required', 'signtool_verify'
) 'gates'
foreach ($name in @(
    'licensing_enabled',
    'http_localhost_disabled',
    'update_localhost_disabled',
    'pe_x64',
    'version_match',
    'intermediate_payload_sha256_match',
    'extracted_rcdata_sha256_match',
    'signatures_required',
    'timestamp_required'
)) {
    Assert-Equal (Get-Property $gates $name) $true "gates.$name"
}
foreach ($name in @('license_tests', 'audio_tests', 'reverb_bounds_asan',
                     'update_policy_tests')) {
    Assert-Equal (Get-Property $gates $name) 'passed' "gates.$name"
}
if (-not ([string] (Get-Property $gates 'signtool_verify')).StartsWith(
        '/pa /all /tw /v passed',
        [StringComparison]::Ordinal)) {
    throw 'Manifest nao comprova signtool /tw'
}

$signing = Get-Property $manifest 'signing'
Assert-PropertySet $signing @(
    'signed', 'publisher', 'signer_thumbprint',
    'same_signer_app_installer', 'timestamped'
) 'signing'
Assert-Equal (Get-Property $signing 'signed') $true 'signing.signed'
Assert-Equal (Get-Property $signing 'same_signer_app_installer') $true `
    'signing.same_signer_app_installer'
Assert-Equal (Get-Property $signing 'timestamped') $true 'signing.timestamped'
$declaredPublisher = [string] (Get-Property $signing 'publisher')
$declaredThumbprint = [string] (Get-Property $signing 'signer_thumbprint')
if ([string]::IsNullOrWhiteSpace($declaredPublisher) -or
    $declaredThumbprint -notmatch '^[0-9A-Fa-f]{40,128}$') {
    throw 'Identidade Authenticode ausente ou invalida no manifest'
}
if (-not $declaredPublisher.Equals(
        $ExpectedPublisher.Trim(),
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'publisher do manifest diverge da configuracao protegida'
}
if (-not $declaredThumbprint.Equals(
        $ExpectedThumbprint,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'thumbprint do manifest diverge da configuracao protegida'
}

$records = @(Get-Property $manifest 'artifacts')
$expectedRoleFiles = [ordered] @{
    windows_app = 'ChurchDAW.exe'
    windows_installer = $installerName
    installer_payload_extracted =
        'ChurchDAW-installer-payload.extracted.exe'
    desktop_update_policy = 'update-policy-v1.json'
}
if ($records.Count -ne $expectedRoleFiles.Count) {
    throw "Quantidade inesperada de artifacts no manifest: $($records.Count)"
}
foreach ($role in $expectedRoleFiles.Keys) {
    $matched = @($records | Where-Object { [string] $_.role -ceq $role })
    if ($matched.Count -ne 1) {
        throw "Manifest exige exatamente um artifact role=$role"
    }
    $record = $matched[0]
    Assert-PropertySet $record @('role', 'file', 'bytes', 'sha256') `
        "artifacts[$role]"
    Assert-Equal (Get-Property $record 'role') $role "$role.role"
    $name = [string] (Get-Property $record 'file')
    Assert-Equal $name $expectedRoleFiles[$role] "$role.file"
    $path = Join-Path $artifactDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Arquivo do manifest ausente: $name"
    }
    $item = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $path `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Equal (Get-Property $record 'bytes') $item.Length "$role.bytes"
    Assert-Equal (Get-Property $record 'sha256') $hash "$role.sha256"
}

$installerIdentity = Assert-ChurchDAWPeReleaseIdentity `
    -Path $installerPath -ExpectedVersion $version -ExpectedProductName ChurchDAW
Assert-Equal $installerIdentity.OriginalFilename $installerName `
    'OriginalFilename do instalador'
$null = Assert-ChurchDAWPeReleaseIdentity `
    -Path $appPath -ExpectedVersion $version -ExpectedProductName ChurchDAW
$null = Assert-ChurchDAWPeReleaseIdentity `
    -Path $extractedPath -ExpectedVersion $version -ExpectedProductName ChurchDAW

$tempBase = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetTempPath()
} else { $env:RUNNER_TEMP }
$signatureLog = Join-Path $tempBase `
    'churchdaw-publisher-windows-authenticode.log'
$appSignature = Assert-ChurchDAWAuthenticodeSignature `
    -Path $appPath -ExpectedPublisher $ExpectedPublisher `
    -ExpectedSignerThumbprints @($ExpectedThumbprint) `
    -RequireTimestamp $true -RequirePublicChain $true
$installerSignature = Assert-ChurchDAWAuthenticodeSignature `
    -Path $installerPath -ExpectedPublisher $ExpectedPublisher `
    -ExpectedSignerThumbprints @($ExpectedThumbprint) `
    -RequireTimestamp $true -RequirePublicChain $true `
    -VerificationLogPath $signatureLog
$extractedSignature = Assert-ChurchDAWAuthenticodeSignature `
    -Path $extractedPath -ExpectedPublisher $ExpectedPublisher `
    -ExpectedSignerThumbprints @($ExpectedThumbprint) `
    -RequireTimestamp $true -RequirePublicChain $true
Assert-ChurchDAWSameSigner $appSignature $installerSignature `
    'ChurchDAW.exe' $installerName
Assert-ChurchDAWSameSigner $appSignature $extractedSignature `
    'ChurchDAW.exe' 'payload extraido'
Assert-Equal $installerSignature.Publisher $declaredPublisher `
    'publisher manifest/assinatura'
Assert-Equal $installerSignature.SignerThumbprint $declaredThumbprint `
    'thumbprint manifest/assinatura'

$embedded = Get-Property $manifest 'embedded_payload'
Assert-PropertySet $embedded @(
    'resource_type', 'resource_id', 'app_sha256', 'intermediate_sha256',
    'extracted_sha256', 'bytes'
) 'embedded_payload'
Assert-Equal (Get-Property $embedded 'resource_type') `
    'RCDATA' 'embedded_payload.resource_type'
Assert-Equal (Get-Property $embedded 'resource_id') `
    1 'embedded_payload.resource_id'
$appHash = (Get-FileHash -LiteralPath $appPath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$extractedHash = (Get-FileHash -LiteralPath $extractedPath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-Equal (Get-Property $embedded 'app_sha256') $appHash `
    'embedded_payload.app_sha256'
Assert-Equal (Get-Property $embedded 'intermediate_sha256') $appHash `
    'embedded_payload.intermediate_sha256'
Assert-Equal (Get-Property $embedded 'extracted_sha256') $extractedHash `
    'embedded_payload.extracted_sha256'
Assert-Equal $extractedHash $appHash 'payload extraido/app'
Assert-Equal (Get-Property $embedded 'bytes') `
    (Get-Item -LiteralPath $extractedPath).Length 'embedded_payload.bytes'

$updatePolicy = Get-Property $manifest 'update_policy'
Assert-PropertySet $updatePolicy @(
    'generated', 'policy_serial', 'minimum_version_code'
) 'update_policy'
Assert-Equal (Get-Property $updatePolicy 'generated') $true `
    'update_policy.generated'
foreach ($name in @('policy_serial', 'minimum_version_code')) {
    $value = Get-Property $updatePolicy $name
    if (($value -isnot [int] -and $value -isnot [long]) -or
        [long] $value -lt 1) {
        throw "update_policy.$name precisa ser inteiro positivo"
    }
}

# O arquivo .extracted do artefato e apenas evidencia do builder. A prova do
# publicador vem de uma nova extracao do RCDATA diretamente do Setup baixado.
$verificationTemp = Join-Path $tempBase (
    'churchdaw-windows-artifact-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $verificationTemp | Out-Null
try {
    $freshPayload = Join-Path $verificationTemp 'ChurchDAW.exe'
    $freshExtraction = Export-ChurchDAWInstallerPayload `
        -Installer $installerPath -Output $freshPayload -ResourceId 1
    Assert-Equal $freshExtraction.Bytes `
        (Get-Item -LiteralPath $appPath).Length 'RCDATA novo bytes/app'
    Assert-Equal $freshExtraction.Sha256 $appHash 'RCDATA novo SHA/app'
    $null = Assert-ChurchDAWPeReleaseIdentity `
        -Path $freshPayload -ExpectedVersion $version `
        -ExpectedProductName ChurchDAW
    $freshSignature = Assert-ChurchDAWAuthenticodeSignature `
        -Path $freshPayload -ExpectedPublisher $ExpectedPublisher `
        -ExpectedSignerThumbprints @($ExpectedThumbprint) `
        -RequireTimestamp $true -RequirePublicChain $true
    Assert-ChurchDAWSameSigner $appSignature $freshSignature `
        'ChurchDAW.exe' 'RCDATA reextraido'
}
finally {
    $freshPayload = Join-Path $verificationTemp 'ChurchDAW.exe'
    if (Test-Path -LiteralPath $freshPayload -PathType Leaf) {
        Remove-Item -LiteralPath $freshPayload -Force
    }
    if (Test-Path -LiteralPath $verificationTemp -PathType Container) {
        if (@(Get-ChildItem -LiteralPath $verificationTemp -Force).Count -ne 0) {
            throw 'Diretorio temporario de extracao nao ficou vazio'
        }
        Remove-Item -LiteralPath $verificationTemp -Force
    }
}

$installerItem = Get-Item -LiteralPath $installerPath
$metadata = [ordered] @{
    schema_version = 1
    platform = 'windows'
    release_tag = $ReleaseTag
    release_version = $version
    version_code = $versionCode
    source_commit = $SourceCommit.ToLowerInvariant()
    installer_name = $installerName
    installer_path = $installerPath
    installer_bytes = [long] $installerItem.Length
    installer_sha256 = (Get-FileHash -LiteralPath $installerPath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    publisher = $installerSignature.Publisher
    signer_thumbprint = $installerSignature.SignerThumbprint
    manifest_sha256 = (Get-FileHash -LiteralPath $manifestPath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    candidate_policy_used_for_publication = $false
}

if (-not [string]::IsNullOrWhiteSpace($MetadataOutput)) {
    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($MetadataOutput))
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    [IO.File]::WriteAllText(
        [IO.Path]::GetFullPath($MetadataOutput),
        ($metadata | ConvertTo-Json -Depth 6) + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

if (-not [string]::IsNullOrWhiteSpace($GitHubOutput)) {
    $outputLines = @(
        "installer_sha256=$($metadata.installer_sha256)",
        "installer_bytes=$($metadata.installer_bytes)",
        "manifest_sha256=$($metadata.manifest_sha256)"
    )
    [IO.File]::AppendAllLines(
        [IO.Path]::GetFullPath($GitHubOutput),
        $outputLines,
        [Text.UTF8Encoding]::new($false))
}

Write-Host 'VERIFY_WINDOWS_RELEASE_ARTIFACT_OK'
Write-Host "Installer: $installerPath"
Write-Host "SHA-256: $($metadata.installer_sha256)"
Write-Host "Bytes: $($metadata.installer_bytes)"
