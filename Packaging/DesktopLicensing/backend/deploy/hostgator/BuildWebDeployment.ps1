[CmdletBinding()]
param(
    [string] $Release = '2026-07-28.4',
    [string] $BaseConfigRelease = '2026-07-28.3'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($Release -notmatch '^[A-Za-z0-9._-]{1,64}$') {
    throw 'Release invalida.'
}

$backendRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectRoot = (Resolve-Path (Join-Path $backendRoot '..\..\..')).Path
$secretsRoot = Join-Path $projectRoot '.secrets'
$licenseKeyPath = Join-Path $secretsRoot 'churchdaw-license-private.pem'
$baseConfigPath = Join-Path $secretsRoot (
    "hostgator-production-$BaseConfigRelease.local.php"
)
$templatePath = Join-Path $PSScriptRoot 'web-bootstrap.php.template'
$outputRoot = Join-Path $projectRoot 'Releases\Hosting'

foreach ($requiredPath in @(
    $baseConfigPath,
    $licenseKeyPath,
    $templatePath
)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $requiredPath"
    }
}

function New-RandomBytes([int] $Count) {
    $bytes = [byte[]]::new($Count)
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
    }
    finally {
        $rng.Dispose()
    }
    return $bytes
}

function ConvertTo-PhpSingleQuoted([string] $Value) {
    return "'" + $Value.Replace('\', '\\').Replace("'", "\'") + "'"
}

function ConvertTo-LowerHex([byte[]] $Bytes) {
    return ([BitConverter]::ToString($Bytes)).Replace('-', '').ToLowerInvariant()
}

$adminPasswordHash =
    [Environment]::GetEnvironmentVariable('CHURCHDAW_ADMIN_PASSWORD_HASH')
if ([string]::IsNullOrWhiteSpace($adminPasswordHash) -or
    $adminPasswordHash -notmatch '^\$argon2id\$') {
    throw 'CHURCHDAW_ADMIN_PASSWORD_HASH Argon2id ausente.'
}

$deployToken = [Convert]::ToBase64String((New-RandomBytes 48)).
    TrimEnd('=').Replace('+', '-').Replace('/', '_')
$deployTokenBytes = [Text.Encoding]::UTF8.GetBytes($deployToken)
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $deployTokenHash = ConvertTo-LowerHex (
        $sha.ComputeHash($deployTokenBytes)
    )
}
finally {
    $sha.Dispose()
}
# A HostGator bloqueia dotfiles publicos antes de o PHP receber a requisicao.
# O nome continua imprevisivel e o endpoint exige o token secreto no header;
# o bootstrap remove a si mesmo ao final do deploy.
$bootstrapName = 'churchdaw-deploy-' +
    (ConvertTo-LowerHex (New-RandomBytes 12)) + '.php'

$stageRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-hostgator-' + [Guid]::NewGuid().ToString('N')
)
$releaseRoot = Join-Path $stageRoot "churchdaw\releases\$Release"
$sharedRoot = Join-Path $stageRoot 'churchdaw\shared'
$deployPublic = Join-Path $stageRoot 'churchdaw\deploy-public'
$publicRoot = Join-Path $stageRoot 'public_html'

try {
    foreach ($directory in @(
        (Join-Path $releaseRoot 'bin'),
        (Join-Path $releaseRoot 'config'),
        (Join-Path $releaseRoot 'migrations'),
        (Join-Path $releaseRoot 'public'),
        (Join-Path $releaseRoot 'src'),
        (Join-Path $sharedRoot 'config'),
        (Join-Path $sharedRoot 'keys'),
        $deployPublic,
        $publicRoot
    )) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    Copy-Item -Path (Join-Path $backendRoot 'bin\*') `
        -Destination (Join-Path $releaseRoot 'bin') -Recurse -Force
    Copy-Item -Path (Join-Path $backendRoot 'migrations\*') `
        -Destination (Join-Path $releaseRoot 'migrations') -Recurse -Force
    Copy-Item -Path (Join-Path $backendRoot 'public\*') `
        -Destination (Join-Path $releaseRoot 'public') -Recurse -Force
    Copy-Item -Path (Join-Path $backendRoot 'src\*') `
        -Destination (Join-Path $releaseRoot 'src') -Recurse -Force
    Copy-Item -LiteralPath $licenseKeyPath `
        -Destination (Join-Path $sharedRoot 'keys\churchdaw-license-private.pem')
    Copy-Item -LiteralPath (
        Join-Path $PSScriptRoot 'public_html\churchdaw-account.php'
    ) -Destination (Join-Path $deployPublic 'churchdaw-account.php')
    Copy-Item -LiteralPath (
        Join-Path $PSScriptRoot 'public_html\account-assets\.htaccess'
    ) -Destination (Join-Path $deployPublic 'account-assets.htaccess')
    Copy-Item -LiteralPath (
        Join-Path $PSScriptRoot 'public_html\churchdaw-routes.htaccess'
    ) -Destination (Join-Path $deployPublic 'churchdaw-routes.htaccess')

    $utf8NoBom = [Text.UTF8Encoding]::new($false)
    $configText = [IO.File]::ReadAllText($baseConfigPath)
    $releasePattern = "('app\.release'\s*=>\s*)'[^']+'"
    if ([regex]::Matches($configText, $releasePattern).Count -ne 1) {
        throw 'Config base sem uma unica app.release.'
    }
    $configText = [regex]::Replace(
        $configText,
        $releasePattern,
        ('$1' + (ConvertTo-PhpSingleQuoted $Release)),
        1
    )
    foreach ($adminKey in @(
        'admin.enabled',
        'admin.username',
        'admin.password_hash'
    )) {
        $configText = [regex]::Replace(
            $configText,
            "(?m)^\s*'$([regex]::Escape($adminKey))'\s*=>\s*.*?,\s*$",
            ''
        )
    }
    $adminConfig = @(
        "    'admin.enabled' => true,",
        "    'admin.username' => 'igorteixeira',",
        "    'admin.password_hash' => " +
            (ConvertTo-PhpSingleQuoted $adminPasswordHash) + ','
    ) -join "`n"
    if ([regex]::Matches($configText, '(?m)^\];\s*$').Count -ne 1) {
        throw 'Config base sem terminador unico.'
    }
    $configText = [regex]::Replace(
        $configText,
        '(?m)^\];\s*$',
        $adminConfig + "`n];",
        1
    )
    [IO.File]::WriteAllText(
        (Join-Path $sharedRoot 'config\local.php'),
        $configText,
        $utf8NoBom
    )

    $bootstrap = [IO.File]::ReadAllText($templatePath).
        Replace('__RELEASE__', $Release).
        Replace('__TOKEN_HASH__', $deployTokenHash)
    [IO.File]::WriteAllText(
        (Join-Path $publicRoot $bootstrapName),
        $bootstrap,
        $utf8NoBom
    )

    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
    $archivePath = Join-Path $outputRoot "ChurchDAW-HostGator-$Release.zip"
    if (Test-Path -LiteralPath $archivePath) {
        throw "O archive da release ja existe: $archivePath"
    }
    Compress-Archive -Path (Join-Path $stageRoot '*') `
        -DestinationPath $archivePath -CompressionLevel Optimal

    $productionConfigBackup = Join-Path $secretsRoot (
        "hostgator-production-$Release.local.php"
    )
    Copy-Item -LiteralPath (Join-Path $sharedRoot 'config\local.php') `
        -Destination $productionConfigBackup
    $deployTokenPath = Join-Path $secretsRoot (
        "hostgator-deploy-$Release.token"
    )
    [IO.File]::WriteAllText($deployTokenPath, $deployToken, $utf8NoBom)

    foreach ($secretFile in @($productionConfigBackup, $deployTokenPath)) {
        & icacls.exe $secretFile /inheritance:r `
            /grant:r 'Administrator:(F)' 'SYSTEM:(F)' `
            'BUILTIN\Administrators:(F)' | Out-Null
    }

    $archiveHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
    [pscustomobject]@{
        Release = $Release
        Archive = $archivePath
        Sha256 = $archiveHash
        Bytes = (Get-Item -LiteralPath $archivePath).Length
        Bootstrap = $bootstrapName
        DeployTokenFile = $deployTokenPath
        ConfigBackup = $productionConfigBackup
    }
}
finally {
    if (Test-Path -LiteralPath $stageRoot) {
        Remove-Item -LiteralPath $stageRoot -Recurse -Force
    }
    $adminPasswordHash = $null
}
