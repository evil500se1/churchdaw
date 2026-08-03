[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $WindowsArtifactRoot,

    [Parameter(Mandatory = $true)]
    [string] $MacArtifactRoot,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v[0-9]+\.[0-9]+\.[0-9]+$')]
    [string] $ReleaseTag,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string] $SourceCommit,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $WindowsRunId,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $MacRunId,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $WindowsArtifactId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^sha256:[0-9a-fA-F]{64}$')]
    [string] $WindowsArtifactDigest,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $WindowsArtifactArchiveBytes,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $MacArtifactId,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^sha256:[0-9a-fA-F]{64}$')]
    [string] $MacArtifactDigest,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $MacArtifactArchiveBytes,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedPreviousPolicySha256,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 1000000000)]
    [int] $PolicySerial,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 1000000000)]
    [int] $MinimumVersionCode,

    [Parameter(Mandatory = $true)]
    [ValidateLength(1, 640)]
    [string] $UpdateMessage,

    [Parameter(Mandatory = $true)]
    [string] $PrivateKey,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedWindowsPublisher,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{40,128}$')]
    [string] $ExpectedWindowsThumbprint,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9.-]+$')]
    [string] $FtpsHost,

    [ValidateRange(1, 65535)]
    [int] $FtpsPort = 21,

    [Parameter(Mandatory = $true)]
    [string] $NetrcPath,

    [string] $RemoteRootDirectory = '/public_html',
    [string] $RemoteDesktopDirectory = '/public_html/app/desktop',
    [string] $PublicSiteUrl = 'https://churchdaw.com/',
    [string] $PublicDesktopBaseUrl = 'https://churchdaw.com/app/desktop',

    [Parameter(Mandatory = $true)]
    [string] $SiteIndex,

    [Parameter(Mandatory = $true)]
    [string] $AuditOutputDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $VerifiedWindowsSha256,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $VerifiedWindowsBytes,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $VerifiedMacSha256,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $VerifiedMacBytes,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $VerifiedWindowsManifestSha256,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $VerifiedMacManifestSha256,

    [ValidatePattern('^$|^https://churchdaw\.com/app/android/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+-signed\.apk$')]
    [string] $ExpectedAndroidDownloadUrl = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')

$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'
$publicKey = Join-Path $PSScriptRoot 'AndroidUpdatePolicyPublicKey.pem'
$policyScript = Join-Path $PSScriptRoot 'SignDesktopUpdatePolicy.ps1'
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$writeUtf8 = [Text.UTF8Encoding]::new($false)
$productId = 'com.churchdaw.app'
$policyKeyId = 'churchdaw-update-2026-01'
$policyAlgorithm = 'SHA256withRSA'
$auditEvents = [Collections.Generic.List[object]]::new()
$uploadedVersioned = [Collections.Generic.List[string]]::new()
$policyPromotionAttempted = $false
$sitePromotionAttempted = $false
$rollbackConfirmed = $false

function Add-AuditEvent {
    param([string] $Action, [string] $Result, [string] $Detail = '')

    $auditEvents.Add([ordered] @{
        timestamp_utc = [DateTimeOffset]::UtcNow.ToString('o')
        action = $Action
        result = $Result
        detail = $Detail
    })
}

function Assert-OrdinalEqual {
    param($Actual, $Expected, [string] $Label)

    if (-not ([string] $Actual).Equals(
            [string] $Expected,
            [StringComparison]::Ordinal)) {
        throw "$Label diverge: '$Actual' != '$Expected'"
    }
}

function Assert-SafeRemoteDirectory {
    param([string] $Value, [string] $Label)

    if ($Value -notmatch '^/[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)*$' -or
        $Value.Contains('..') -or $Value.EndsWith('/')) {
        throw "$Label inseguro: $Value"
    }
}

function Assert-SafeRemoteName {
    param([string] $Value, [string] $Label)

    if ($Value -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,159}$' -or
        $Value.Contains('..') -or [IO.Path]::GetFileName($Value) -cne $Value) {
        throw "$Label inseguro: $Value"
    }
}

function Resolve-UniqueFile {
    param([string] $Root, [string] $Name)

    $found = @(Get-ChildItem -LiteralPath $Root -Recurse -File |
        Where-Object { $_.Name.Equals($Name, [StringComparison]::Ordinal) })
    if ($found.Count -ne 1) {
        throw "Esperava exatamente um '$Name' em '$Root'; encontrei $($found.Count)"
    }
    return $found[0].FullName
}

function Get-FileRecord {
    param([string] $Path)

    $item = Get-Item -LiteralPath $Path
    return [ordered] @{
        name = $item.Name
        path = $item.FullName
        bytes = [long] $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
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

function Get-StrictJson {
    param([string] $Path)

    try {
        return [IO.File]::ReadAllText($Path, $utf8) | ConvertFrom-Json
    }
    catch {
        throw "JSON invalido em '$Path': $($_.Exception.Message)"
    }
}

function Invoke-GitHubApiJson {
    param(
        [string] $Endpoint,
        [ValidateRange(1000, 30000)] [int] $TimeoutMilliseconds
    )

    if ($Endpoint -notmatch
        '^repos/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/git/(?:ref/tags/v[0-9]+\.[0-9]+\.[0-9]+|tags/[0-9a-fA-F]{40})$') {
        throw "Endpoint GitHub inesperado: $Endpoint"
    }

    $gh = Get-Command gh -CommandType Application -ErrorAction Stop |
        Select-Object -First 1
    if (-not $gh) {
        throw 'gh nao encontrado para revalidar a tag remota'
    }

    $token = [Guid]::NewGuid().ToString('N')
    $stdoutPath = Join-Path ([IO.Path]::GetTempPath()) `
        "churchdaw-gh-$token.stdout"
    $stderrPath = Join-Path ([IO.Path]::GetTempPath()) `
        "churchdaw-gh-$token.stderr"
    $process = $null
    try {
        $argumentText = ('api --method GET ' +
            '-H "Accept: application/vnd.github+json" ' +
            '-H "X-GitHub-Api-Version: 2022-11-28" ' +
            '"' + $Endpoint + '"')
        $process = Start-Process -FilePath $gh.Source `
            -ArgumentList $argumentText -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            try { $process.Kill() } catch {}
            if (-not $process.WaitForExit(5000)) {
                throw "GitHub API nao encerrou apos timeout: $Endpoint"
            }
            throw ("GitHub API excedeu $TimeoutMilliseconds ms: " +
                   $Endpoint)
        }
        if ($process.ExitCode -ne 0 -or
            -not (Test-Path -LiteralPath $stdoutPath -PathType Leaf)) {
            throw "GitHub API falhou (exit $($process.ExitCode)): $Endpoint"
        }
        $responseText = [IO.File]::ReadAllText($stdoutPath, $utf8)
        if ([string]::IsNullOrWhiteSpace($responseText)) {
            throw "GitHub API retornou resposta vazia: $Endpoint"
        }
        try { return $responseText | ConvertFrom-Json }
        catch { throw "GitHub API retornou JSON invalido: $Endpoint" }
    }
    finally {
        if ($process) { $process.Dispose() }
        foreach ($path in @($stdoutPath, $stderrPath)) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
                Remove-Item -LiteralPath $path -Force
            }
        }
    }
}

function Assert-RemoteReleaseTag {
    if ([string]::IsNullOrWhiteSpace($env:GH_TOKEN) -or
        $env:GITHUB_REPOSITORY -notmatch `
            '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
        throw 'GH_TOKEN ou GITHUB_REPOSITORY ausente/invalido'
    }

    $tagDeadline = [DateTimeOffset]::UtcNow.AddSeconds(30)
    $expectedRef = "refs/tags/$ReleaseTag"
    $remainingMilliseconds = [int] [Math]::Floor(
        ($tagDeadline - [DateTimeOffset]::UtcNow).TotalMilliseconds)
    if ($remainingMilliseconds -lt 1000) {
        throw 'Orcamento da revalidacao remota da tag esgotado'
    }
    $remote = Invoke-GitHubApiJson `
        "repos/$env:GITHUB_REPOSITORY/git/ref/tags/$ReleaseTag" `
        ([Math]::Min(30000, $remainingMilliseconds))
    Assert-OrdinalEqual ([string] $remote.ref) $expectedRef `
        'referencia remota da tag'
    $remoteType = [string] $remote.object.type
    $remoteSha = [string] $remote.object.sha
    if ($remoteSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw 'Objeto remoto da tag tem SHA invalido'
    }

    $localRef = @(& git rev-parse --verify $expectedRef)
    if ($LASTEXITCODE -ne 0 -or $localRef.Count -ne 1 -or
        [string] $localRef[0] -notmatch '^[0-9a-fA-F]{40}$' -or
        -not $remoteSha.Equals(
            ([string] $localRef[0]).Trim(),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Objeto bruto remoto da tag diverge do checkout'
    }

    $seen = @{}
    for ($depth = 0; $remoteType -ceq 'tag'; ++$depth) {
        if ($depth -ge 8 -or $seen.ContainsKey($remoteSha)) {
            throw 'Cadeia remota de tags invalida ou profunda demais'
        }
        $seen[$remoteSha] = $true
        $remainingMilliseconds = [int] [Math]::Floor(
            ($tagDeadline - [DateTimeOffset]::UtcNow).TotalMilliseconds)
        if ($remainingMilliseconds -lt 1000) {
            throw 'Orcamento da revalidacao remota da tag esgotado'
        }
        $tag = Invoke-GitHubApiJson `
            "repos/$env:GITHUB_REPOSITORY/git/tags/$remoteSha" `
            ([Math]::Min(30000, $remainingMilliseconds))
        $remoteType = [string] $tag.object.type
        $remoteSha = [string] $tag.object.sha
        if ($remoteSha -notmatch '^[0-9a-fA-F]{40}$') {
            throw 'SHA invalido na cadeia remota de tags'
        }
    }
    if ($remoteType -cne 'commit' -or
        -not $remoteSha.Equals(
            $SourceCommit,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Tag remota nao termina no source commit aprovado'
    }
    Add-AuditEvent 'remote-tag' 'confirmed' $remoteSha.ToLowerInvariant()
}

function Assert-PublishTimeBudget {
    $startedText = [string] $env:PUBLISH_JOB_STARTED_AT_UTC
    $started = [DateTimeOffset]::MinValue
    if (-not [DateTimeOffset]::TryParse(
            $startedText,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeUniversal,
            [ref] $started)) {
        throw 'PUBLISH_JOB_STARTED_AT_UTC ausente ou invalido'
    }
    $elapsed = [DateTimeOffset]::UtcNow - $started.ToUniversalTime()
    if ($elapsed.TotalMinutes -lt 0.0 -or $elapsed.TotalMinutes -ge 20.0) {
        throw ('Job consumiu o orcamento seguro antes da primeira mutacao; ' +
               "decorrido=$([Math]::Round($elapsed.TotalMinutes, 1)) min")
    }
    Add-AuditEvent 'post-mutation-budget' 'reserved' `
        "elapsed_minutes=$([Math]::Round($elapsed.TotalMinutes, 1))"
}

function Get-RequiredProperty {
    param($Object, [string] $Name)

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Campo obrigatorio ausente: $Name"
    }
    return $property.Value
}

function Get-PolicyInteger {
    param($Payload, [string] $Name)

    $value = Get-RequiredProperty $Payload $Name
    if (($value -isnot [int] -and $value -isnot [long]) -or
        [long] $value -lt 1 -or [long] $value -gt 1000000000) {
        throw "policy.$Name precisa ser inteiro positivo valido"
    }
    return [int] $value
}

function Get-PolicyInstallerRecord {
    param($Payload, [ValidateSet('windows', 'mac')] [string] $Platform)

    $urlName = "${Platform}_installer_url"
    $shaName = "${Platform}_installer_sha256"
    $bytesName = "${Platform}_installer_bytes"
    $url = [string] (Get-RequiredProperty $Payload $urlName)
    $prefix = "$PublicDesktopBaseUrl/"
    if (-not $url.StartsWith($prefix, [StringComparison]::Ordinal)) {
        throw "policy.$urlName nao aponta para o endpoint oficial exato"
    }
    $name = $url.Substring($prefix.Length)
    Assert-SafeRemoteName $name "policy.$urlName"
    $expectedExtension = if ($Platform -ceq 'windows') { '.exe' } else { '.dmg' }
    if (-not $name.EndsWith($expectedExtension, [StringComparison]::Ordinal)) {
        throw "policy.$urlName tem extensao inesperada"
    }

    $sha256 = [string] (Get-RequiredProperty $Payload $shaName)
    if ($sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "policy.$shaName invalido"
    }
    $bytesValue = Get-RequiredProperty $Payload $bytesName
    if (($bytesValue -isnot [int] -and $bytesValue -isnot [long]) -or
        [long] $bytesValue -lt 1) {
        throw "policy.$bytesName precisa ser inteiro positivo"
    }
    return [ordered] @{
        name = $name
        path = ''
        bytes = [long] $bytesValue
        sha256 = $sha256
        url = $url
    }
}

function Get-FtpsDirectoryUrl {
    param([string] $RemoteDirectory)

    Assert-SafeRemoteDirectory $RemoteDirectory 'diretorio FTPS'
    return "ftp://$($FtpsHost):$FtpsPort$RemoteDirectory/"
}

function Invoke-Curl {
    param(
        [Parameter(Mandatory = $true)][string[]] $Arguments,
        [Parameter(Mandatory = $true)][string] $Label,
        [switch] $AllowFailure
    )

    $output = @(& $script:curl @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        $safeOutput = @($output | ForEach-Object {
            ([string] $_).Replace($NetrcPath, '<netrc>')
        }) -join '; '
        throw "$Label falhou (curl $exitCode): $safeOutput"
    }
    return [pscustomobject] @{
        ExitCode = [int] $exitCode
        Output = $output
    }
}

function Get-FtpsNames {
    param([string] $RemoteDirectory)

    $url = Get-FtpsDirectoryUrl $RemoteDirectory
    $listPath = Join-Path $tempRoot (
        'ftps-list-' + [Guid]::NewGuid().ToString('N') + '.txt')
    $null = Invoke-Curl -Label "listar $RemoteDirectory" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '60',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath,
        '--output', $listPath, '--list-only', $url
    )
    if (-not (Test-Path -LiteralPath $listPath -PathType Leaf)) {
        throw "curl nao criou a listagem FTPS: $RemoteDirectory"
    }
    $names = @([IO.File]::ReadAllLines($listPath) |
        ForEach-Object {
            $line = ([string] $_).Trim()
            if (-not [string]::IsNullOrWhiteSpace($line)) {
                $token = ($line -split '\s+')[-1]
                $clean = ($token -split '[/\\/]')[-1].Trim()
                if ($clean -ne '.' -and $clean -ne '..') {
                    $clean
                }
            }
        } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    return @($names | Sort-Object -Unique)
}

function Test-FtpsFileExists {
    param([string] $RemoteDirectory, [string] $Name)

    Assert-SafeRemoteName $Name 'nome FTPS'
    return @((Get-FtpsNames $RemoteDirectory) |
        Where-Object { $_.Equals($Name, [StringComparison]::OrdinalIgnoreCase) }).Count -ge 1
}

function Upload-FtpsFile {
    param(
        [string] $Path,
        [string] $RemoteDirectory,
        [string] $Name,
        [ValidateRange(30, 900)] [int] $MaxTimeSeconds = 900
    )

    Assert-SafeRemoteName $Name 'nome FTPS para upload'
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Arquivo para upload ausente: $Path"
    }
    $url = (Get-FtpsDirectoryUrl $RemoteDirectory) +
        [Uri]::EscapeDataString($Name)
    $null = Invoke-Curl -Label "upload $Name" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', "$MaxTimeSeconds",
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath,
        '--upload-file', $Path, $url
    )
}

function Download-FtpsFile {
    param(
        [string] $RemoteDirectory,
        [string] $Name,
        [string] $Output,
        [ValidateRange(30, 900)] [int] $MaxTimeSeconds = 900
    )

    Assert-SafeRemoteName $Name 'nome FTPS para download'
    $url = (Get-FtpsDirectoryUrl $RemoteDirectory) +
        [Uri]::EscapeDataString($Name)
    $null = Invoke-Curl -Label "download $Name" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', "$MaxTimeSeconds",
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath,
        '--output', $Output, $url
    )
    if (-not (Test-Path -LiteralPath $Output -PathType Leaf)) {
        throw "curl nao criou o download FTPS: $Output"
    }
}

function Format-FtpsRemotePath {
    param([string] $RemoteDirectory, [string] $Name)

    if ($RemoteDirectory -eq '/') {
        return "/$Name"
    }
    return "$RemoteDirectory/$Name"
}

function Invoke-FtpsQuote {
    param([string] $RemoteDirectory, [string[]] $Commands, [string] $Label)

    $arguments = @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '60',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath
    )
    foreach ($command in $Commands) {
        $arguments += @('--quote', $command)
    }
    $arguments += @('--list-only', (Get-FtpsDirectoryUrl $RemoteDirectory))
    $null = Invoke-Curl -Label $Label -Arguments $arguments
}

function Rename-FtpsFile {
    param([string] $RemoteDirectory, [string] $From, [string] $To)

    Assert-SafeRemoteName $From 'origem do rename FTPS'
    Assert-SafeRemoteName $To 'destino do rename FTPS'
    $fromPath = Format-FtpsRemotePath $RemoteDirectory $From
    $toPath = Format-FtpsRemotePath $RemoteDirectory $To
    Invoke-FtpsQuote $RemoteDirectory @("RNFR $fromPath", "RNTO $toPath") `
        "rename $From -> $To"
}

function Remove-FtpsFile {
    param([string] $RemoteDirectory, [string] $Name)

    Assert-SafeRemoteName $Name 'nome do probe FTPS'
    if (Test-FtpsFileExists $RemoteDirectory $Name) {
        $path = Format-FtpsRemotePath $RemoteDirectory $Name
        Invoke-FtpsQuote $RemoteDirectory @("DELE $path") "limpar probe $Name"
    }
}

function Assert-FileRecord {
    param([string] $Path, $Expected, [string] $Label)

    $actual = Get-FileRecord $Path
    if ([long] $actual.bytes -ne [long] $Expected.bytes -or
        -not ([string] $actual.sha256).Equals(
            [string] $Expected.sha256,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw ("$Label diverge: $($actual.bytes)/$($actual.sha256) != " +
               "$($Expected.bytes)/$($Expected.sha256)")
    }
}

function Assert-FtpsCurrentRecord {
    param(
        [string] $RemoteDirectory,
        [string] $Name,
        $Expected,
        [string] $Label,
        [ValidateRange(30, 900)] [int] $MaxTimeSeconds = 900
    )

    if (-not (Test-FtpsFileExists $RemoteDirectory $Name)) {
        throw "$Label nao existe no FTPS"
    }
    $probe = Join-Path $tempRoot (
        "current-$([Guid]::NewGuid().ToString('N'))-$Name")
    Download-FtpsFile $RemoteDirectory $Name $probe $MaxTimeSeconds
    Assert-FileRecord $probe $Expected $Label
}

function Publish-VersionedFile {
    param([string] $Path, [string] $RemoteDirectory)

    $record = Get-FileRecord $Path
    $name = [string] $record.name
    Assert-SafeRemoteName $name 'arquivo versionado'
    $existing = Join-Path $tempRoot "existing-$name"
    if (Test-FtpsFileExists $RemoteDirectory $name) {
        Download-FtpsFile $RemoteDirectory $name $existing
        Assert-FileRecord $existing $record "arquivo remoto existente $name"
        Add-AuditEvent 'publish-versioned' 'reused-identical' $name
        return $record
    }

    $temporaryName = "$name.tmp.$runToken"
    Assert-SafeRemoteName $temporaryName 'temporario versionado'
    if (Test-FtpsFileExists $RemoteDirectory $temporaryName) {
        throw "Temporario remoto preexistente: $temporaryName"
    }
    Upload-FtpsFile $Path $RemoteDirectory $temporaryName
    try {
        $roundTrip = Join-Path $tempRoot "roundtrip-$name"
        Download-FtpsFile $RemoteDirectory $temporaryName $roundTrip
        Assert-FileRecord $roundTrip $record "round-trip FTPS $name"
        if (Test-FtpsFileExists $RemoteDirectory $name) {
            Download-FtpsFile $RemoteDirectory $name $existing
            Assert-FileRecord $existing $record `
                "arquivo remoto surgido durante upload $name"
            Remove-FtpsFile $RemoteDirectory $temporaryName
            Add-AuditEvent 'publish-versioned' 'reused-raced-identical' $name
            return $record
        }
        Rename-FtpsFile $RemoteDirectory $temporaryName $name
    }
    catch {
        try { Remove-FtpsFile $RemoteDirectory $temporaryName }
        catch { Add-AuditEvent 'temp-cleanup' 'failed' $temporaryName }
        throw
    }
    $final = Join-Path $tempRoot "final-$name"
    Download-FtpsFile $RemoteDirectory $name $final
    Assert-FileRecord $final $record "final FTPS $name"
    $uploadedVersioned.Add($name)
    Add-AuditEvent 'publish-versioned' 'uploaded' $name
    return $record
}

function Replace-FtpsFile {
    param(
        [string] $Path,
        [string] $RemoteDirectory,
        [string] $Name,
        [Parameter(Mandatory = $true)] $ExpectedCurrentRecord,
        [scriptblock] $BeforeRename = $null
    )

    Assert-SafeRemoteName $Name 'arquivo fixo'
    $record = Get-FileRecord $Path
    $temporaryName = "$Name.tmp.$runToken"
    Assert-SafeRemoteName $temporaryName 'temporario fixo'
    if (Test-FtpsFileExists $RemoteDirectory $temporaryName) {
        throw "Temporario remoto preexistente: $temporaryName"
    }
    Upload-FtpsFile $Path $RemoteDirectory $temporaryName 60
    try {
        $roundTrip = Join-Path $tempRoot "roundtrip-fixed-$Name"
        Download-FtpsFile $RemoteDirectory $temporaryName $roundTrip 60
        Assert-FileRecord $roundTrip $record "round-trip FTPS $Name"
        Assert-FtpsCurrentRecord $RemoteDirectory $Name `
            $ExpectedCurrentRecord "estado corrente imediatamente antes de $Name" `
            60
        if ($BeforeRename) {
            & $BeforeRename
        }
        Rename-FtpsFile $RemoteDirectory $temporaryName $Name
    }
    catch {
        try { Remove-FtpsFile $RemoteDirectory $temporaryName }
        catch { Add-AuditEvent 'temp-cleanup' 'failed' $temporaryName }
        throw
    }
    $final = Join-Path $tempRoot "final-fixed-$Name"
    Download-FtpsFile $RemoteDirectory $Name $final 60
    Assert-FileRecord $final $record "final FTPS $Name"
    Add-AuditEvent 'replace-fixed' 'replaced' $Name
    return $record
}

function Download-HttpsExact {
    param(
        [string] $Url,
        [string] $Output,
        $Expected,
        [string] $Label,
        [int] $Attempts = 20,
        [ValidateRange(30, 900)] [int] $MaxTimeSeconds = 900,
        [ValidateRange(1, 30)] [int] $SleepSeconds = 10
    )

    if ($Url -notmatch '^https://churchdaw\.com(?:/|$)') {
        throw "URL publica fora do host permitido: $Url"
    }
    $separator = if ($Url.Contains('?')) { '&' } else { '?' }
    for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
        if (Test-Path -LiteralPath $Output -PathType Leaf) {
            Remove-Item -LiteralPath $Output -Force
        }
        $nonceUrl = "$Url${separator}churchdaw_release_nonce=$runToken-$attempt"
        $result = Invoke-Curl -AllowFailure -Label "HTTPS $Label" -Arguments @(
            '--disable', '--silent', '--show-error', '--fail', '--location',
            '--proto', '=https', '--tlsv1.2',
            '--connect-timeout', '30', '--max-time', "$MaxTimeSeconds",
            '--header', 'Cache-Control: no-cache',
            '--output', $Output, $nonceUrl
        )
        if ($result.ExitCode -eq 0 -and
            (Test-Path -LiteralPath $Output -PathType Leaf)) {
            try {
                Assert-FileRecord $Output $Expected $Label
                $canonicalOutput = "$Output.canonical"
                if (Test-Path -LiteralPath $canonicalOutput -PathType Leaf) {
                    Remove-Item -LiteralPath $canonicalOutput -Force
                }
                $canonicalResult = Invoke-Curl -AllowFailure `
                    -Label "HTTPS canonico $Label" -Arguments @(
                        '--disable', '--silent', '--show-error', '--fail',
                        '--location', '--proto', '=https', '--tlsv1.2',
                        '--connect-timeout', '30', '--max-time',
                        "$MaxTimeSeconds",
                        '--header', 'Cache-Control: no-cache',
                        '--output', $canonicalOutput, $Url
                    )
                if ($canonicalResult.ExitCode -ne 0 -or
                    -not (Test-Path -LiteralPath $canonicalOutput `
                        -PathType Leaf)) {
                    throw "endpoint canonico nao respondeu para $Label"
                }
                Assert-FileRecord $canonicalOutput $Expected `
                    "endpoint canonico $Label"
                Remove-Item -LiteralPath $canonicalOutput -Force
                Add-AuditEvent 'https-verify' 'matched' $Label
                return
            }
            catch {
                if ($attempt -eq $Attempts) { throw }
            }
        }
        if ($attempt -lt $Attempts) {
            Start-Sleep -Seconds $SleepSeconds
        }
    }
    throw "HTTPS nao convergiu para $Label em $Attempts tentativas"
}

function Download-HttpsPostMutationExact {
    param([string] $Url, [string] $Output, $Expected, [string] $Label)

    Download-HttpsExact $Url $Output $Expected $Label `
        -Attempts 3 -MaxTimeSeconds 30 -SleepSeconds 5
}

function Read-VerifiedPolicy {
    param([string] $Path, [string] $Label)

    $envelope = Get-StrictJson $Path
    $names = @($envelope.PSObject.Properties.Name | Sort-Object)
    $expectedNames = @(
        'algorithm', 'key_id', 'payload', 'schema_version', 'signature'
    )
    if (($names -join ',') -cne ($expectedNames -join ',')) {
        throw "$Label tem campos de envelope inesperados"
    }
    if ((Get-RequiredProperty $envelope 'schema_version') -ne 1 -or
        [string] (Get-RequiredProperty $envelope 'algorithm') -cne
            $policyAlgorithm -or
        [string] (Get-RequiredProperty $envelope 'key_id') -cne $policyKeyId) {
        throw "$Label tem metadados de envelope invalidos"
    }
    try {
        $payloadBytes = [Convert]::FromBase64String(
            [string] (Get-RequiredProperty $envelope 'payload'))
        $signatureBytes = [Convert]::FromBase64String(
            [string] (Get-RequiredProperty $envelope 'signature'))
    }
    catch {
        throw "$Label tem Base64 invalido"
    }
    if ($payloadBytes.Length -lt 128 -or $payloadBytes.Length -gt 16384 -or
        $signatureBytes.Length -ne 384) {
        throw "$Label tem payload/assinatura com tamanho invalido"
    }
    $id = [Guid]::NewGuid().ToString('N')
    $payloadPath = Join-Path $tempRoot "$id-payload.json"
    $signaturePath = Join-Path $tempRoot "$id-signature.bin"
    [IO.File]::WriteAllBytes($payloadPath, $payloadBytes)
    [IO.File]::WriteAllBytes($signaturePath, $signatureBytes)
    & $openssl dgst -sha256 -verify $publicKey -signature $signaturePath `
        $payloadPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Assinatura RSA invalida em $Label"
    }
    $payload = Get-StrictJson $payloadPath
    if ($payload.PSObject.Properties.Count -ne 17) {
        throw "$Label tem quantidade inesperada de campos no payload"
    }
    return $payload
}

function Assert-PolicyPayload {
    param(
        $Payload,
        [int] $ExpectedSerial,
        [int] $ExpectedLatest,
        [int] $ExpectedMinimum,
        [string] $ExpectedVersion,
        [string] $ExpectedMode,
        [int] $ExpectedRevokes,
        $WindowsRecord,
        $MacRecord
    )

    $expected = [ordered] @{
        schema_version = 1
        product_id = $productId
        policy_serial = $ExpectedSerial
        mode = $ExpectedMode
        revokes_policy_serial = $ExpectedRevokes
        latest_version_code = $ExpectedLatest
        minimum_version_code = $ExpectedMinimum
        latest_version_name = $ExpectedVersion
        windows_installer_url = if ($WindowsRecord) {
            "$PublicDesktopBaseUrl/$($WindowsRecord.name)"
        } else { '' }
        windows_installer_sha256 = if ($WindowsRecord) {
            $WindowsRecord.sha256
        } else { '' }
        windows_installer_bytes = if ($WindowsRecord) {
            [long] $WindowsRecord.bytes
        } else { [long] 0 }
        mac_installer_url = if ($MacRecord) {
            "$PublicDesktopBaseUrl/$($MacRecord.name)"
        } else { '' }
        mac_installer_sha256 = if ($MacRecord) {
            $MacRecord.sha256
        } else { '' }
        mac_installer_bytes = if ($MacRecord) {
            [long] $MacRecord.bytes
        } else { [long] 0 }
    }
    foreach ($entry in $expected.GetEnumerator()) {
        $actual = Get-RequiredProperty $Payload $entry.Key
        if ($entry.Value -is [string]) {
            Assert-OrdinalEqual $actual $entry.Value "policy.$($entry.Key)"
        }
        elseif ([long] $actual -ne [long] $entry.Value) {
            throw "policy.$($entry.Key) diverge: $actual != $($entry.Value)"
        }
    }
    $issued = [long] (Get-RequiredProperty $Payload 'issued_at')
    $expires = [long] (Get-RequiredProperty $Payload 'expires_at')
    if ($issued -lt 1700000000 -or $expires -le $issued -or
        $expires -gt $issued + 180 * 86400) {
        throw 'policy issued_at/expires_at incoerentes'
    }
    $message = [string] (Get-RequiredProperty $Payload 'update_message')
    if ($message.Length -gt 640 -or $message.IndexOfAny([char[]] "`0`r`n") -ge 0) {
        throw 'policy update_message invalida'
    }
}

function Invoke-FtpsPreflight {
    param([string] $RemoteDirectory)

    $probeA = "churchdaw-probe-a-$runToken.txt"
    $probeB = "churchdaw-probe-b-$runToken.txt"
    $probeFinal = "churchdaw-probe-final-$runToken.txt"
    $probeAPath = Join-Path $tempRoot $probeA
    $probeBPath = Join-Path $tempRoot $probeB
    [IO.File]::WriteAllText($probeAPath, "probe-a-$runToken", $writeUtf8)
    [IO.File]::WriteAllText($probeBPath, "probe-b-$runToken", $writeUtf8)
    $cleanupFailures = [Collections.Generic.List[string]]::new()
    try {
        foreach ($name in @($probeA, $probeB, $probeFinal)) {
            if (Test-FtpsFileExists $RemoteDirectory $name) {
                throw "Probe remoto ja existe: $name"
            }
        }
        Upload-FtpsFile $probeAPath $RemoteDirectory $probeA
        Rename-FtpsFile $RemoteDirectory $probeA $probeFinal
        $first = Join-Path $tempRoot 'probe-first.txt'
        Download-FtpsFile $RemoteDirectory $probeFinal $first
        Assert-FileRecord $first (Get-FileRecord $probeAPath) 'probe rename'

        Upload-FtpsFile $probeBPath $RemoteDirectory $probeB
        Rename-FtpsFile $RemoteDirectory $probeB $probeFinal
        $second = Join-Path $tempRoot 'probe-second.txt'
        Download-FtpsFile $RemoteDirectory $probeFinal $second
        Assert-FileRecord $second (Get-FileRecord $probeBPath) `
            'probe replacement'
        Add-AuditEvent 'ftps-preflight' 'passed' $RemoteDirectory
    }
    finally {
        foreach ($name in @($probeA, $probeB, $probeFinal)) {
            try { Remove-FtpsFile $RemoteDirectory $name }
            catch {
                $cleanupFailures.Add("${name}: $($_.Exception.Message)")
                Add-AuditEvent 'probe-cleanup' 'failed' `
                    "${name}: $($_.Exception.Message)"
            }
        }
    }
    if ($cleanupFailures.Count -gt 0) {
        throw "Falha ao limpar probes FTPS: $($cleanupFailures -join '; ')"
    }
}

function Write-PromotionManifest {
    param([string] $Status, [string] $Failure = '')

    $manifest = [ordered] @{
        schema_version = 1
        product = 'ChurchDAW'
        status = $Status
        failure = $Failure
        release_tag = $ReleaseTag
        release_version = $version
        version_code = $versionCode
        source_commit = $SourceCommit.ToLowerInvariant()
        github = [ordered] @{
            publisher_run_id = $env:GITHUB_RUN_ID
            publisher_run_attempt = $env:GITHUB_RUN_ATTEMPT
            windows = [ordered] @{
                run_id = $WindowsRunId
                artifact_id = $WindowsArtifactId
                artifact_digest = $WindowsArtifactDigest.ToLowerInvariant()
                artifact_archive_bytes = $WindowsArtifactArchiveBytes
            }
            macos = [ordered] @{
                run_id = $MacRunId
                artifact_id = $MacArtifactId
                artifact_digest = $MacArtifactDigest.ToLowerInvariant()
                artifact_archive_bytes = $MacArtifactArchiveBytes
            }
        }
        inputs = [ordered] @{
            expected_previous_policy_sha256 =
                $ExpectedPreviousPolicySha256.ToLowerInvariant()
            policy_serial = $PolicySerial
            minimum_version_code = $MinimumVersionCode
            rollback_policy_serial = $derivedRollbackPolicySerial
            rollback_version_code = $derivedRollbackVersionCode
            rollback_minimum_version_code = $derivedRollbackMinimumVersionCode
            rollback_version_name = $derivedRollbackVersionName
        }
        candidates = [ordered] @{
            windows = $windowsRecord
            macos = $macRecord
            windows_manifest_sha256 =
                $VerifiedWindowsManifestSha256.ToLowerInvariant()
            macos_manifest_sha256 =
                $VerifiedMacManifestSha256.ToLowerInvariant()
        }
        policy = [ordered] @{
            previous_sha256 = if ($previousPolicyRecord) {
                $previousPolicyRecord.sha256
            } else { '' }
            release_sha256 = if ($releasePolicyRecord) {
                $releasePolicyRecord.sha256
            } else { '' }
            rollback_sha256 = if ($rollbackPolicyRecord) {
                $rollbackPolicyRecord.sha256
            } else { '' }
            promotion_attempted = $policyPromotionAttempted
            rollback_confirmed = $rollbackConfirmed
        }
        site = [ordered] @{
            index_sha256 = if ($siteRecord) { $siteRecord.sha256 } else { '' }
            promotion_attempted = $sitePromotionAttempted
        }
        uploaded_versioned = @($uploadedVersioned)
        audit_events = @($auditEvents)
        generated_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    $path = Join-Path $AuditOutputDirectory 'desktop-promotion-manifest.json'
    [IO.File]::WriteAllText(
        $path,
        ($manifest | ConvertTo-Json -Depth 12) + [Environment]::NewLine,
        $writeUtf8)
}

foreach ($requiredFile in @($openssl, $publicKey, $policyScript, $PrivateKey,
                             $NetrcPath, $SiteIndex)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $requiredFile"
    }
}
$script:curl = (Get-Command curl.exe -ErrorAction Stop).Source
$null = Get-Command gh.exe -ErrorAction Stop
Assert-SafeRemoteDirectory $RemoteRootDirectory 'RemoteRootDirectory'
Assert-SafeRemoteDirectory $RemoteDesktopDirectory 'RemoteDesktopDirectory'
if (-not $RemoteDesktopDirectory.StartsWith(
        $RemoteRootDirectory + '/',
        [StringComparison]::Ordinal)) {
    throw 'RemoteDesktopDirectory precisa ficar dentro de RemoteRootDirectory'
}
if ($PublicSiteUrl -cne 'https://churchdaw.com/' -or
    $PublicDesktopBaseUrl -cne 'https://churchdaw.com/app/desktop') {
    throw 'URLs publicas precisam ser os endpoints oficiais exatos'
}
if ($UpdateMessage.IndexOfAny([char[]] "`0`r`n") -ge 0) {
    throw 'UpdateMessage nao pode conter NUL nem quebra de linha'
}
$version = $ReleaseTag.Substring(1)
$versionCode = Get-VersionCode $version
if ($versionCode -lt $MinimumVersionCode) {
    throw 'Versao da release ficou abaixo do minimum_version_code'
}
$runToken = "${env:GITHUB_RUN_ID}-${env:GITHUB_RUN_ATTEMPT}"
if ($runToken -notmatch '^[1-9][0-9]*-[1-9][0-9]*$') {
    throw 'GITHUB_RUN_ID/GITHUB_RUN_ATTEMPT invalidos'
}

$auditRoot = [IO.Path]::GetFullPath($AuditOutputDirectory)
New-Item -ItemType Directory -Path $auditRoot -Force | Out-Null
$tempBase = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
$tempRoot = Join-Path $env:RUNNER_TEMP "churchdaw-desktop-publish-$runToken"
if (Test-Path -LiteralPath $tempRoot) {
    throw "Diretorio temporario ja existe: $tempRoot"
}
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$resolvedTemp = [IO.Path]::GetFullPath($tempRoot).TrimEnd('\') + '\'
if (-not $resolvedTemp.StartsWith(
        $tempBase,
        [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($tempRoot) -cne
        "churchdaw-desktop-publish-$runToken") {
    throw 'Diretorio temporario nao passou na prova de posse'
}

$windowsRecord = $null
$macRecord = $null
$previousPolicyRecord = $null
$releasePolicyRecord = $null
$rollbackPolicyRecord = $null
$siteRecord = $null
$siteBackupRecord = $null
$derivedRollbackPolicySerial = $PolicySerial + 1
$derivedRollbackVersionCode = $null
$derivedRollbackVersionName = ''
$derivedRollbackMinimumVersionCode = $null
$previousWindowsRecord = $null
$previousMacRecord = $null
$siteBackup = Join-Path $tempRoot 'site-index-before.html'
$policyBackup = Join-Path $tempRoot 'policy-before.json'
$releasePolicy = Join-Path $tempRoot 'update-policy-v1.release.json'
$rollbackPolicy = Join-Path $tempRoot 'update-policy-v1.rollback.json'

try {
    Assert-RemoteReleaseTag
    $windowsInstaller = Resolve-UniqueFile $WindowsArtifactRoot `
        "ChurchDAW-$version-Setup.exe"
    $macInstaller = Resolve-UniqueFile $MacArtifactRoot `
        "ChurchDAW-$version-macOS.dmg"
    $windowsManifest = Resolve-UniqueFile $WindowsArtifactRoot `
        'release-manifest.json'
    $macManifest = Resolve-UniqueFile $MacArtifactRoot 'release-manifest.json'
    $windowsRecord = Get-FileRecord $windowsInstaller
    $macRecord = Get-FileRecord $macInstaller

    Assert-OrdinalEqual $windowsRecord.sha256 `
        $VerifiedWindowsSha256.ToLowerInvariant() 'SHA Windows verificado'
    Assert-OrdinalEqual $macRecord.sha256 `
        $VerifiedMacSha256.ToLowerInvariant() 'SHA macOS verificado'
    if ([long] $windowsRecord.bytes -ne $VerifiedWindowsBytes -or
        [long] $macRecord.bytes -ne $VerifiedMacBytes) {
        throw 'Bytes dos candidatos divergem dos jobs verificadores'
    }
    Assert-OrdinalEqual ((Get-FileHash -LiteralPath $windowsManifest `
        -Algorithm SHA256).Hash.ToLowerInvariant()) `
        $VerifiedWindowsManifestSha256.ToLowerInvariant() `
        'manifest Windows verificado'
    Assert-OrdinalEqual ((Get-FileHash -LiteralPath $macManifest `
        -Algorithm SHA256).Hash.ToLowerInvariant()) `
        $VerifiedMacManifestSha256.ToLowerInvariant() `
        'manifest macOS verificado'

    $signatureLog = Join-Path $auditRoot 'windows-authenticode-publisher.log'
    $signature = Assert-ChurchDAWAuthenticodeSignature `
        -Path $windowsInstaller `
        -ExpectedPublisher $ExpectedWindowsPublisher `
        -ExpectedSignerThumbprints @($ExpectedWindowsThumbprint) `
        -RequireTimestamp $true -RequirePublicChain $true `
        -VerificationLogPath $signatureLog
    Add-AuditEvent 'windows-authenticode' 'passed' `
        $signature.SignerThumbprint

    $siteText = [IO.File]::ReadAllText($SiteIndex, $utf8)
    $androidUrls = @([regex]::Matches(
        $siteText,
        'https://churchdaw\.com/app/android/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+-signed\.apk') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
    if ($androidUrls.Count -ne 1) {
        throw 'Site precisa conter exatamente um download Android oficial'
    }
    $androidDownloadUrl = [string] $androidUrls[0]
    if (-not [string]::IsNullOrWhiteSpace($ExpectedAndroidDownloadUrl) -and
        $androidDownloadUrl -cne $ExpectedAndroidDownloadUrl) {
        throw 'Download Android do site diverge do pin solicitado'
    }
    $androidHead = Invoke-Curl -AllowFailure -Label 'HEAD Android do site' `
        -Arguments @(
            '--disable', '--silent', '--show-error', '--fail', '--location',
            '--head', '--proto', '=https', '--tlsv1.2',
            '--connect-timeout', '30', '--max-time', '60',
            '--header', 'Cache-Control: no-cache', $androidDownloadUrl)
    if ($androidHead.ExitCode -ne 0) {
        throw "Download Android do site esta indisponivel: $androidDownloadUrl"
    }
    foreach ($required in @(
        "$PublicDesktopBaseUrl/$($windowsRecord.name)",
        "$PublicDesktopBaseUrl/$($macRecord.name)",
        $androidDownloadUrl
    )) {
        if (-not $siteText.Contains($required)) {
            throw "Site nao contem URL obrigatoria: $required"
        }
    }
    foreach ($forbidden in @('ChurchDAW-1.4.0-', 'ChurchDAW-1.5.4.apk')) {
        if ($siteText.Contains($forbidden)) {
            throw "Site ainda contem referencia antiga: $forbidden"
        }
    }
    $siteRecord = Get-FileRecord $SiteIndex

    Invoke-FtpsPreflight $RemoteRootDirectory
    Invoke-FtpsPreflight $RemoteDesktopDirectory

    if (-not (Test-FtpsFileExists $RemoteDesktopDirectory `
            'update-policy-v1.json')) {
        $seedPath = 'Packaging/DesktopHosting/app/desktop/update-policy-v1.json'
        if ($seedPath -and (Test-Path -LiteralPath $seedPath -PathType Leaf)) {
            $seedRecord = Get-FileRecord $seedPath
            if ($seedRecord.sha256 -eq $ExpectedPreviousPolicySha256.ToLowerInvariant()) {
                Upload-FtpsFile $seedPath $RemoteDesktopDirectory 'update-policy-v1.json'
            }
        }
    }
    if (-not (Test-FtpsFileExists $RemoteDesktopDirectory `
            'update-policy-v1.json')) {
        throw 'Politica desktop anterior nao existe no FTPS'
    }
    Download-FtpsFile $RemoteDesktopDirectory 'update-policy-v1.json' `
        $policyBackup
    $previousPolicyRecord = Get-FileRecord $policyBackup
    Assert-OrdinalEqual $previousPolicyRecord.sha256 `
        $ExpectedPreviousPolicySha256.ToLowerInvariant() `
        'hash otimista da policy anterior'

    $previousHttps = Join-Path $tempRoot 'policy-before-https.json'
    Download-HttpsExact `
        "$PublicDesktopBaseUrl/update-policy-v1.json" `
        $previousHttps $previousPolicyRecord 'policy anterior HTTPS'
    $previousPayload = Read-VerifiedPolicy $policyBackup 'policy anterior'
    $previousSerial = Get-PolicyInteger $previousPayload 'policy_serial'
    $previousLatestVersionCode = Get-PolicyInteger $previousPayload `
        'latest_version_code'
    $previousMinimumVersionCode = Get-PolicyInteger $previousPayload `
        'minimum_version_code'
    $previousVersionName = [string] (Get-RequiredProperty $previousPayload `
        'latest_version_name')
    $previousMode = [string] (Get-RequiredProperty $previousPayload 'mode')
    $previousRevokesValue = Get-RequiredProperty $previousPayload `
        'revokes_policy_serial'
    if (($previousRevokesValue -isnot [int] -and
         $previousRevokesValue -isnot [long]) -or
        [long] $previousRevokesValue -lt 0 -or
        [long] $previousRevokesValue -gt 1000000000) {
        throw 'policy.revokes_policy_serial precisa ser inteiro nao negativo'
    }
    $previousRevokes = [int] $previousRevokesValue
    if ($previousSerial -ne ($PolicySerial - 1)) {
        throw 'PolicySerial nao e exatamente anterior + 1'
    }
    if ($previousVersionName -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
        (Get-VersionCode $previousVersionName) -ne $previousLatestVersionCode) {
        throw 'Versao nominal da policy anterior nao corresponde ao codigo'
    }
    if ($previousLatestVersionCode -ge $versionCode) {
        throw 'Release nao aumenta latest_version_code da policy publica'
    }
    if ($previousMinimumVersionCode -gt $MinimumVersionCode) {
        throw 'Release tentaria reduzir minimum_version_code sem rollback'
    }
    if ($previousMode -ceq 'normal') {
        if ($previousRevokes -ne 0) {
            throw 'Policy normal anterior nao pode revogar outro serial'
        }
    }
    elseif ($previousMode -ceq 'rollback') {
        if ($previousRevokes -ne ($previousSerial - 1)) {
            throw 'Policy rollback anterior nao revoga o serial imediatamente anterior'
        }
    }
    else {
        throw "Modo inesperado na policy anterior: $previousMode"
    }

    $previousWindowsUrl = [string] (Get-RequiredProperty $previousPayload `
        'windows_installer_url')
    $previousMacUrl = [string] (Get-RequiredProperty $previousPayload `
        'mac_installer_url')
    if (-not [string]::IsNullOrWhiteSpace($previousWindowsUrl) -or
        -not [string]::IsNullOrWhiteSpace($previousMacUrl)) {
        if ([string]::IsNullOrWhiteSpace($previousWindowsUrl) -or
            [string]::IsNullOrWhiteSpace($previousMacUrl)) {
            throw 'Policy anterior tem conjunto parcial de instaladores'
        }
        $previousWindowsRecord = Get-PolicyInstallerRecord `
            $previousPayload 'windows'
        $previousMacRecord = Get-PolicyInstallerRecord $previousPayload 'mac'
    }
    Assert-PolicyPayload $previousPayload $previousSerial `
        $previousLatestVersionCode $previousMinimumVersionCode `
        $previousVersionName $previousMode $previousRevokes `
        $previousWindowsRecord $previousMacRecord

    $derivedRollbackVersionCode = $previousLatestVersionCode
    $derivedRollbackVersionName = $previousVersionName
    $derivedRollbackMinimumVersionCode = $previousMinimumVersionCode
    Add-AuditEvent 'previous-policy' 'verified' `
        "serial=$previousSerial mode=$previousMode sha=$($previousPolicyRecord.sha256)"

    $null = Publish-VersionedFile $windowsInstaller $RemoteDesktopDirectory
    $null = Publish-VersionedFile $macInstaller $RemoteDesktopDirectory

    $windowsPublic = Join-Path $tempRoot $windowsRecord.name
    $macPublic = Join-Path $tempRoot $macRecord.name
    Download-HttpsExact "$PublicDesktopBaseUrl/$($windowsRecord.name)" `
        $windowsPublic $windowsRecord 'Windows publico'
    Download-HttpsExact "$PublicDesktopBaseUrl/$($macRecord.name)" `
        $macPublic $macRecord 'macOS publico'

    if (-not (Test-FtpsFileExists $RemoteRootDirectory 'index.html')) {
        $seedPath = 'Packaging/SiteProduction/index.html'
        if ($seedPath -and (Test-Path -LiteralPath $seedPath -PathType Leaf)) {
            $seedRecord = Get-FileRecord $seedPath
            if ($seedRecord.sha256 -eq $ExpectedPreviousSiteSha256.ToLowerInvariant()) {
                Upload-FtpsFile $seedPath $RemoteRootDirectory 'index.html'
            }
        }
    }
    if (-not (Test-FtpsFileExists $RemoteRootDirectory 'index.html')) {
        throw 'index.html atual nao existe no FTPS'
    }
    Download-FtpsFile $RemoteRootDirectory 'index.html' $siteBackup
    $siteBackupRecord = Get-FileRecord $siteBackup
    $siteBackupHttps = Join-Path $tempRoot 'site-index-before-https.html'
    Download-HttpsExact $PublicSiteUrl $siteBackupHttps $siteBackupRecord `
        'site anterior HTTPS'

    $policyArguments = @{
        PolicySerial = $PolicySerial
        LatestVersionCode = $versionCode
        MinimumVersionCode = $MinimumVersionCode
        LatestVersionName = $version
        UpdateMessage = $UpdateMessage
        WindowsInstaller = $windowsPublic
        WindowsInstallerUrl =
            "$PublicDesktopBaseUrl/$($windowsRecord.name)"
        MacInstaller = $macPublic
        MacInstallerUrl = "$PublicDesktopBaseUrl/$($macRecord.name)"
        ReleaseMode = 'Commercial'
        ExpectedWindowsPublisher = $ExpectedWindowsPublisher
        ExpectedWindowsSignerThumbprints = @($ExpectedWindowsThumbprint)
        WindowsVerificationLogPath =
            (Join-Path $auditRoot 'windows-authenticode-policy.log')
        PrivateKey = $PrivateKey
        PreviousPolicy = $policyBackup
        Output = $releasePolicy
    }
    $null = & $policyScript @policyArguments
    $releasePolicyRecord = Get-FileRecord $releasePolicy
    $releasePayload = Read-VerifiedPolicy $releasePolicy 'policy da release'
    Assert-PolicyPayload $releasePayload $PolicySerial $versionCode `
        $MinimumVersionCode $version 'normal' 0 $windowsRecord $macRecord

    $rollbackArguments = @{
        PolicySerial = $derivedRollbackPolicySerial
        LatestVersionCode = $derivedRollbackVersionCode
        MinimumVersionCode = $derivedRollbackMinimumVersionCode
        LatestVersionName = $derivedRollbackVersionName
        UpdateMessage = 'Atualizacao revogada por seguranca; mantenha a versao estavel instalada.'
        ReleaseMode = 'Commercial'
        Mode = 'rollback'
        RevokesPolicySerial = $PolicySerial
        AllowRollbackPolicyWithoutInstaller = $true
        PrivateKey = $PrivateKey
        PreviousPolicy = $releasePolicy
        Output = $rollbackPolicy
    }
    $null = & $policyScript @rollbackArguments
    $rollbackPolicyRecord = Get-FileRecord $rollbackPolicy
    $rollbackPayload = Read-VerifiedPolicy $rollbackPolicy 'policy de rollback'
    Assert-PolicyPayload $rollbackPayload $derivedRollbackPolicySerial `
        $derivedRollbackVersionCode $derivedRollbackMinimumVersionCode `
        $derivedRollbackVersionName 'rollback' $PolicySerial $null $null
    Add-AuditEvent 'compose-policy' 'passed' `
        "release=$($releasePolicyRecord.sha256); rollback=$($rollbackPolicyRecord.sha256)"

    # Fechar todos os checks potencialmente longos antes da primeira mutacao.
    # Se qualquer endpoint ou estado remoto nao convergir, o job termina sem
    # tocar no site nem na policy publica.
    Assert-RemoteReleaseTag
    Assert-FtpsCurrentRecord $RemoteDesktopDirectory `
        'update-policy-v1.json' $previousPolicyRecord `
        'policy anterior no gate pre-mutacao' 60
    $previousHttpsFinal = Join-Path $tempRoot `
        'policy-before-https-final.json'
    Download-HttpsExact `
        "$PublicDesktopBaseUrl/update-policy-v1.json" `
        $previousHttpsFinal $previousPolicyRecord `
        'policy anterior HTTPS no gate pre-mutacao'
    Download-HttpsExact `
        "$PublicDesktopBaseUrl/$($windowsRecord.name)" `
        $windowsPublic $windowsRecord 'Windows publico no gate pre-mutacao'
    Download-HttpsExact `
        "$PublicDesktopBaseUrl/$($macRecord.name)" `
        $macPublic $macRecord 'macOS publico no gate pre-mutacao'
    Assert-FtpsCurrentRecord $RemoteRootDirectory 'index.html' `
        $siteBackupRecord 'site anterior no gate pre-mutacao' 60
    Download-HttpsExact $PublicSiteUrl $siteBackupHttps $siteBackupRecord `
        'site anterior HTTPS no gate pre-mutacao'

    # O owner do repositorio ainda possui bypass operacional no ruleset. A
    # tag e portanto revalidada depois dos uploads inertes e imediatamente
    # antes da primeira mudanca publica visivel (o index e, depois, a policy).
    Assert-PublishTimeBudget
    Assert-RemoteReleaseTag
    $sitePromotionAttempted = $true
    $null = Replace-FtpsFile $SiteIndex $RemoteRootDirectory 'index.html' `
        -ExpectedCurrentRecord $siteBackupRecord `
        -BeforeRename { Assert-RemoteReleaseTag }
    $siteHttps = Join-Path $tempRoot 'site-index-https.html'
    Download-HttpsPostMutationExact $PublicSiteUrl $siteHttps $siteRecord `
        'site index'

    # Fechar a janela TOCTOU imediatamente antes da policy, que e a ultima
    # mudanca e o gatilho efetivo do auto-update compartilhado.
    Assert-RemoteReleaseTag
    Assert-FtpsCurrentRecord $RemoteDesktopDirectory `
        'update-policy-v1.json' $previousPolicyRecord `
        'policy anterior imediatamente antes da promocao' 60
    Download-HttpsPostMutationExact `
        "$PublicDesktopBaseUrl/update-policy-v1.json" `
        $previousHttpsFinal $previousPolicyRecord `
        'policy anterior HTTPS imediatamente antes da promocao'
    Assert-FtpsCurrentRecord $RemoteRootDirectory 'index.html' $siteRecord `
        'site novo imediatamente antes da policy' 60
    Download-HttpsPostMutationExact $PublicSiteUrl $siteHttps $siteRecord `
        'site novo HTTPS imediatamente antes da policy'

    Assert-RemoteReleaseTag
    $policyPromotionAttempted = $true
    $null = Replace-FtpsFile $releasePolicy $RemoteDesktopDirectory `
        'update-policy-v1.json' -ExpectedCurrentRecord $previousPolicyRecord `
        -BeforeRename { Assert-RemoteReleaseTag }
    $policyFtpsAfter = Join-Path $tempRoot 'policy-after-ftps.json'
    Download-FtpsFile $RemoteDesktopDirectory 'update-policy-v1.json' `
        $policyFtpsAfter 60
    Assert-FileRecord $policyFtpsAfter $releasePolicyRecord 'policy FTPS final'
    $policyHttpsAfter = Join-Path $tempRoot 'policy-after-https.json'
    Download-HttpsPostMutationExact `
        "$PublicDesktopBaseUrl/update-policy-v1.json" `
        $policyHttpsAfter $releasePolicyRecord 'policy HTTPS final'
    $finalPayload = Read-VerifiedPolicy $policyHttpsAfter 'policy publica final'
    Assert-PolicyPayload $finalPayload $PolicySerial $versionCode `
        $MinimumVersionCode $version 'normal' 0 $windowsRecord $macRecord
    Assert-RemoteReleaseTag
    Add-AuditEvent 'policy-last' 'confirmed' `
        "serial=$PolicySerial sha=$($releasePolicyRecord.sha256)"

    Write-PromotionManifest 'published'
    Write-Host 'PUBLISH_DESKTOP_RELEASE_OK'
    Write-Host "Windows: $($windowsRecord.name) $($windowsRecord.sha256)"
    Write-Host "macOS: $($macRecord.name) $($macRecord.sha256)"
    Write-Host "Policy serial: $PolicySerial"
}
catch {
    $failure = $_.Exception.Message
    Add-AuditEvent 'publisher' 'failed' $failure

    if ($policyPromotionAttempted -and $rollbackPolicyRecord) {
        try {
            if (-not (Test-FtpsFileExists $RemoteDesktopDirectory `
                    'update-policy-v1.json')) {
                throw 'Policy ativa sumiu; recusando sobrescrever estado desconhecido'
            }
            $activePolicyPath = Join-Path $tempRoot `
                'policy-active-before-rollback.json'
            Download-FtpsFile $RemoteDesktopDirectory 'update-policy-v1.json' `
                $activePolicyPath 60
            $activePolicyRecord = Get-FileRecord $activePolicyPath
            $knownPolicyHashes = @(
                $previousPolicyRecord.sha256,
                $releasePolicyRecord.sha256,
                $rollbackPolicyRecord.sha256
            )
            if ($activePolicyRecord.sha256 -notin $knownPolicyHashes) {
                throw ('Policy ativa mudou fora deste workflow; recusando ' +
                       "clobber: $($activePolicyRecord.sha256)")
            }
            if ($activePolicyRecord.sha256 -ne $rollbackPolicyRecord.sha256) {
                # Mesmo que a serial 7 nao tenha ficado ativa no origin, ela
                # pode ter sido observada por um cliente durante a tentativa.
                # Publicar a serial 8 e seguro tanto sobre 6 quanto sobre 7;
                # restaurar 6 seria replay para quem ja aceitou 7.
                $null = Replace-FtpsFile $rollbackPolicy `
                    $RemoteDesktopDirectory 'update-policy-v1.json' `
                    -ExpectedCurrentRecord $activePolicyRecord
            }
            $rollbackHttps = Join-Path $tempRoot 'rollback-policy-https.json'
            Download-HttpsPostMutationExact `
                "$PublicDesktopBaseUrl/update-policy-v1.json" `
                $rollbackHttps $rollbackPolicyRecord 'rollback policy HTTPS'
            $activeRollback = Read-VerifiedPolicy $rollbackHttps `
                'rollback publico final'
            Assert-PolicyPayload $activeRollback $derivedRollbackPolicySerial `
                $derivedRollbackVersionCode `
                $derivedRollbackMinimumVersionCode `
                $derivedRollbackVersionName 'rollback' $PolicySerial `
                $null $null
            $rollbackConfirmed = $true
            Add-AuditEvent 'rollback' 'confirmed' `
                "serial=$derivedRollbackPolicySerial"
        }
        catch {
            Add-AuditEvent 'rollback' 'critical-failure' $_.Exception.Message
            $failure += '; ROLLBACK NAO CONFIRMADO: ' + $_.Exception.Message
        }
    }
    $siteRollbackAllowed = (-not $policyPromotionAttempted) -or
        $rollbackConfirmed
    if ($sitePromotionAttempted -and $siteRollbackAllowed -and
        (Test-Path -LiteralPath $siteBackup -PathType Leaf)) {
        try {
            if (-not (Test-FtpsFileExists $RemoteRootDirectory 'index.html')) {
                throw 'index.html ativo sumiu; estado remoto desconhecido'
            }
            $activeSitePath = Join-Path $tempRoot `
                'site-active-before-rollback.html'
            Download-FtpsFile $RemoteRootDirectory 'index.html' $activeSitePath 60
            $activeSiteRecord = Get-FileRecord $activeSitePath
            $siteBackupRecord = Get-FileRecord $siteBackup
            if ($activeSiteRecord.sha256 -eq $siteRecord.sha256) {
                $null = Replace-FtpsFile $siteBackup $RemoteRootDirectory `
                    'index.html' -ExpectedCurrentRecord $activeSiteRecord
                Add-AuditEvent 'site-rollback' 'confirmed' `
                    'index.html anterior'
            }
            elseif ($activeSiteRecord.sha256 -eq $siteBackupRecord.sha256) {
                Add-AuditEvent 'site-rollback' 'already-previous' `
                    'index.html anterior'
            }
            else {
                throw ('index.html mudou fora deste workflow; recusando ' +
                       "clobber: $($activeSiteRecord.sha256)")
            }
            $siteRollbackHttps = Join-Path $tempRoot `
                'site-index-rollback-https.html'
            Download-HttpsPostMutationExact $PublicSiteUrl `
                $siteRollbackHttps $siteBackupRecord `
                'site anterior restaurado HTTPS'
        }
        catch {
            Add-AuditEvent 'site-rollback' 'failed' $_.Exception.Message
            $failure += '; SITE ROLLBACK NAO CONFIRMADO: ' +
                $_.Exception.Message
        }
    }
    elseif ($sitePromotionAttempted -and -not $siteRollbackAllowed) {
        Add-AuditEvent 'site-rollback' 'skipped-unknown-policy-state' `
            'policy rollback nao foi confirmado; recusando clobber remoto'
        $failure += '; SITE ROLLBACK OMITIDO POR ESTADO DE POLICY DESCONHECIDO'
    }

    try { Write-PromotionManifest 'failed' $failure }
    catch { Write-Warning "Falha ao escrever manifest de auditoria: $_" }
    throw $failure
}
finally {
    if (Test-Path -LiteralPath $tempRoot -PathType Container) {
        $candidate = [IO.Path]::GetFullPath($tempRoot).TrimEnd('\') + '\'
        if (-not $candidate.Equals(
                $resolvedTemp,
                [StringComparison]::OrdinalIgnoreCase) -or
            -not $candidate.StartsWith(
                $tempBase,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Recusando limpar diretorio temporario sem prova de posse'
        }
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
