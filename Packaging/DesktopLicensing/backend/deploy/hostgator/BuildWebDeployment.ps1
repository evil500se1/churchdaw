[CmdletBinding()]
param(
    [string] $Release = '2026-07-28.4',
    [string] $BaseConfigRelease = '2026-07-28.3',
    [string] $WooviCredentialFile = '',
    [switch] $CheckoutEnabled
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
$wooviWebhookAuthorizationPath = Join-Path $secretsRoot (
    'woovi-webhook-authorization.token'
)
$baseConfigPath = Join-Path $secretsRoot (
    "hostgator-production-$BaseConfigRelease.local.php"
)
if ([string]::IsNullOrWhiteSpace($WooviCredentialFile)) {
    $WooviCredentialFile = Join-Path $secretsRoot 'woovi-api-key.credential.xml'
}
$templatePath = Join-Path $PSScriptRoot 'web-bootstrap.php.template'
$outputRoot = Join-Path $projectRoot 'Releases\Hosting'

foreach ($requiredPath in @(
    $baseConfigPath,
    $licenseKeyPath,
    $templatePath,
    $WooviCredentialFile,
    $wooviWebhookAuthorizationPath
)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $requiredPath"
    }
}

$wooviCredential = Import-Clixml -LiteralPath $WooviCredentialFile
if ($wooviCredential -isnot [Management.Automation.PSCredential]) {
    throw 'Credencial Woovi invalida.'
}
$wooviApiKey = $wooviCredential.GetNetworkCredential().Password
if ([string]::IsNullOrWhiteSpace($wooviApiKey) -or
    $wooviApiKey.Length -lt 40) {
    throw 'Chave Woovi invalida.'
}
$wooviWebhookAuthorization = [IO.File]::ReadAllText(
    $wooviWebhookAuthorizationPath
).Trim()
if ($wooviWebhookAuthorization.Length -lt 32 -or
    $wooviWebhookAuthorization.Length -gt 256) {
    throw 'Autorizacao de webhook Woovi invalida.'
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

function Assert-PublishedCustomerDownloads([string] $ViewsPath) {
    $source = [IO.File]::ReadAllText($ViewsPath)
    $pattern = "(?<path>/app/(?:desktop|android)/ChurchDAW-" +
        "[A-Za-z0-9._+-]+\.(?:exe|dmg|apk))"
    $paths = @([regex]::Matches($source, $pattern) |
        ForEach-Object { $_.Groups['path'].Value } | Sort-Object -Unique)
    $expectedPatterns = @(
        '^/app/desktop/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+(?:-r[1-9][0-9]*)?-Setup\.exe$',
        '^/app/desktop/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+(?:-r[1-9][0-9]*)?(?:-macOS)?\.dmg$',
        '^/app/android/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+(?:-r[1-9][0-9]*)?-signed\.apk$'
    )
    if ($paths.Count -ne 3) {
        throw "Tela de sucesso precisa ter exatamente 3 downloads; encontrou $($paths.Count)"
    }
    foreach ($expected in $expectedPatterns) {
        if (@($paths | Where-Object { $_ -cmatch $expected }).Count -ne 1) {
            throw "Tela de sucesso nao possui um download valido para: $expected"
        }
    }

    $curl = (Get-Command curl.exe -CommandType Application -ErrorAction Stop |
        Select-Object -First 1).Source
    foreach ($path in $paths) {
        $url = 'https://churchdaw.com' + $path
        $status = @(& $curl '--disable' '--silent' '--show-error' '--location' `
            '--head' '--proto' '=https' '--tlsv1.2' '--connect-timeout' '30' `
            '--max-time' '60' '--output' 'NUL' '--write-out' '%{http_code}' `
            $url 2>&1)
        $exitCode = $LASTEXITCODE
        $statusText = ($status -join '').Trim()
        if ($exitCode -ne 0 -or $statusText -cne '200') {
            throw "Download da tela de sucesso indisponivel: $url (curl=$exitCode HTTP=$statusText)"
        }
    }
}

Assert-PublishedCustomerDownloads (Join-Path $backendRoot 'src\Views.php')

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
    $managedKeys = @(
        'admin.enabled',
        'admin.username',
        'admin.password_hash',
        'woovi.enabled',
        'woovi.api_key',
        'woovi.webhook_authorization',
        'woovi.sandbox',
        'woovi.charge_expiration_minutes',
        'checkout.provider',
        'checkout.enabled',
        'plan.single.price_cents',
        'plan.complete.price_cents',
        'product.price_cents',
        'legal.terms_version',
        'legal.privacy_version'
    )
    $configLines = [regex]::Split($configText, '\r?\n')
    $cleanConfigLines = [Collections.Generic.List[string]]::new()
    for ($lineIndex = 0; $lineIndex -lt $configLines.Length; ++$lineIndex) {
        $line = [string] $configLines[$lineIndex]
        $entry = [regex]::Match(
            $line,
            "^[ \t]*'(?<key>[A-Za-z0-9_.]+)'[ \t]*=>[ \t]*(?<tail>.*)$"
        )
        if ($entry.Success -and
            $managedKeys -ccontains $entry.Groups['key'].Value) {
            $tail = $entry.Groups['tail'].Value.Trim()
            if ($tail -ne '') {
                if (-not $tail.EndsWith(',')) {
                    throw "Entrada gerenciada malformada: $($entry.Groups['key'].Value)"
                }
                continue
            }
            if ($lineIndex + 2 -ge $configLines.Length) {
                throw "Entrada gerenciada multiline incompleta: $($entry.Groups['key'].Value)"
            }
            $valueLine = ([string] $configLines[$lineIndex + 1]).Trim()
            $commaLine = ([string] $configLines[$lineIndex + 2]).Trim()
            if ($valueLine.Length -lt 2 -or
                $valueLine[0] -ne [char] 39 -or
                $valueLine[$valueLine.Length - 1] -ne [char] 39 -or
                $commaLine -cne ',') {
                throw "Entrada gerenciada multiline invalida: $($entry.Groups['key'].Value)"
            }
            $lineIndex += 2
            continue
        }
        $cleanConfigLines.Add($line)
    }
    $configText = $cleanConfigLines -join "`n"
    foreach ($managedKey in $managedKeys) {
        if ([regex]::IsMatch(
                $configText,
                "(?m)^[ \t]*'$([regex]::Escape($managedKey))'[ \t]*=>")) {
            throw "Config base preservou chave gerenciada antiga: $managedKey"
        }
    }
    $checkoutLiteral = if ($CheckoutEnabled) { 'true' } else { 'false' }
    $managedConfig = @(
        "    'admin.enabled' => true,",
        "    'admin.username' => 'igorteixeira',",
        ("    'admin.password_hash' => " +
            (ConvertTo-PhpSingleQuoted $adminPasswordHash) + ','),
        "    'woovi.enabled' => true,",
        ("    'woovi.api_key' => " +
            (ConvertTo-PhpSingleQuoted $wooviApiKey) + ','),
        ("    'woovi.webhook_authorization' => " +
            (ConvertTo-PhpSingleQuoted $wooviWebhookAuthorization) + ','),
        "    'woovi.sandbox' => false,",
        "    'woovi.charge_expiration_minutes' => 30,",
        "    'checkout.provider' => 'woovi',",
        "    'checkout.enabled' => $checkoutLiteral,",
        "    'plan.single.price_cents' => 12999,",
        "    'plan.complete.price_cents' => 19990,",
        "    'product.price_cents' => 19990,",
        "    'legal.terms_version' => '2026-08-15',",
        "    'legal.privacy_version' => '2026-08-15',"
    ) -join "`n"
    if ([regex]::Matches($configText, '(?m)^\];\s*$').Count -ne 1) {
        throw 'Config base sem terminador unico.'
    }
    $configText = [regex]::Replace(
        $configText,
        '(?m)^\];\s*$',
        $managedConfig + "`n];",
        1
    )
    foreach ($managedKey in $managedKeys) {
        $keyPattern = "(?m)^[ \t]*'$([regex]::Escape($managedKey))'" +
            "[ \t]*=>[^\r\n]*,[ \t]*$"
        if ([regex]::Matches($configText, $keyPattern).Count -ne 1) {
            throw "Config final nao possui uma unica chave gerenciada: $managedKey"
        }
    }
    [IO.File]::WriteAllText(
        (Join-Path $releaseRoot 'config\local.php'),
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
    Copy-Item -LiteralPath (Join-Path $releaseRoot 'config\local.php') `
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
    $wooviApiKey = $null
    $wooviWebhookAuthorization = $null
    $wooviCredential = $null
}
