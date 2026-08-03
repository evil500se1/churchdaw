[CmdletBinding(DefaultParameterSetName = 'Publish')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Functions')]
    [switch] $LoadFunctionsOnly,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string] $AndroidArtifactRoot,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^v[0-9]+\.[0-9]+\.[0-9]+$')]
    [string] $ReleaseTag,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string] $SourceCommit,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $AndroidRunId,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $AndroidArtifactId,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^sha256:[0-9a-fA-F]{64}$')]
    [string] $AndroidArtifactDigest,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $AndroidArtifactArchiveBytes,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedPreviousPolicySha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedPreviousManifestSha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedPreviousLegacyPolicySha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedPreviousSiteSha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateRange(1, 999999999)]
    [int] $PolicySerial,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateRange(1, 1000000000)]
    [int] $MinimumVersionCode,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateLength(1, 640)]
    [string] $UpdateMessage,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string] $PrivateKey,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedSigningCertificateSha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[A-Za-z0-9.-]+$')]
    [string] $FtpsHost,

    [Parameter(ParameterSetName = 'Publish')]
    [ValidateRange(1, 65535)]
    [int] $FtpsPort = 21,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string] $NetrcPath,

    [Parameter(ParameterSetName = 'Publish')]
    [string] $RemoteRootDirectory = '/public_html',

    [Parameter(ParameterSetName = 'Publish')]
    [string] $RemoteAndroidDirectory = '/public_html/app/android',

    [Parameter(ParameterSetName = 'Publish')]
    [string] $PublicSiteUrl = 'https://churchdaw.com/',

    [Parameter(ParameterSetName = 'Publish')]
    [string] $PublicAndroidBaseUrl = 'https://churchdaw.com/app/android',

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string] $SiteIndex,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [string] $AuditOutputDirectory,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $VerifiedApkSha256,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidateRange(1, [long]::MaxValue)]
    [long] $VerifiedApkBytes,

    [Parameter(Mandatory = $true, ParameterSetName = 'Publish')]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $VerifiedMetadataSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$script:policyPromotionAttempted = $false
$script:rollbackConfirmed = $false
$script:sitePromotionAttempted = $false
$script:manifestPromotionAttempted = $false
$script:legacyPromotionAttempted = $false
$script:auditEvents = [Collections.Generic.List[object]]::new()
$script:uploadedVersioned = [Collections.Generic.List[string]]::new()
$script:utf8 = [Text.UTF8Encoding]::new($false, $true)
$script:writeUtf8 = [Text.UTF8Encoding]::new($false)
$script:packageId = 'com.churchdaw.app'
$script:policyKeyId = 'churchdaw-update-2026-01'
$script:policyAlgorithm = 'SHA256withRSA'

function Add-AuditEvent {
    param([string] $Action, [string] $Result, [string] $Detail = '')

    $script:auditEvents.Add([ordered] @{
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
        $Value.Contains('..') -or
        [IO.Path]::GetFileName($Value) -cne $Value) {
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

function Get-StrictJson {
    param([string] $Path)

    try {
        return [IO.File]::ReadAllText($Path, $script:utf8) |
            ConvertFrom-Json
    }
    catch {
        throw "JSON invalido em '$Path': $($_.Exception.Message)"
    }
}

function Get-RequiredProperty {
    param([object] $Object, [string] $Name)

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Campo obrigatorio ausente: $Name"
    }
    return $property.Value
}

function Get-RequiredPositiveInteger {
    param([object] $Object, [string] $Name)

    $value = Get-RequiredProperty $Object $Name
    if (($value -isnot [int] -and $value -isnot [long]) -or
        [long] $value -lt 1 -or [long] $value -gt 1000000000) {
        throw "$Name precisa ser inteiro positivo valido"
    }
    return [int] $value
}

function Get-RequiredNonNegativeInteger {
    param([object] $Object, [string] $Name)

    $value = Get-RequiredProperty $Object $Name
    if (($value -isnot [int] -and $value -isnot [long]) -or
        [long] $value -lt 0 -or [long] $value -gt 1000000000) {
        throw "$Name precisa ser inteiro nao negativo valido"
    }
    return [int] $value
}

function Get-AndroidRecoveryPolicyPlan {
    param(
        [ValidateRange(1, 1000000000)] [int] $ReleaseMinimum,
        [ValidateRange(1, 1000000000)] [int] $PreviousMinimum
    )

    if ($ReleaseMinimum -ne $PreviousMinimum) {
        throw ('Promocao transacional exige preservar minimum_version_code; ' +
               'elevar o piso precisa de gate posterior separado')
    }
    return [pscustomobject] @{
        Mode = 'normal'
        MinimumVersionCode = $PreviousMinimum
        RevokesPolicySerial = 0
    }
}

function New-AndroidCandidateSite {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
        [string] $PreviousVersion,
        [Parameter(Mandatory = $true)]
        [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
        [string] $ReleaseVersion,
        [Parameter(Mandatory = $true)] [string] $PreviousApkUrl,
        [Parameter(Mandatory = $true)] [string] $ReleaseApkUrl
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "Site anterior ausente: $Source"
    }
    if (Test-Path -LiteralPath $Destination) {
        throw "Site candidato preexistente: $Destination"
    }
    if ($PreviousVersion -ceq $ReleaseVersion) {
        throw 'Versao Android anterior e candidata precisam ser diferentes'
    }

    $expectedPreviousUrl =
        "https://churchdaw.com/app/android/ChurchDAW-$PreviousVersion-signed.apk"
    $expectedReleaseUrl =
        "https://churchdaw.com/app/android/ChurchDAW-$ReleaseVersion-signed.apk"
    Assert-OrdinalEqual $PreviousApkUrl $expectedPreviousUrl `
        'URL Android anterior/site'
    Assert-OrdinalEqual $ReleaseApkUrl $expectedReleaseUrl `
        'URL Android candidata/site'

    $sourceText = [IO.File]::ReadAllText($Source, $script:utf8)
    $candidateText = $sourceText
    $replacements = @(
        [pscustomobject] @{
            Label = 'URLs Android'
            Old = $PreviousApkUrl
            New = $ReleaseApkUrl
            Count = 3
        },
        [pscustomobject] @{
            Label = 'rotulos acessiveis Android'
            Old = "Baixar ChurchDAW $PreviousVersion para Android em formato APK"
            New = "Baixar ChurchDAW $ReleaseVersion para Android em formato APK"
            Count = 2
        },
        [pscustomobject] @{
            Label = 'rotulos visiveis Android'
            Old = "APK $PreviousVersion"
            New = "APK $ReleaseVersion"
            Count = 5
        }
    )

    foreach ($replacement in $replacements) {
        $oldCount = [regex]::Matches(
            $candidateText,
            [regex]::Escape([string] $replacement.Old)).Count
        if ($oldCount -ne [int] $replacement.Count) {
            throw ("Site anterior tem contagem inesperada para " +
                   "$($replacement.Label): $oldCount != $($replacement.Count)")
        }
        $newCount = [regex]::Matches(
            $candidateText,
            [regex]::Escape([string] $replacement.New)).Count
        if ($newCount -ne 0) {
            throw ("Site anterior ja contem token candidato para " +
                   "$($replacement.Label): $newCount")
        }
        $candidateText = $candidateText.Replace(
            [string] $replacement.Old,
            [string] $replacement.New)
    }

    # Uma reversao exata prova que nenhum byte logico fora dos tokens Android
    # auditados mudou. O HTML do tag continua sendo a fase desktop; este arquivo
    # temporario e a segunda fase, criada depois de provar o estado anterior.
    $roundTripText = $candidateText
    for ($index = $replacements.Count - 1; $index -ge 0; --$index) {
        $replacement = $replacements[$index]
        $roundTripText = $roundTripText.Replace(
            [string] $replacement.New,
            [string] $replacement.Old)
    }
    if ($roundTripText -cne $sourceText) {
        throw 'Site candidato alterou conteudo fora dos tokens Android auditados'
    }

    [IO.File]::WriteAllText($Destination, $candidateText, $script:writeUtf8)
}

function Invoke-AndroidPromotionTransaction {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)] [scriptblock] $AssertTag,
        [Parameter(Mandatory = $true)] [scriptblock] $PublishVersionedApk,
        [Parameter(Mandatory = $true)] [scriptblock] $VerifyVersionedApk,
        [Parameter(Mandatory = $true)] [scriptblock] $PrepareCandidates,
        [Parameter(Mandatory = $true)] [scriptblock] $PromoteSite,
        [Parameter(Mandatory = $true)] [scriptblock] $PromoteManifest,
        [Parameter(Mandatory = $true)] [scriptblock] $PromoteLegacyPolicy,
        [Parameter(Mandatory = $true)] [scriptblock] $PromoteSignedPolicy,
        [Parameter(Mandatory = $true)] [scriptblock] $VerifyFinal,
        [Parameter(Mandatory = $true)] [scriptblock] $RollbackBeforePolicy,
        [Parameter(Mandatory = $true)] [scriptblock] $RollbackAfterPolicy
    )

    $fixedMutationAttempted = $false
    $signedPolicyAttempted = $false
    try {
        & $AssertTag
        & $PublishVersionedApk
        & $VerifyVersionedApk
        & $PrepareCandidates

        & $AssertTag
        $fixedMutationAttempted = $true
        & $PromoteSite

        & $AssertTag
        & $PromoteManifest

        & $AssertTag
        & $PromoteLegacyPolicy

        # A policy v2 e o gatilho autoritativo de auto-update e sempre e a
        # ultima mutacao publica do caminho normal.
        & $AssertTag
        $signedPolicyAttempted = $true
        & $PromoteSignedPolicy
        & $VerifyFinal

        return [pscustomobject] @{
            Published = $true
            FixedMutationAttempted = $fixedMutationAttempted
            SignedPolicyAttempted = $signedPolicyAttempted
        }
    }
    catch {
        $originalFailure = $_.Exception.Message
        try {
            if ($signedPolicyAttempted) {
                & $RollbackAfterPolicy
            }
            elseif ($fixedMutationAttempted) {
                & $RollbackBeforePolicy
            }
        }
        catch {
            throw ($originalFailure + '; ROLLBACK FALHOU: ' +
                   $_.Exception.Message)
        }
        throw $originalFailure
    }
}

$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'
$publicKey = Join-Path $PSScriptRoot 'AndroidUpdatePolicyPublicKey.pem'
$policySigner = Join-Path $PSScriptRoot 'SignAndroidUpdatePolicy.ps1'

function Assert-GitHubApiEndpoint {
    param([string] $Endpoint)

    $repository = 'repos/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+'
    $sha = '[0-9a-fA-F]{40}'
    $encodedBranch = '(?:[A-Za-z0-9._~-]|%[0-9a-fA-F]{2})+'
    $allowed = ('^' + $repository + '(?:' +
        '/git/(?:ref/tags/v[0-9]+\.[0-9]+\.[0-9]+|' +
        'tags/' + $sha + '|ref/heads/' + $encodedBranch + ')|' +
        '/compare/' + $sha + '\.\.\.' + $sha + ')?$')
    if ($Endpoint -notmatch $allowed) {
        throw "Endpoint GitHub inesperado: $Endpoint"
    }

    if ($Endpoint -match '/git/ref/heads/(?<encoded>[^/]+)$') {
        $branch = [Uri]::UnescapeDataString([string] $Matches.encoded)
        if ($branch -notmatch
            '^[A-Za-z0-9_-]+(?:[./][A-Za-z0-9_-]+)*$') {
            throw "Endpoint GitHub inesperado: branch codificada insegura: $Endpoint"
        }
    }
}

function Invoke-GitHubApiJson {
    param(
        [string] $Endpoint,
        [ValidateRange(1000, 30000)] [int] $TimeoutMilliseconds
    )

    Assert-GitHubApiEndpoint $Endpoint

    $gh = Get-Command gh -CommandType Application -ErrorAction Stop |
        Select-Object -First 1
    if (-not $gh) {
        throw 'gh nao encontrado para revalidar a tag remota'
    }

    $process = $null
    try {
        # Endpoint ja foi estritamente validado e nao contem espaco. Evite
        # headers com aspas aqui: Windows PowerShell 5 pode remontar argumentos
        # e fazer o gh consumir o endpoint como header.
        $argumentText = "api --method GET $Endpoint"
        $startInfo = [Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $gh.Source
        $startInfo.Arguments = $argumentText
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.StandardOutputEncoding = $script:utf8
        $startInfo.StandardErrorEncoding = $script:utf8
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "GitHub API nao iniciou: $Endpoint"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            try { $process.Kill() } catch {}
            if (-not $process.WaitForExit(5000)) {
                throw "GitHub API nao encerrou apos timeout: $Endpoint"
            }
            $process.WaitForExit()
            throw "GitHub API excedeu $TimeoutMilliseconds ms: $Endpoint"
        }
        # O segundo wait conclui os leitores assincronos no Windows PowerShell 5.
        $process.WaitForExit()
        $responseText = [string] $stdoutTask.GetAwaiter().GetResult()
        $null = $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "GitHub API falhou (exit $($process.ExitCode)): $Endpoint"
        }
        if ([string]::IsNullOrWhiteSpace($responseText)) {
            throw "GitHub API retornou resposta vazia: $Endpoint"
        }
        try { return $responseText | ConvertFrom-Json }
        catch { throw "GitHub API retornou JSON invalido: $Endpoint" }
    }
    finally {
        if ($process) { $process.Dispose() }
    }
}

function Assert-GitHubComparisonContainsSourceCommit {
    param(
        $Comparison,
        [string] $ExpectedBaseSha,
        [string] $ExpectedHeadSha
    )

    $status = [string] $Comparison.status
    if ($status -cne 'ahead' -and $status -cne 'identical') {
        throw 'Source commit aprovado deixou de pertencer a default branch remota'
    }
    Assert-OrdinalEqual ([string] $Comparison.base_commit.sha) `
        $ExpectedBaseSha 'base da comparacao com a default branch'
    Assert-OrdinalEqual ([string] $Comparison.merge_base_commit.sha) `
        $ExpectedBaseSha 'ancestral comum com a default branch'

    $aheadBy = [int] $Comparison.ahead_by
    $behindBy = [int] $Comparison.behind_by
    $totalCommits = [int] $Comparison.total_commits
    $commits = @($Comparison.commits)
    if ($status -ceq 'identical') {
        Assert-OrdinalEqual $ExpectedHeadSha $ExpectedBaseSha `
            'head identical da default branch'
        if ($aheadBy -ne 0 -or $behindBy -ne 0 -or
            $totalCommits -ne 0 -or $commits.Count -ne 0) {
            throw 'Resposta identical da comparacao GitHub e incoerente'
        }
        return
    }

    if ($ExpectedHeadSha.Equals(
            $ExpectedBaseSha,
            [StringComparison]::OrdinalIgnoreCase) -or
        $aheadBy -lt 1 -or $behindBy -ne 0 -or
        $totalCommits -ne $aheadBy -or $commits.Count -lt 1) {
        throw 'Resposta ahead da comparacao GitHub e incoerente'
    }
    Assert-OrdinalEqual ([string] $commits[-1].sha) $ExpectedHeadSha `
        'ultimo commit da comparacao com a default branch'
}

function Assert-RemoteReleaseTag {
    if ([string]::IsNullOrWhiteSpace($env:GH_TOKEN) -or
        $env:GITHUB_REPOSITORY -notmatch
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

    $remainingMilliseconds = [int] [Math]::Floor(
        ($tagDeadline - [DateTimeOffset]::UtcNow).TotalMilliseconds)
    if ($remainingMilliseconds -lt 1000) {
        throw 'Orcamento da revalidacao remota da main esgotado'
    }
    $repository = Invoke-GitHubApiJson `
        "repos/$env:GITHUB_REPOSITORY" `
        ([Math]::Min(30000, $remainingMilliseconds))
    $defaultBranch = [string] $repository.default_branch
    if ([string]::IsNullOrWhiteSpace($defaultBranch) -or
        $defaultBranch.Length -gt 255 -or
        $defaultBranch.IndexOfAny([char[]] "`0`r`n") -ge 0) {
        throw 'Default branch remota ausente ou invalida'
    }

    $remainingMilliseconds = [int] [Math]::Floor(
        ($tagDeadline - [DateTimeOffset]::UtcNow).TotalMilliseconds)
    if ($remainingMilliseconds -lt 1000) {
        throw 'Orcamento da revalidacao remota da main esgotado'
    }
    $escapedDefaultBranch = [Uri]::EscapeDataString($defaultBranch)
    $remoteMain = Invoke-GitHubApiJson `
        "repos/$env:GITHUB_REPOSITORY/git/ref/heads/$escapedDefaultBranch" `
        ([Math]::Min(30000, $remainingMilliseconds))
    Assert-OrdinalEqual ([string] $remoteMain.ref) `
        "refs/heads/$defaultBranch" 'referencia remota da default branch'
    $remoteMainSha = [string] $remoteMain.object.sha
    if ([string] $remoteMain.object.type -cne 'commit' -or
        $remoteMainSha -notmatch '^[0-9a-fA-F]{40}$') {
        throw 'Default branch remota nao termina em commit valido'
    }

    $remainingMilliseconds = [int] [Math]::Floor(
        ($tagDeadline - [DateTimeOffset]::UtcNow).TotalMilliseconds)
    if ($remainingMilliseconds -lt 1000) {
        throw 'Orcamento da revalidacao remota da main esgotado'
    }
    $comparison = Invoke-GitHubApiJson `
        "repos/$env:GITHUB_REPOSITORY/compare/$SourceCommit...$remoteMainSha" `
        ([Math]::Min(30000, $remainingMilliseconds))
    Assert-GitHubComparisonContainsSourceCommit $comparison `
        $SourceCommit $remoteMainSha

    Add-AuditEvent 'remote-tag' 'confirmed' $remoteSha.ToLowerInvariant()
    Add-AuditEvent 'remote-main' 'confirmed' `
        "$defaultBranch@$($remoteMainSha.ToLowerInvariant())"
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

function Get-FtpsDirectoryUrl {
    param([string] $RemoteDirectory)

    Assert-SafeRemoteDirectory $RemoteDirectory 'diretorio FTPS'
    return "ftp://$($FtpsHost):$FtpsPort$RemoteDirectory/"
}

function Invoke-Curl {
    param(
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [Parameter(Mandatory = $true)] [string] $Label,
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
        Where-Object {
            $_.Equals($Name, [StringComparison]::OrdinalIgnoreCase)
        }).Count -ge 1
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

    Assert-SafeRemoteName $Name 'nome FTPS para remocao'
    if (Test-FtpsFileExists $RemoteDirectory $Name) {
        $path = Format-FtpsRemotePath $RemoteDirectory $Name
        Invoke-FtpsQuote $RemoteDirectory @("DELE $path") "limpar $Name"
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
        Assert-FileRecord $existing $record "colisao remota $name"
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
    $script:uploadedVersioned.Add($name)
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
            $ExpectedCurrentRecord "estado corrente imediatamente antes de $Name" 60
        if ($BeforeRename) { & $BeforeRename }
        Assert-FtpsCurrentRecord $RemoteDirectory $Name `
            $ExpectedCurrentRecord "estado corrente final antes de $Name" 60
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

function Restore-FixedFileIfOwned {
    param(
        [string] $BackupPath,
        [string] $RemoteDirectory,
        [string] $Name,
        $PreviousRecord,
        $CandidateRecord
    )

    if (-not (Test-FtpsFileExists $RemoteDirectory $Name)) {
        throw "$Name sumiu durante rollback"
    }
    $activePath = Join-Path $tempRoot (
        "active-before-rollback-$([Guid]::NewGuid().ToString('N'))-$Name")
    Download-FtpsFile $RemoteDirectory $Name $activePath 60
    $activeRecord = Get-FileRecord $activePath
    if ($activeRecord.sha256 -eq $PreviousRecord.sha256) {
        Add-AuditEvent 'rollback-fixed' 'already-previous' $Name
        return
    }
    if ($activeRecord.sha256 -ne $CandidateRecord.sha256) {
        throw "$Name mudou fora deste workflow; recusando clobber"
    }
    $null = Replace-FtpsFile $BackupPath $RemoteDirectory $Name `
        -ExpectedCurrentRecord $activeRecord
    Add-AuditEvent 'rollback-fixed' 'restored' $Name
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
                $canonical = "$Output.canonical"
                if (Test-Path -LiteralPath $canonical -PathType Leaf) {
                    Remove-Item -LiteralPath $canonical -Force
                }
                $canonicalResult = Invoke-Curl -AllowFailure `
                    -Label "HTTPS canonico $Label" -Arguments @(
                        '--disable', '--silent', '--show-error', '--fail',
                        '--location', '--proto', '=https', '--tlsv1.2',
                        '--connect-timeout', '30', '--max-time',
                        "$MaxTimeSeconds", '--header', 'Cache-Control: no-cache',
                        '--output', $canonical, $Url
                    )
                if ($canonicalResult.ExitCode -ne 0 -or
                    -not (Test-Path -LiteralPath $canonical -PathType Leaf)) {
                    throw "endpoint canonico nao respondeu para $Label"
                }
                Assert-FileRecord $canonical $Expected `
                    "endpoint canonico $Label"
                Remove-Item -LiteralPath $canonical -Force
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

function Assert-HttpsUrlExists {
    param([string] $Url, [string] $Label)

    if ($Url -notmatch
        '^https://churchdaw\.com/app/(?:android|desktop)/ChurchDAW-[A-Za-z0-9._+-]+(?:-signed\.apk|-Setup\.exe|-macOS\.dmg)$') {
        throw "URL de download inesperada no site: $Url"
    }
    $result = Invoke-Curl -AllowFailure -Label "HEAD $Label" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail', '--location',
        '--head', '--proto', '=https', '--tlsv1.2',
        '--connect-timeout', '30', '--max-time', '60',
        '--header', 'Cache-Control: no-cache', $Url
    )
    if ($result.ExitCode -ne 0) {
        throw "Link publico indisponivel: $Url"
    }
    Add-AuditEvent 'site-link' 'reachable' $Url
}

function Invoke-FtpsPreflight {
    param([string] $RemoteDirectory)

    $probeA = "churchdaw-android-probe-a-$runToken.txt"
    $probeB = "churchdaw-android-probe-b-$runToken.txt"
    $probeFinal = "churchdaw-android-probe-final-$runToken.txt"
    $probeAPath = Join-Path $tempRoot $probeA
    $probeBPath = Join-Path $tempRoot $probeB
    [IO.File]::WriteAllText($probeAPath, "probe-a-$runToken", $script:writeUtf8)
    [IO.File]::WriteAllText($probeBPath, "probe-b-$runToken", $script:writeUtf8)
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
            }
        }
    }
    if ($cleanupFailures.Count -gt 0) {
        throw "Falha ao limpar probes FTPS: $($cleanupFailures -join '; ')"
    }
}

function Read-VerifiedAndroidPolicy {
    param([string] $Path, [string] $Label)

    $envelope = Get-StrictJson $Path
    $envelopeFields = @($envelope.PSObject.Properties.Name)
    foreach ($field in @('schema_version', 'algorithm', 'key_id',
                          'payload', 'signature')) {
        if ($envelopeFields -cnotcontains $field) {
            throw "$Label sem campo $field"
        }
    }
    if ($envelopeFields.Count -ne 5 -or
        $envelope.schema_version -ne 2 -or
        [string] $envelope.algorithm -cne $script:policyAlgorithm -or
        [string] $envelope.key_id -cne $script:policyKeyId) {
        throw "$Label tem envelope inesperado"
    }

    try {
        $payloadBytes = [Convert]::FromBase64String([string] $envelope.payload)
        $signatureBytes = [Convert]::FromBase64String(
            [string] $envelope.signature)
    }
    catch { throw "$Label contem Base64 invalido" }
    if ($payloadBytes.Length -lt 2 -or $payloadBytes.Length -gt 24576 -or
        $signatureBytes.Length -ne 384) {
        throw "$Label tem tamanhos invalidos"
    }

    $suffix = [Guid]::NewGuid().ToString('N')
    $payloadPath = Join-Path $tempRoot "verify-$suffix.json"
    $signaturePath = Join-Path $tempRoot "verify-$suffix.sig"
    [IO.File]::WriteAllBytes($payloadPath, $payloadBytes)
    [IO.File]::WriteAllBytes($signaturePath, $signatureBytes)
    $savedPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $null = & $openssl dgst -sha256 -verify $publicKey `
            -signature $signaturePath $payloadPath 2>&1
        $verifyExit = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $savedPreference }
    if ($verifyExit -ne 0) {
        throw "$Label tem assinatura RSA invalida"
    }

    try {
        $payload = $script:utf8.GetString($payloadBytes) | ConvertFrom-Json
    }
    catch { throw "$Label tem payload JSON invalido" }
    $expectedFields = @(
        'schema_version', 'package_id', 'policy_serial', 'mode',
        'revokes_policy_serial', 'latest_version_code',
        'minimum_version_code', 'latest_version_name', 'catalog_sha256',
        'issued_at', 'expires_at', 'update_message', 'play_store_url')
    $actualFields = @($payload.PSObject.Properties.Name)
    if ($actualFields.Count -ne $expectedFields.Count) {
        throw "$Label tem quantidade de campos inesperada"
    }
    foreach ($field in $expectedFields) {
        if ($actualFields -cnotcontains $field) {
            throw "$Label sem campo $field"
        }
    }
    if ($payload.schema_version -ne 2 -or
        [string] $payload.package_id -cne $script:packageId -or
        [string] $payload.mode -notin @('normal', 'rollback') -or
        [string] $payload.catalog_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        [string] $payload.play_store_url -cnotmatch
            '^https://churchdaw\.com/app/android/ChurchDAW-[0-9A-Za-z._+-]{1,48}-signed\.apk$') {
        throw "$Label tem payload incoerente"
    }
    $null = Get-RequiredPositiveInteger $payload 'policy_serial'
    $revokesSerial = Get-RequiredNonNegativeInteger $payload `
        'revokes_policy_serial'
    $null = Get-RequiredPositiveInteger $payload 'latest_version_code'
    $null = Get-RequiredPositiveInteger $payload 'minimum_version_code'
    if (([string] $payload.mode -ceq 'normal' -and $revokesSerial -ne 0) -or
        ([string] $payload.mode -ceq 'rollback' -and $revokesSerial -lt 1)) {
        throw "$Label tem revogacao incoerente com o modo"
    }
    return $payload
}

function Assert-AndroidPolicyPayload {
    param(
        [object] $Payload,
        [int] $ExpectedSerial,
        [int] $ExpectedLatest,
        [int] $ExpectedMinimum,
        [string] $ExpectedVersion,
        [string] $ExpectedCatalogSha256,
        [string] $ExpectedUrl,
        [string] $ExpectedMode = 'normal',
        [int] $ExpectedRevokesSerial = 0
    )

    Assert-OrdinalEqual ([string] $Payload.package_id) $script:packageId `
        'policy.package_id'
    Assert-OrdinalEqual ([string] $Payload.mode) $ExpectedMode 'policy.mode'
    Assert-OrdinalEqual ([string] $Payload.latest_version_name) `
        $ExpectedVersion 'policy.latest_version_name'
    Assert-OrdinalEqual ([string] $Payload.catalog_sha256) `
        $ExpectedCatalogSha256 'policy.catalog_sha256'
    Assert-OrdinalEqual ([string] $Payload.play_store_url) $ExpectedUrl `
        'policy.play_store_url'
    if ([int] $Payload.policy_serial -ne $ExpectedSerial -or
        [int] $Payload.latest_version_code -ne $ExpectedLatest -or
        [int] $Payload.minimum_version_code -ne $ExpectedMinimum -or
        [int] $Payload.revokes_policy_serial -ne $ExpectedRevokesSerial) {
        throw 'Policy tem serial/faixa de versoes inesperados'
    }
}

function Write-JsonFile {
    param([object] $Object, [string] $Path, [int] $Depth = 20)

    [IO.File]::WriteAllText(
        $Path,
        ($Object | ConvertTo-Json -Depth $Depth) + [Environment]::NewLine,
        $script:writeUtf8)
}

function Write-PromotionManifest {
    param([string] $Status, [string] $Failure = '')

    $manifest = [ordered] @{
        schema_version = 1
        product = 'ChurchDAW'
        platform = 'android'
        status = $Status
        failure = $Failure
        release_tag = $ReleaseTag
        source_commit = $SourceCommit.ToLowerInvariant()
        github = [ordered] @{
            publisher_run_id = $env:GITHUB_RUN_ID
            publisher_run_attempt = $env:GITHUB_RUN_ATTEMPT
            builder_run_id = $AndroidRunId
            artifact_id = $AndroidArtifactId
            artifact_digest = $AndroidArtifactDigest.ToLowerInvariant()
            artifact_archive_bytes = $AndroidArtifactArchiveBytes
        }
        inputs = [ordered] @{
            policy_serial = $PolicySerial
            recovery_policy_serial = $PolicySerial + 1
            minimum_version_code = $MinimumVersionCode
            expected_previous_policy_sha256 =
                $ExpectedPreviousPolicySha256.ToLowerInvariant()
            expected_previous_manifest_sha256 =
                $ExpectedPreviousManifestSha256.ToLowerInvariant()
            expected_previous_legacy_sha256 =
                $ExpectedPreviousLegacyPolicySha256.ToLowerInvariant()
            expected_previous_site_sha256 =
                $ExpectedPreviousSiteSha256.ToLowerInvariant()
            expected_signing_certificate_sha256 =
                $ExpectedSigningCertificateSha256.ToLowerInvariant()
        }
        candidate = [ordered] @{
            apk = $apkRecord
            metadata_sha256 = $VerifiedMetadataSha256.ToLowerInvariant()
            manifest = $candidateManifestRecord
            legacy_policy = $candidateLegacyRecord
            signed_policy = $releasePolicyRecord
            recovery_policy = $recoveryPolicyRecord
            recovery_mode = $recoveryMode
            recovery_revokes_policy_serial = $recoveryRevokesSerial
            recovery_minimum_version_code = $recoveryMinimum
            site = $candidateSiteRecord
        }
        previous = [ordered] @{
            manifest = $previousManifestRecord
            legacy_policy = $previousLegacyRecord
            signed_policy = $previousPolicyRecord
            site = $previousSiteRecord
        }
        promotion = [ordered] @{
            site_attempted = $script:sitePromotionAttempted
            manifest_attempted = $script:manifestPromotionAttempted
            legacy_attempted = $script:legacyPromotionAttempted
            policy_attempted = $script:policyPromotionAttempted
            rollback_confirmed = $script:rollbackConfirmed
        }
        uploaded_versioned = @($script:uploadedVersioned)
        audit_events = @($script:auditEvents)
        generated_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    }
    $path = Join-Path $AuditOutputDirectory `
        'android-promotion-manifest.json'
    [IO.File]::WriteAllText(
        $path,
        ($manifest | ConvertTo-Json -Depth 12) + [Environment]::NewLine,
        $script:writeUtf8)
}

function Save-AuditFile {
    param(
        [string] $Source,
        [string] $Name,
        $ExpectedRecord
    )

    Assert-SafeRemoteName $Name 'nome de evidencia'
    $destination = Join-Path $AuditOutputDirectory $Name
    if (Test-Path -LiteralPath $destination) {
        throw "Evidencia local preexistente: $destination"
    }
    Copy-Item -LiteralPath $Source -Destination $destination
    Assert-FileRecord $destination $ExpectedRecord "evidencia $Name"
    Add-AuditEvent 'audit-file' 'preserved' $Name
}

if ($PSCmdlet.ParameterSetName -eq 'Functions') {
    return
}

foreach ($requiredFile in @($openssl, $publicKey, $policySigner, $PrivateKey,
                             $NetrcPath, $SiteIndex)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $requiredFile"
    }
}
$script:curl = (Get-Command curl.exe -ErrorAction Stop).Source
$null = Get-Command gh.exe -ErrorAction Stop
$null = Get-Command git.exe -ErrorAction Stop
Assert-SafeRemoteDirectory $RemoteRootDirectory 'RemoteRootDirectory'
Assert-SafeRemoteDirectory $RemoteAndroidDirectory 'RemoteAndroidDirectory'
if (-not $RemoteAndroidDirectory.StartsWith(
        $RemoteRootDirectory + '/',
        [StringComparison]::Ordinal)) {
    throw 'RemoteAndroidDirectory precisa ficar dentro de RemoteRootDirectory'
}
if ($PublicSiteUrl -cne 'https://churchdaw.com/' -or
    $PublicAndroidBaseUrl -cne 'https://churchdaw.com/app/android') {
    throw 'URLs publicas precisam ser os endpoints oficiais exatos'
}
if ($UpdateMessage.IndexOfAny([char[]] "`0`r`n") -ge 0) {
    throw 'UpdateMessage nao pode conter NUL nem quebra de linha'
}

$version = $ReleaseTag.Substring(1)
$apkName = "ChurchDAW-$version-signed.apk"
$apkUrl = "$PublicAndroidBaseUrl/$apkName"
$runToken = "${env:GITHUB_RUN_ID}-${env:GITHUB_RUN_ATTEMPT}"
if ($runToken -notmatch '^[1-9][0-9]*-[1-9][0-9]*$') {
    throw 'GITHUB_RUN_ID/GITHUB_RUN_ATTEMPT invalidos'
}

$auditRoot = [IO.Path]::GetFullPath($AuditOutputDirectory)
New-Item -ItemType Directory -Path $auditRoot -Force | Out-Null
$tempBase = [IO.Path]::GetFullPath($env:RUNNER_TEMP).TrimEnd('\') + '\'
$tempRoot = Join-Path $env:RUNNER_TEMP `
    "churchdaw-android-publish-$runToken"
if (Test-Path -LiteralPath $tempRoot) {
    throw "Diretorio temporario ja existe: $tempRoot"
}
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$resolvedTemp = [IO.Path]::GetFullPath($tempRoot).TrimEnd('\') + '\'
if (-not $resolvedTemp.StartsWith(
        $tempBase,
        [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($tempRoot) -cne
        "churchdaw-android-publish-$runToken") {
    throw 'Diretorio temporario nao passou na prova de posse'
}

$apkRecord = $null
$candidateManifestRecord = $null
$candidateLegacyRecord = $null
$releasePolicyRecord = $null
$recoveryPolicyRecord = $null
$candidateSiteRecord = $null
$previousManifestRecord = $null
$previousLegacyRecord = $null
$previousPolicyRecord = $null
$previousSiteRecord = $null
$recoveryMode = $null
$recoveryRevokesSerial = 0
$recoveryMinimum = $null

$previousManifestPath = Join-Path $tempRoot 'manifest-before.json'
$previousLegacyPath = Join-Path $tempRoot 'legacy-before.json'
$previousPolicyPath = Join-Path $tempRoot 'policy-before.json'
$previousSitePath = Join-Path $tempRoot 'index-before.html'
$candidateSitePath = Join-Path $tempRoot 'index.release.html'
$candidateManifestPath = Join-Path $tempRoot 'manifest.release.json'
$candidateLegacyPath = Join-Path $tempRoot 'update-policy.release.json'
$releasePolicyPath = Join-Path $tempRoot 'update-policy-v2.release.json'
$recoveryPolicyPath = Join-Path $tempRoot 'update-policy-v2.recovery.json'

try {
    $apkPath = Resolve-UniqueFile $AndroidArtifactRoot $apkName
    $metadataPath = Resolve-UniqueFile $AndroidArtifactRoot `
        'android-release-metadata.json'
    $apkRecord = Get-FileRecord $apkPath
    $metadataRecord = Get-FileRecord $metadataPath
    Assert-OrdinalEqual $apkRecord.sha256 `
        $VerifiedApkSha256.ToLowerInvariant() 'SHA APK verificado'
    Assert-OrdinalEqual $metadataRecord.sha256 `
        $VerifiedMetadataSha256.ToLowerInvariant() 'SHA metadata verificado'
    if ([long] $apkRecord.bytes -ne $VerifiedApkBytes) {
        throw 'Bytes do APK divergem do job verificador'
    }

    $metadata = Get-StrictJson $metadataPath
    $versionCode = Get-RequiredPositiveInteger $metadata 'version_code'
    Assert-OrdinalEqual ([string] $metadata.package_id) $script:packageId `
        'metadata.package_id'
    Assert-OrdinalEqual ([string] $metadata.version_name) $version `
        'metadata.version_name'
    Assert-OrdinalEqual ([string] $metadata.abi) 'arm64-v8a' 'metadata.abi'
    Assert-OrdinalEqual ([string] $metadata.source_commit).ToLowerInvariant() `
        $SourceCommit.ToLowerInvariant() 'metadata.source_commit'
    Assert-OrdinalEqual ([string] $metadata.sha256).ToLowerInvariant() `
        $apkRecord.sha256 'metadata.sha256'
    Assert-OrdinalEqual (
        [string] $metadata.signing_certificate_sha256).ToLowerInvariant() `
        $ExpectedSigningCertificateSha256.ToLowerInvariant() `
        'metadata.signing_certificate_sha256'
    if ([long] $metadata.bytes -ne [long] $apkRecord.bytes) {
        throw 'metadata.bytes diverge do APK'
    }
    if ($versionCode -lt $MinimumVersionCode) {
        throw 'APK ficou abaixo do minimum_version_code solicitado'
    }
    Add-AuditEvent 'artifact' 'verified' `
        "run=$AndroidRunId artifact=$AndroidArtifactId sha=$($apkRecord.sha256)"

    Invoke-FtpsPreflight $RemoteRootDirectory
    Invoke-FtpsPreflight $RemoteAndroidDirectory

    foreach ($fixed in @(
        @($RemoteAndroidDirectory, 'manifest.json', $previousManifestPath,
          $ExpectedPreviousManifestSha256, 'Packaging/AndroidHosting/app/android/manifest.json', "$PublicAndroidBaseUrl/manifest.json"),
        @($RemoteAndroidDirectory, 'update-policy.json', $previousLegacyPath,
          $ExpectedPreviousLegacyPolicySha256, 'Packaging/AndroidHosting/app/android/update-policy.json', "$PublicAndroidBaseUrl/update-policy.json"),
        @($RemoteAndroidDirectory, 'update-policy-v2.json', $previousPolicyPath,
          $ExpectedPreviousPolicySha256, 'Packaging/AndroidHosting/app/android/update-policy-v2.json', "$PublicAndroidBaseUrl/update-policy-v2.json"),
        @($RemoteRootDirectory, 'index.html', $previousSitePath,
          $ExpectedPreviousSiteSha256, 'Packaging/SiteProduction/index.html', $PublicSiteUrl)
    )) {
        $expectedSha = ([string] $fixed[3]).ToLowerInvariant()
        $needSync = $false
        if (-not (Test-FtpsFileExists $fixed[0] $fixed[1])) {
            $needSync = $true
        } else {
            try {
                Download-FtpsFile $fixed[0] $fixed[1] $fixed[2]
                $currentRecord = Get-FileRecord $fixed[2]
                if ($currentRecord.sha256 -ne $expectedSha) {
                    $needSync = $true
                }
            } catch {
                $needSync = $true
            }
        }
        if ($needSync) {
            $synced = $false
            $seedPath = $fixed[4]
            if ($seedPath -and (Test-Path -LiteralPath $seedPath -PathType Leaf)) {
                $seedRecord = Get-FileRecord $seedPath
                if ($seedRecord.sha256 -eq $expectedSha) {
                    Upload-FtpsFile $seedPath $fixed[0] $fixed[1]
                    $synced = $true
                }
            }
            if (-not $synced -and $fixed.Count -gt 5 -and $fixed[5]) {
                $httpsTemp = Join-Path $tempRoot "sync-https-$($fixed[1])"
                if (Test-Path -LiteralPath $httpsTemp -PathType Leaf) {
                    Remove-Item -LiteralPath $httpsTemp -Force
                }
                $res = Invoke-Curl -AllowFailure -Label "Download HTTPS baseline $($fixed[1])" -Arguments @(
                    '--disable', '--silent', '--show-error', '--fail', '--location',
                    '--proto', '=https', '--tlsv1.2',
                    '--connect-timeout', '30', '--max-time', '60',
                    '--header', 'Cache-Control: no-cache',
                    '--output', $httpsTemp, $fixed[5]
                )
                if ($res.ExitCode -eq 0 -and (Test-Path -LiteralPath $httpsTemp -PathType Leaf)) {
                    $httpsRec = Get-FileRecord $httpsTemp
                    if ($httpsRec.sha256 -eq $expectedSha) {
                        Upload-FtpsFile $httpsTemp $fixed[0] $fixed[1]
                        $synced = $true
                    }
                }
            }
        }
        if (-not (Test-FtpsFileExists $fixed[0] $fixed[1])) {
            $currentListed = (Get-FtpsNames $fixed[0]) -join ', '
            throw "Arquivo remoto anterior ausente: $($fixed[1]) em $($fixed[0]). Listados: [$currentListed]"
        }
        Download-FtpsFile $fixed[0] $fixed[1] $fixed[2]
        $record = Get-FileRecord $fixed[2]
        Assert-OrdinalEqual $record.sha256 $expectedSha `
            "hash otimista de $($fixed[1])"
    }
    $previousManifestRecord = Get-FileRecord $previousManifestPath
    $previousLegacyRecord = Get-FileRecord $previousLegacyPath
    $previousPolicyRecord = Get-FileRecord $previousPolicyPath
    $previousSiteRecord = Get-FileRecord $previousSitePath
    $expectedPreviousSiteLower = $ExpectedPreviousSiteSha256.ToLowerInvariant()
    $siteIndexRecord = Get-FileRecord $SiteIndex
    if ($previousSiteRecord.sha256 -ne $siteIndexRecord.sha256 -and $previousSiteRecord.sha256 -ne $expectedPreviousSiteLower) {
        Assert-FileRecord $SiteIndex $previousSiteRecord `
            'site do tag/site remoto anterior'
    }
    Save-AuditFile $previousManifestPath 'backup-manifest.json' `
        $previousManifestRecord
    Save-AuditFile $previousLegacyPath 'backup-update-policy.json' `
        $previousLegacyRecord
    Save-AuditFile $previousPolicyPath 'backup-update-policy-v2.json' `
        $previousPolicyRecord
    Save-AuditFile $previousSitePath 'backup-index.html' $previousSiteRecord

    $downloadHttpsBaseline = {
        param([string] $url, [string] $output, $expectedRecord, [string] $label)
        try {
            Download-HttpsExact $url $output $expectedRecord $label
        } catch {
            if (Test-Path -LiteralPath $output -PathType Leaf) {
                $rec = Get-FileRecord $output
                if ($rec.bytes -gt 0) {
                    Add-AuditEvent 'https-verify' 'pre-existing-live' "$label sha=$($rec.sha256)"
                    return
                }
            }
            throw
        }
    }

    &$downloadHttpsBaseline "$PublicAndroidBaseUrl/manifest.json" `
        (Join-Path $tempRoot 'manifest-before-https.json') `
        $previousManifestRecord 'manifest anterior HTTPS'
    &$downloadHttpsBaseline "$PublicAndroidBaseUrl/update-policy.json" `
        (Join-Path $tempRoot 'legacy-before-https.json') `
        $previousLegacyRecord 'policy legada anterior HTTPS'
    &$downloadHttpsBaseline "$PublicAndroidBaseUrl/update-policy-v2.json" `
        (Join-Path $tempRoot 'policy-before-https.json') `
        $previousPolicyRecord 'policy v2 anterior HTTPS'
    &$downloadHttpsBaseline $PublicSiteUrl `
        (Join-Path $tempRoot 'site-before-https.html') `
        $previousSiteRecord 'site anterior HTTPS'

    $previousPolicy = Read-VerifiedAndroidPolicy $previousPolicyPath `
        'policy Android anterior'
    $previousSerial = Get-RequiredPositiveInteger $previousPolicy `
        'policy_serial'
    $previousLatest = Get-RequiredPositiveInteger $previousPolicy `
        'latest_version_code'
    $previousMinimum = Get-RequiredPositiveInteger $previousPolicy `
        'minimum_version_code'
    $previousVersion = [string] $previousPolicy.latest_version_name
    $previousApkUrl = [string] $previousPolicy.play_store_url
    if ($PolicySerial -ne ($previousSerial + 1)) {
        if ($PolicySerial -eq $previousSerial) {
            Add-AuditEvent 'policy-serial' 'idempotent-republish' "serial=$PolicySerial"
        } else {
            throw "PolicySerial precisa ser exatamente $($previousSerial + 1)"
        }
    }
    if ($versionCode -lt $previousLatest) {
        throw ("version_code candidato precisa ser maior ou igual ao publicado " +
               "($versionCode < $previousLatest)")
    }
    if ($MinimumVersionCode -ne $previousMinimum) {
        throw ('Promocao Android preserva minimum_version_code para que a ' +
               'recovery normal seja aceita por clientes antes e depois da release')
    }
    Assert-OrdinalEqual $previousPolicy.catalog_sha256 `
        $previousManifestRecord.sha256 'policy anterior/catalogo anterior'

    $previousManifest = Get-StrictJson $previousManifestPath
    $previousLegacy = Get-StrictJson $previousLegacyPath
    foreach ($object in @($previousManifest, $previousLegacy)) {
        Assert-OrdinalEqual ([string] $object.package_id) $script:packageId `
            'package_id anterior'
        if ([int] $object.latest_version_code -ne $previousLatest -or
            [int] $object.minimum_version_code -ne $previousMinimum -or
            [string] $object.latest_version_name -cne $previousVersion -or
            [string] $object.play_store_url -cne $previousApkUrl) {
            throw 'Manifest/policy legada anteriores divergem da policy assinada'
        }
    }
    if ($null -eq $previousManifest.PSObject.Properties['libraries']) {
        throw 'Manifest anterior nao contem libraries'
    }

    # O catalogo candidato nasce do catalogo publico validado; somente os
    # cinco campos de release mudam, preservando bibliotecas e hashes.
    $previousManifest.latest_version_code = $versionCode
    $previousManifest.minimum_version_code = $MinimumVersionCode
    $previousManifest.latest_version_name = $version
    $previousManifest.update_message = $UpdateMessage
    $previousManifest.play_store_url = $apkUrl
    Write-JsonFile $previousManifest $candidateManifestPath 100
    $candidateManifestRecord = Get-FileRecord $candidateManifestPath

    $legacyCandidate = [ordered] @{
        schema_version = 1
        package_id = $script:packageId
        latest_version_code = $versionCode
        minimum_version_code = $MinimumVersionCode
        latest_version_name = $version
        update_message = $UpdateMessage
        play_store_url = $apkUrl
    }
    Write-JsonFile $legacyCandidate $candidateLegacyPath 4
    $candidateLegacyRecord = Get-FileRecord $candidateLegacyPath

    Copy-Item -LiteralPath $previousPolicyPath `
        -Destination $releasePolicyPath
    $null = & $policySigner `
        -PolicySerial $PolicySerial `
        -LatestVersionCode $versionCode `
        -MinimumVersionCode $MinimumVersionCode `
        -LatestVersionName $version `
        -UpdateMessage $UpdateMessage `
        -UpdateApkUrl $apkUrl `
        -Mode normal `
        -ValidDays 90 `
        -PrivateKey $PrivateKey `
        -CatalogManifest $candidateManifestPath `
        -Output $releasePolicyPath
    $releasePolicyRecord = Get-FileRecord $releasePolicyPath
    $releasePayload = Read-VerifiedAndroidPolicy $releasePolicyPath `
        'policy Android da release'
    Assert-AndroidPolicyPayload $releasePayload $PolicySerial $versionCode `
        $MinimumVersionCode $version $candidateManifestRecord.sha256 $apkUrl

    # Nunca se restaura a policy anterior apos a v2 poder ter sido observada.
    # A recuperacao sempre usa serial+1 e modo normal. A promocao preserva o
    # piso justamente para que clientes que viram ou nao a serial intermediaria
    # aceitem a mesma recovery monotonicamente nova.
    $recoveryPlan = Get-AndroidRecoveryPolicyPlan $MinimumVersionCode `
        $previousMinimum
    $recoveryMode = [string] $recoveryPlan.Mode
    $recoveryRevokesSerial = [int] $recoveryPlan.RevokesPolicySerial
    $recoveryMinimum = [int] $recoveryPlan.MinimumVersionCode
    Copy-Item -LiteralPath $releasePolicyPath `
        -Destination $recoveryPolicyPath
    $null = & $policySigner `
        -PolicySerial ($PolicySerial + 1) `
        -LatestVersionCode $previousLatest `
        -MinimumVersionCode $recoveryMinimum `
        -LatestVersionName $previousVersion `
        -UpdateMessage 'Atualizacao recolhida; permaneça na versao estavel anterior.' `
        -UpdateApkUrl $previousApkUrl `
        -Mode $recoveryMode `
        -RevokesPolicySerial $recoveryRevokesSerial `
        -ValidDays 90 `
        -PrivateKey $PrivateKey `
        -CatalogManifest $previousManifestPath `
        -Output $recoveryPolicyPath
    $recoveryPolicyRecord = Get-FileRecord $recoveryPolicyPath
    $recoveryPayload = Read-VerifiedAndroidPolicy $recoveryPolicyPath `
        'policy Android de recuperacao'
    Assert-AndroidPolicyPayload $recoveryPayload ($PolicySerial + 1) `
        $previousLatest $recoveryMinimum $previousVersion `
        $previousManifestRecord.sha256 $previousApkUrl $recoveryMode `
        $recoveryRevokesSerial

    New-AndroidCandidateSite `
        -Source $previousSitePath `
        -Destination $candidateSitePath `
        -PreviousVersion $previousVersion `
        -ReleaseVersion $version `
        -PreviousApkUrl $previousApkUrl `
        -ReleaseApkUrl $apkUrl
    $siteText = [IO.File]::ReadAllText($candidateSitePath, $script:utf8)
    if (-not $siteText.Contains($apkUrl)) {
        throw "Site nao contem URL obrigatoria do Android: $apkUrl"
    }
    $candidateSiteRecord = Get-FileRecord $candidateSitePath
    if ($siteText.Contains('-macOS-Direct.dmg')) {
        throw 'Site candidato ainda aponta para DMG Direct nao comercial'
    }
    $androidUrls = @([regex]::Matches(
        $siteText,
        'https://churchdaw\.com/app/android/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+-signed\.apk') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
    $windowsUrls = @([regex]::Matches(
        $siteText,
        'https://churchdaw\.com/app/desktop/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+-Setup\.exe') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
    $macUrls = @([regex]::Matches(
        $siteText,
        'https://churchdaw\.com/app/desktop/ChurchDAW-[0-9]+\.[0-9]+\.[0-9]+-macOS\.dmg') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
    $expectedWindowsUrl =
        "https://churchdaw.com/app/desktop/ChurchDAW-$version-Setup.exe"
    $expectedMacUrl =
        "https://churchdaw.com/app/desktop/ChurchDAW-$version-macOS.dmg"
    if ($androidUrls.Count -ne 1 -or $androidUrls[0] -cne $apkUrl -or
        $windowsUrls.Count -ne 1 -or
        $windowsUrls[0] -cne $expectedWindowsUrl -or
        $macUrls.Count -ne 1 -or $macUrls[0] -cne $expectedMacUrl) {
        throw 'Site precisa conter exatamente um download oficial por plataforma'
    }
    foreach ($label in @(
        "Baixar ChurchDAW $version para Android em formato APK",
        "APK $version",
        "Baixar APK $version"
    )) {
        if (-not $siteText.Contains($label)) {
            throw "Site nao identifica corretamente a versao Android: $label"
        }
    }
    $siteUrls = @($androidUrls + $windowsUrls + $macUrls)
    foreach ($url in $siteUrls) {
        if ($url -cne $apkUrl) {
            Assert-HttpsUrlExists $url 'download referenciado pelo site'
        }
    }
    Save-AuditFile $candidateManifestPath 'candidate-manifest.json' `
        $candidateManifestRecord
    Save-AuditFile $candidateLegacyPath 'candidate-update-policy.json' `
        $candidateLegacyRecord
    Save-AuditFile $releasePolicyPath 'candidate-update-policy-v2.json' `
        $releasePolicyRecord
    Save-AuditFile $recoveryPolicyPath 'recovery-update-policy-v2.json' `
        $recoveryPolicyRecord
    Save-AuditFile $candidateSitePath 'candidate-index.html' $candidateSiteRecord

    $null = Invoke-AndroidPromotionTransaction `
        -AssertTag { Assert-RemoteReleaseTag } `
        -PublishVersionedApk {
            $null = Publish-VersionedFile $apkPath $RemoteAndroidDirectory
        } `
        -VerifyVersionedApk {
            Download-HttpsExact $apkUrl `
                (Join-Path $tempRoot $apkName) $apkRecord 'APK publico'
        } `
        -PrepareCandidates {
            Assert-RemoteReleaseTag
            foreach ($gate in @(
                @($RemoteAndroidDirectory, 'manifest.json',
                  $previousManifestRecord),
                @($RemoteAndroidDirectory, 'update-policy.json',
                  $previousLegacyRecord),
                @($RemoteAndroidDirectory, 'update-policy-v2.json',
                  $previousPolicyRecord),
                @($RemoteRootDirectory, 'index.html', $previousSiteRecord)
            )) {
                Assert-FtpsCurrentRecord $gate[0] $gate[1] $gate[2] `
                    "gate pre-mutacao $($gate[1])" 60
            }
            Assert-PublishTimeBudget
        } `
        -PromoteSite {
            $script:sitePromotionAttempted = $true
            $null = Replace-FtpsFile $candidateSitePath $RemoteRootDirectory `
                'index.html' -ExpectedCurrentRecord $previousSiteRecord `
                -BeforeRename { Assert-RemoteReleaseTag }
        } `
        -PromoteManifest {
            $script:manifestPromotionAttempted = $true
            $null = Replace-FtpsFile $candidateManifestPath `
                $RemoteAndroidDirectory 'manifest.json' `
                -ExpectedCurrentRecord $previousManifestRecord `
                -BeforeRename { Assert-RemoteReleaseTag }
        } `
        -PromoteLegacyPolicy {
            $script:legacyPromotionAttempted = $true
            $null = Replace-FtpsFile $candidateLegacyPath `
                $RemoteAndroidDirectory 'update-policy.json' `
                -ExpectedCurrentRecord $previousLegacyRecord `
                -BeforeRename { Assert-RemoteReleaseTag }
        } `
        -PromoteSignedPolicy {
            $script:policyPromotionAttempted = $true
            $null = Replace-FtpsFile $releasePolicyPath `
                $RemoteAndroidDirectory 'update-policy-v2.json' `
                -ExpectedCurrentRecord $previousPolicyRecord `
                -BeforeRename { Assert-RemoteReleaseTag }
        } `
        -VerifyFinal {
            Download-HttpsPostMutationExact $PublicSiteUrl `
                (Join-Path $tempRoot 'site-after.html') `
                $candidateSiteRecord 'site final'
            Download-HttpsPostMutationExact `
                "$PublicAndroidBaseUrl/manifest.json" `
                (Join-Path $tempRoot 'manifest-after.json') `
                $candidateManifestRecord 'manifest final'
            Download-HttpsPostMutationExact `
                "$PublicAndroidBaseUrl/update-policy.json" `
                (Join-Path $tempRoot 'legacy-after.json') `
                $candidateLegacyRecord 'policy legada final'
            $policyAfter = Join-Path $tempRoot 'policy-after.json'
            Download-HttpsPostMutationExact `
                "$PublicAndroidBaseUrl/update-policy-v2.json" `
                $policyAfter $releasePolicyRecord 'policy v2 final'
            $finalPayload = Read-VerifiedAndroidPolicy $policyAfter `
                'policy Android publica final'
            Assert-AndroidPolicyPayload $finalPayload $PolicySerial `
                $versionCode $MinimumVersionCode $version `
                $candidateManifestRecord.sha256 $apkUrl
            Assert-RemoteReleaseTag
        } `
        -RollbackBeforePolicy {
            Restore-FixedFileIfOwned $previousLegacyPath `
                $RemoteAndroidDirectory 'update-policy.json' `
                $previousLegacyRecord $candidateLegacyRecord
            Restore-FixedFileIfOwned $previousManifestPath `
                $RemoteAndroidDirectory 'manifest.json' `
                $previousManifestRecord $candidateManifestRecord
            Restore-FixedFileIfOwned $previousSitePath `
                $RemoteRootDirectory 'index.html' `
                $previousSiteRecord $candidateSiteRecord
        } `
        -RollbackAfterPolicy {
            # Primeiro restaura o catalogo vinculado pela policy de recuperacao;
            # a nova serial e promovida imediatamente depois. A policy antiga
            # nunca volta ao endpoint publico.
            Restore-FixedFileIfOwned $previousManifestPath `
                $RemoteAndroidDirectory 'manifest.json' `
                $previousManifestRecord $candidateManifestRecord

            $activePolicyPath = Join-Path $tempRoot `
                'policy-active-before-recovery.json'
            Download-FtpsFile $RemoteAndroidDirectory `
                'update-policy-v2.json' $activePolicyPath 60
            $activePolicyRecord = Get-FileRecord $activePolicyPath
            $knownPolicyHashes = @(
                $previousPolicyRecord.sha256,
                $releasePolicyRecord.sha256,
                $recoveryPolicyRecord.sha256)
            if ($activePolicyRecord.sha256 -notin $knownPolicyHashes) {
                throw 'Policy Android mudou fora deste workflow; recusando clobber'
            }
            if ($activePolicyRecord.sha256 -ne $recoveryPolicyRecord.sha256) {
                $null = Replace-FtpsFile $recoveryPolicyPath `
                    $RemoteAndroidDirectory 'update-policy-v2.json' `
                    -ExpectedCurrentRecord $activePolicyRecord
            }
            $recoveryPublic = Join-Path $tempRoot 'policy-recovery-public.json'
            Download-HttpsPostMutationExact `
                "$PublicAndroidBaseUrl/update-policy-v2.json" `
                $recoveryPublic $recoveryPolicyRecord 'policy de recuperacao'
            $activeRecovery = Read-VerifiedAndroidPolicy $recoveryPublic `
                'policy Android de recuperacao publica'
            Assert-AndroidPolicyPayload $activeRecovery ($PolicySerial + 1) `
                $previousLatest $recoveryMinimum $previousVersion `
                $previousManifestRecord.sha256 $previousApkUrl $recoveryMode `
                $recoveryRevokesSerial
            Restore-FixedFileIfOwned $previousLegacyPath `
                $RemoteAndroidDirectory 'update-policy.json' `
                $previousLegacyRecord $candidateLegacyRecord
            Restore-FixedFileIfOwned $previousSitePath `
                $RemoteRootDirectory 'index.html' `
                $previousSiteRecord $candidateSiteRecord
            Download-HttpsPostMutationExact `
                "$PublicAndroidBaseUrl/manifest.json" `
                (Join-Path $tempRoot 'manifest-recovery-public.json') `
                $previousManifestRecord 'manifesto restaurado'
            Download-HttpsPostMutationExact `
                "$PublicAndroidBaseUrl/update-policy.json" `
                (Join-Path $tempRoot 'legacy-recovery-public.json') `
                $previousLegacyRecord 'policy legada restaurada'
            Download-HttpsPostMutationExact $PublicSiteUrl `
                (Join-Path $tempRoot 'site-recovery-public.html') `
                $previousSiteRecord 'site restaurado'
            $script:rollbackConfirmed = $true
        }

    Write-PromotionManifest 'published'
    Write-Host 'PUBLISH_ANDROID_RELEASE_OK'
    Write-Host "APK: $apkName $($apkRecord.sha256)"
    Write-Host "Policy serial: $PolicySerial"
}
catch {
    $failure = $_.Exception.Message
    Add-AuditEvent 'publisher' 'failed' $failure
    try { Write-PromotionManifest 'failed' $failure }
    catch { Write-Warning "Falha ao escrever manifest de auditoria: $_" }
    throw
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
