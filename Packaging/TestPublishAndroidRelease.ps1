[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$publisher = Join-Path $PSScriptRoot 'PublishAndroidRelease.ps1'
if (-not (Test-Path -LiteralPath $publisher -PathType Leaf)) {
    throw "Publicador Android ausente: $publisher"
}

. $publisher -LoadFunctionsOnly

$script:passed = 0
$script:failed = 0
$script:publisherSource = [IO.File]::ReadAllText($publisher)
$script:androidWorkflowPath = Join-Path $PSScriptRoot `
    '..\.github\workflows\release-android-publish.yml'
$script:androidWorkflowSource = [IO.File]::ReadAllText(
    $script:androidWorkflowPath)

function Assert-True {
    param([bool] $Condition, [string] $Message)

    if (-not $Condition) { throw $Message }
}

function Assert-AcceptsGitHubEndpoint {
    param([string] $Endpoint)

    try {
        Assert-GitHubApiEndpoint $Endpoint
    }
    catch {
        throw "Endpoint valido foi recusado '$Endpoint': $($_.Exception.Message)"
    }
}

function Assert-RejectsGitHubEndpoint {
    param([string] $Endpoint)

    $caught = $false
    try {
        Assert-GitHubApiEndpoint $Endpoint
    }
    catch {
        $caught = $_.Exception.Message.Contains(
            'Endpoint GitHub inesperado')
    }

    Assert-True $caught "Endpoint inseguro foi aceito: $Endpoint"
}

function Assert-Sequence {
    param([Collections.Generic.List[string]] $Actual, [string[]] $Expected)

    $actualText = $Actual -join ' > '
    $expectedText = $Expected -join ' > '
    if ($actualText -cne $expectedText) {
        throw "Sequencia inesperada: '$actualText' != '$expectedText'"
    }
}

function Test-Case {
    param([string] $Name, [scriptblock] $Body)

    try {
        & $Body
        ++$script:passed
        Write-Host "PASS $Name"
    }
    catch {
        ++$script:failed
        Write-Host "FAIL $Name - $($_.Exception.Message)"
    }
}

function Invoke-Scenario {
    param(
        [string] $FailurePoint = '',
        [int] $FailTagCall = 0,
        [switch] $RollbackFailure
    )

    $events = [Collections.Generic.List[string]]::new()
    $state = [pscustomobject] @{ TagCalls = 0 }
    $step = {
        param([string] $Name)
        $events.Add($Name)
        if ($FailurePoint -ceq $Name) { throw "failure-$Name" }
    }
    $tag = {
        ++$state.TagCalls
        $events.Add("tag-$($state.TagCalls)")
        if ($FailTagCall -eq $state.TagCalls) {
            throw "tag-moved-$($state.TagCalls)"
        }
    }
    $rollbackBefore = {
        $events.Add('rollback-before')
        if ($RollbackFailure) { throw 'rollback-before-failed' }
    }
    $rollbackAfter = {
        $events.Add('rollback-after')
        if ($RollbackFailure) { throw 'rollback-after-failed' }
    }

    $caught = $null
    try {
        $null = Invoke-AndroidPromotionTransaction `
            -AssertTag $tag `
            -PublishVersionedApk { & $step 'apk' } `
            -VerifyVersionedApk { & $step 'verify-apk' } `
            -PrepareCandidates { & $step 'prepare' } `
            -PromoteSite { & $step 'site' } `
            -PromoteManifest { & $step 'manifest' } `
            -PromoteLegacyPolicy { & $step 'legacy' } `
            -PromoteSignedPolicy { & $step 'policy-v2' } `
            -VerifyFinal { & $step 'verify-final' } `
            -RollbackBeforePolicy $rollbackBefore `
            -RollbackAfterPolicy $rollbackAfter
    }
    catch { $caught = $_.Exception.Message }

    return [pscustomobject] @{
        Events = $events
        Failure = $caught
    }
}

function Invoke-VersionedCollisionProbe {
    param([bool] $Identical)

    $ownedName = 'churchdaw-android-publisher-test-' +
        [Guid]::NewGuid().ToString('N')
    $ownedRoot = Join-Path ([IO.Path]::GetTempPath()) $ownedName
    New-Item -ItemType Directory -Path $ownedRoot | Out-Null
    try {
        $candidate = Join-Path $ownedRoot 'ChurchDAW-1.5.8-signed.apk'
        $existing = Join-Path $ownedRoot 'existing.apk'
        [IO.File]::WriteAllText($candidate, 'candidate-apk')
        [IO.File]::WriteAllText(
            $existing,
            $(if ($Identical) { 'candidate-apk' } else { 'different-apk' }))

        $script:tempRoot = $ownedRoot
        $script:runToken = '100-1'
        $script:uploadedVersioned = [Collections.Generic.List[string]]::new()
        $script:auditEvents = [Collections.Generic.List[object]]::new()
        $script:collisionExisting = $existing
        $script:unexpectedUpload = $false

        function Test-FtpsFileExists { return $true }
        function Download-FtpsFile {
            param($RemoteDirectory, $Name, $Output, $MaxTimeSeconds = 900)
            Copy-Item -LiteralPath $script:collisionExisting -Destination $Output
        }
        function Upload-FtpsFile { $script:unexpectedUpload = $true }

        $failure = $null
        try {
            $null = Publish-VersionedFile $candidate '/public_html/app/android'
        }
        catch { $failure = $_.Exception.Message }
        return [pscustomobject] @{
            Failure = $failure
            UnexpectedUpload = $script:unexpectedUpload
            UploadedCount = $script:uploadedVersioned.Count
        }
    }
    finally {
        $resolved = [IO.Path]::GetFullPath($ownedRoot).TrimEnd('\') + '\'
        $base = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith(
                $base,
                [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($ownedRoot) -cne $ownedName) {
            throw 'Recusando limpar diretorio de teste sem prova de posse'
        }
        Remove-Item -LiteralPath $ownedRoot -Recurse -Force
    }
}

function Invoke-ReplaceOrderingProbe {
    $ownedName = 'churchdaw-android-replace-test-' +
        [Guid]::NewGuid().ToString('N')
    $ownedRoot = Join-Path ([IO.Path]::GetTempPath()) $ownedName
    New-Item -ItemType Directory -Path $ownedRoot | Out-Null
    try {
        $candidate = Join-Path $ownedRoot 'manifest.release.json'
        [IO.File]::WriteAllText($candidate, '{"candidate":true}')
        $script:tempRoot = $ownedRoot
        $script:runToken = '101-1'
        $script:replaceEvents = [Collections.Generic.List[string]]::new()

        function Assert-SafeRemoteName {}
        function Get-FileRecord {
            param($Path)
            return [pscustomobject] @{
                name = [IO.Path]::GetFileName($Path)
                path = $Path
                bytes = 18
                sha256 = ('a' * 64)
            }
        }
        function Test-FtpsFileExists { return $false }
        function Upload-FtpsFile {
            $script:replaceEvents.Add('upload')
        }
        function Download-FtpsFile {
            param($RemoteDirectory, $Name, $Output, $MaxTimeSeconds = 900)
            $kind = if ($Output.Contains('roundtrip-fixed-')) {
                'download-roundtrip'
            }
            else {
                'download-final'
            }
            $script:replaceEvents.Add($kind)
            [IO.File]::WriteAllText($Output, 'downloaded')
        }
        function Assert-FileRecord {
            $script:replaceEvents.Add('assert-file')
        }
        function Assert-FtpsCurrentRecord {
            param($RemoteDirectory, $Name, $Expected, $Label,
                  $MaxTimeSeconds = 900)
            $script:replaceEvents.Add("assert-current:$Label")
        }
        function Rename-FtpsFile {
            $script:replaceEvents.Add('rename-rnfr-rnto')
        }
        function Remove-FtpsFile {
            $script:replaceEvents.Add('remove-temp')
        }
        function Add-AuditEvent {
            $script:replaceEvents.Add('audit')
        }

        $beforeRename = { $script:replaceEvents.Add('before-rename') }
        $null = Replace-FtpsFile $candidate '/public_html/app/android' `
            'manifest.json' -ExpectedCurrentRecord ([pscustomobject] @{
                bytes = 17
                sha256 = ('b' * 64)
            }) -BeforeRename $beforeRename
        return [pscustomobject] @{ Events = @($script:replaceEvents) }
    }
    finally {
        $resolved = [IO.Path]::GetFullPath($ownedRoot).TrimEnd('\') + '\'
        $base = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith(
                $base,
                [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($ownedRoot) -cne $ownedName) {
            throw 'Recusando limpar diretorio de replace sem prova de posse'
        }
        Remove-Item -LiteralPath $ownedRoot -Recurse -Force
    }
}

function Invoke-AndroidCandidateSiteProbe {
    param([switch] $RemoveOneAndroidUrl)

    $siteSource = Join-Path $PSScriptRoot 'SiteProduction\index.html'
    $jucerPath = Join-Path $PSScriptRoot '..\ChurchDAW.jucer'
    [xml] $jucer = [IO.File]::ReadAllText($jucerPath)
    $releaseVersion = [string] $jucer.JUCERPROJECT.version
    $siteText = [IO.File]::ReadAllText($siteSource, $script:utf8)
    $urlMatches = @([regex]::Matches(
        $siteText,
        'https://churchdaw\.com/app/android/ChurchDAW-(?<version>[0-9]+\.[0-9]+\.[0-9]+)-signed\.apk') |
        ForEach-Object { $_.Value } | Sort-Object -Unique)
    if ($urlMatches.Count -ne 1) {
        throw 'Fixture real precisa conter uma unica URL Android anterior'
    }
    $previousUrl = [string] $urlMatches[0]
    $versionMatch = [regex]::Match(
        $previousUrl,
        'ChurchDAW-(?<version>[0-9]+\.[0-9]+\.[0-9]+)-signed\.apk$')
    if (-not $versionMatch.Success) {
        throw 'Fixture real nao revelou a versao Android anterior'
    }
    $previousVersion = [string] $versionMatch.Groups['version'].Value
    $releaseUrl =
        "https://churchdaw.com/app/android/ChurchDAW-$releaseVersion-signed.apk"

    $ownedName = 'churchdaw-android-site-test-' +
        [Guid]::NewGuid().ToString('N')
    $ownedRoot = Join-Path ([IO.Path]::GetTempPath()) $ownedName
    New-Item -ItemType Directory -Path $ownedRoot | Out-Null
    $sourceCopy = Join-Path $ownedRoot 'index-before.html'
    $candidate = Join-Path $ownedRoot 'index-release.html'
    Copy-Item -LiteralPath $siteSource -Destination $sourceCopy
    if ($RemoveOneAndroidUrl) {
        $tampered = [IO.File]::ReadAllText($sourceCopy, $script:utf8)
        $offset = $tampered.IndexOf($previousUrl, [StringComparison]::Ordinal)
        if ($offset -lt 0) { throw 'URL para adulteracao nao encontrada' }
        $tampered = $tampered.Remove($offset, $previousUrl.Length).
            Insert($offset, 'about:blank')
        [IO.File]::WriteAllText($sourceCopy, $tampered, $script:writeUtf8)
    }
    $sourceBefore = Get-FileRecord $sourceCopy

    try {
        $failure = $null
        try {
            New-AndroidCandidateSite `
                -Source $sourceCopy `
                -Destination $candidate `
                -PreviousVersion $previousVersion `
                -ReleaseVersion $releaseVersion `
                -PreviousApkUrl $previousUrl `
                -ReleaseApkUrl $releaseUrl
        }
        catch { $failure = $_.Exception.Message }

        $sourceAfter = Get-FileRecord $sourceCopy
        $candidateText = if (Test-Path -LiteralPath $candidate) {
            [IO.File]::ReadAllText($candidate, $script:utf8)
        }
        else { '' }
        return [pscustomobject] @{
            Failure = $failure
            SourceUnchanged =
                ($sourceBefore.bytes -eq $sourceAfter.bytes -and
                 $sourceBefore.sha256 -ceq $sourceAfter.sha256)
            CandidateExists = (Test-Path -LiteralPath $candidate)
            CandidateText = $candidateText
            PreviousVersion = $previousVersion
            ReleaseVersion = $releaseVersion
            PreviousUrl = $previousUrl
            ReleaseUrl = $releaseUrl
        }
    }
    finally {
        $resolved = [IO.Path]::GetFullPath($ownedRoot).TrimEnd('\') + '\'
        $base = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $resolved.StartsWith(
                $base,
                [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($ownedRoot) -cne $ownedName) {
            throw 'Recusando limpar fixture do site sem prova de posse'
        }
        Remove-Item -LiteralPath $ownedRoot -Recurse -Force
    }
}

Test-Case 'sucesso promove policy v2 por ultimo' {
    $result = Invoke-Scenario
    Assert-True ($null -eq $result.Failure) 'cenario de sucesso falhou'
    Assert-Sequence $result.Events @(
        'tag-1', 'apk', 'verify-apk', 'prepare',
        'tag-2', 'site', 'tag-3', 'manifest',
        'tag-4', 'legacy', 'tag-5', 'policy-v2', 'verify-final')
}

Test-Case 'colisao de APK imutavel para antes de mutacao fixa' {
    $result = Invoke-Scenario -FailurePoint 'apk'
    Assert-True ($result.Failure -ceq 'failure-apk') `
        'falha original da colisao nao foi preservada'
    Assert-Sequence $result.Events @('tag-1', 'apk')
}

Test-Case 'falha de verificacao publica do APK nao aciona rollback' {
    $result = Invoke-Scenario -FailurePoint 'verify-apk'
    Assert-True ($result.Failure -ceq 'failure-verify-apk') `
        'falha de verificacao nao foi preservada'
    Assert-Sequence $result.Events @('tag-1', 'apk', 'verify-apk')
}

Test-Case 'falha antes da policy restaura mutacoes fixas' {
    $result = Invoke-Scenario -FailurePoint 'manifest'
    Assert-True ($result.Failure -ceq 'failure-manifest') `
        'falha pre-policy nao foi preservada'
    Assert-Sequence $result.Events @(
        'tag-1', 'apk', 'verify-apk', 'prepare',
        'tag-2', 'site', 'tag-3', 'manifest', 'rollback-before')
}

Test-Case 'tag movida antes da policy recusa e restaura' {
    $result = Invoke-Scenario -FailTagCall 5
    Assert-True ($result.Failure -ceq 'tag-moved-5') `
        'mudanca de tag nao foi preservada'
    Assert-Sequence $result.Events @(
        'tag-1', 'apk', 'verify-apk', 'prepare',
        'tag-2', 'site', 'tag-3', 'manifest',
        'tag-4', 'legacy', 'tag-5', 'rollback-before')
}

Test-Case 'falha durante policy usa recovery monotonicamente nova' {
    $result = Invoke-Scenario -FailurePoint 'policy-v2'
    Assert-True ($result.Failure -ceq 'failure-policy-v2') `
        'falha durante policy nao foi preservada'
    Assert-True ($result.Events[$result.Events.Count - 1] -ceq 'rollback-after') `
        'recovery pos-policy nao foi acionada'
    Assert-True (-not $result.Events.Contains('rollback-before')) `
        'rollback antigo foi usado depois da policy'
}

Test-Case 'falha de verificacao final usa recovery monotonicamente nova' {
    $result = Invoke-Scenario -FailurePoint 'verify-final'
    Assert-True ($result.Failure -ceq 'failure-verify-final') `
        'falha final nao foi preservada'
    Assert-True ($result.Events[$result.Events.Count - 1] -ceq 'rollback-after') `
        'recovery pos-policy nao foi acionada'
}

Test-Case 'falha de rollback reporta causa original e rollback' {
    $result = Invoke-Scenario -FailurePoint 'manifest' -RollbackFailure
    Assert-True ($result.Failure.Contains('failure-manifest')) `
        'causa original sumiu'
    Assert-True ($result.Failure.Contains('ROLLBACK FALHOU')) `
        'falha de rollback nao foi destacada'
    Assert-True ($result.Failure.Contains('rollback-before-failed')) `
        'causa do rollback sumiu'
}

Test-Case 'payload normal exige revogacao zero' {
    $payload = [pscustomobject] @{
        package_id = 'com.churchdaw.app'
        mode = 'normal'
        latest_version_name = '1.5.8'
        catalog_sha256 = ('a' * 64)
        play_store_url =
            'https://churchdaw.com/app/android/ChurchDAW-1.5.8-signed.apk'
        policy_serial = 23
        latest_version_code = 14
        minimum_version_code = 9
        revokes_policy_serial = 0
    }
    Assert-AndroidPolicyPayload $payload 23 14 9 '1.5.8' ('a' * 64) `
        $payload.play_store_url
    $payload.revokes_policy_serial = 22
    $caught = $false
    try {
        Assert-AndroidPolicyPayload $payload 23 14 9 '1.5.8' ('a' * 64) `
            $payload.play_store_url
    }
    catch { $caught = $true }
    Assert-True $caught 'policy normal aceitou revogacao inesperada'
}

Test-Case 'payload rollback vincula serial revogada' {
    $payload = [pscustomobject] @{
        package_id = 'com.churchdaw.app'
        mode = 'rollback'
        latest_version_name = '1.5.7'
        catalog_sha256 = ('b' * 64)
        play_store_url =
            'https://churchdaw.com/app/android/ChurchDAW-1.5.7-signed.apk'
        policy_serial = 24
        latest_version_code = 13
        minimum_version_code = 9
        revokes_policy_serial = 23
    }
    Assert-AndroidPolicyPayload $payload 24 13 9 '1.5.7' ('b' * 64) `
        $payload.play_store_url 'rollback' 23
}

Test-Case 'recovery sem elevar piso usa policy normal com serial nova' {
    $plan = Get-AndroidRecoveryPolicyPlan 9 9
    Assert-True ([string] $plan.Mode -ceq 'normal') `
        'recovery sem elevacao nao ficou normal'
    Assert-True ([int] $plan.MinimumVersionCode -eq 9) `
        'recovery sem elevacao mudou o piso'
    Assert-True ([int] $plan.RevokesPolicySerial -eq 0) `
        'recovery normal revogou serial'
}

Test-Case 'promocao recusa elevar piso sem gate posterior' {
    $caught = $false
    try { $null = Get-AndroidRecoveryPolicyPlan 14 9 }
    catch {
        $caught = $_.Exception.Message.Contains(
            'preservar minimum_version_code')
    }
    Assert-True $caught `
        'publicador aceitou piso sem recovery universal'
}

Test-Case 'site real suporta fase desktop seguida da fase Android' {
    $result = Invoke-AndroidCandidateSiteProbe
    Assert-True ($null -eq $result.Failure) `
        "geracao do site candidato falhou: $($result.Failure)"
    Assert-True $result.SourceUnchanged `
        'gerador alterou o site anterior comprovado'
    Assert-True $result.CandidateExists 'site candidato nao foi criado'
    Assert-True ($result.PreviousVersion -cne $result.ReleaseVersion) `
        'fixture deixou de representar a fase desktop com Android anterior'
    Assert-True (-not $result.CandidateText.Contains($result.PreviousUrl)) `
        'site candidato preservou URL Android anterior'
    $releaseUrlCount = [regex]::Matches(
        $result.CandidateText,
        [regex]::Escape($result.ReleaseUrl)).Count
    Assert-True ($releaseUrlCount -eq 3) `
        "site candidato tem $releaseUrlCount URLs Android, esperado 3"
    foreach ($label in @(
        "Baixar ChurchDAW $($result.ReleaseVersion) para Android em formato APK",
        "APK $($result.ReleaseVersion)",
        "Baixar APK $($result.ReleaseVersion)"
    )) {
        Assert-True $result.CandidateText.Contains($label) `
            "site candidato sem rotulo Android: $label"
    }
    Assert-True $result.CandidateText.Contains(
        "ChurchDAW-$($result.ReleaseVersion)-Setup.exe") `
        'site da fase desktop nao fixa o EXE da mesma release'
    Assert-True $result.CandidateText.Contains(
        "ChurchDAW-$($result.ReleaseVersion)-macOS.dmg") `
        'site da fase desktop nao fixa o DMG da mesma release'
}

Test-Case 'gerador recusa deriva quando formato Android muda' {
    $result = Invoke-AndroidCandidateSiteProbe -RemoveOneAndroidUrl
    Assert-True ($null -ne $result.Failure -and
        $result.Failure.Contains('contagem inesperada')) `
        'gerador aceitou site anterior com uma URL Android ausente'
    Assert-True $result.SourceUnchanged `
        'falha do gerador alterou o site anterior'
    Assert-True (-not $result.CandidateExists) `
        'falha do gerador deixou candidato parcial'
}

Test-Case 'publicador deriva e promove somente o site temporario auditado' {
    $proofIndex = $script:publisherSource.IndexOf(
        'Assert-FileRecord $SiteIndex $previousSiteRecord',
        [StringComparison]::Ordinal)
    $deriveIndex = $script:publisherSource.IndexOf(
        '-Source $previousSitePath',
        [StringComparison]::Ordinal)
    $promoteIndex = $script:publisherSource.IndexOf(
        'Replace-FtpsFile $candidateSitePath $RemoteRootDirectory',
        [StringComparison]::Ordinal)
    Assert-True ($proofIndex -ge 0 -and $deriveIndex -gt $proofIndex) `
        'site nao e derivado depois de provar tag igual ao remoto anterior'
    Assert-True ($promoteIndex -gt $deriveIndex) `
        'publicador nao promove o site temporario derivado'
    Assert-True (-not $script:publisherSource.Contains(
        'Replace-FtpsFile $SiteIndex $RemoteRootDirectory')) `
        'publicador ainda promove diretamente o HTML imutavel do tag'
}

Test-Case 'arquivo versionado remoto identico e reutilizado sem upload' {
    $result = Invoke-VersionedCollisionProbe $true
    Assert-True ($null -eq $result.Failure) `
        "arquivo identico foi recusado: $($result.Failure)"
    Assert-True (-not $result.UnexpectedUpload) `
        'arquivo identico iniciou upload desnecessario'
    Assert-True ($result.UploadedCount -eq 0) `
        'arquivo remoto reutilizado foi marcado como novo upload'
}

Test-Case 'arquivo versionado remoto diferente bloqueia colisao' {
    $result = Invoke-VersionedCollisionProbe $false
    Assert-True ($null -ne $result.Failure -and
        $result.Failure.Contains('colisao remota')) `
        'colisao diferente nao foi recusada'
    Assert-True (-not $result.UnexpectedUpload) `
        'colisao diferente iniciou upload'
}

Test-Case 'versionCode candidato precisa ser estritamente maior' {
    $gate = 'if ($versionCode -le $previousLatest)'
    $gateIndex = $script:publisherSource.IndexOf(
        $gate,
        [StringComparison]::Ordinal)
    $catalogMutationIndex = $script:publisherSource.IndexOf(
        '$previousManifest.latest_version_code = $versionCode',
        [StringComparison]::Ordinal)
    Assert-True ($gateIndex -ge 0) `
        'gate versionCode <= publicado ausente'
    Assert-True ($catalogMutationIndex -gt $gateIndex) `
        'catalogo pode ser alterado antes do gate versionCode'
    Assert-True ((14 -le 14) -and (13 -le 14) -and -not (15 -le 14)) `
        'operador do gate nao representa crescimento estrito'
}

Test-Case 'replace revalida estado depois do callback e antes do RNTO' {
    $result = Invoke-ReplaceOrderingProbe
    $currentIndexes = @()
    for ($i = 0; $i -lt $result.Events.Count; ++$i) {
        if ($result.Events[$i].StartsWith(
                'assert-current:',
                [StringComparison]::Ordinal)) {
            $currentIndexes += $i
        }
    }
    Assert-True ($currentIndexes.Count -eq 2) `
        'Replace-FtpsFile nao fez exatamente duas revalidacoes'
    $beforeIndex = [Array]::IndexOf($result.Events, 'before-rename')
    $renameIndex = [Array]::IndexOf($result.Events, 'rename-rnfr-rnto')
    Assert-True ($currentIndexes[0] -lt $beforeIndex) `
        'primeira revalidacao nao ocorreu antes de BeforeRename'
    Assert-True ($currentIndexes[1] -eq ($beforeIndex + 1)) `
        'segunda revalidacao nao ocorreu logo depois de BeforeRename'
    Assert-True ($renameIndex -eq ($currentIndexes[1] + 1)) `
        'RNFR/RNTO nao ocorreu imediatamente depois da segunda revalidacao'
}

Test-Case 'rollbackConfirmed exige restauracoes e HTTPS finais' {
    $start = $script:publisherSource.IndexOf(
        '-RollbackAfterPolicy {',
        [StringComparison]::Ordinal)
    $end = $script:publisherSource.IndexOf(
        "`n        }`n`n    Write-PromotionManifest",
        $start,
        [StringComparison]::Ordinal)
    Assert-True ($start -ge 0 -and $end -gt $start) `
        'bloco RollbackAfterPolicy nao foi isolado'
    $block = $script:publisherSource.Substring($start, $end - $start)
    $confirmed = '$script:rollbackConfirmed = $true'
    $confirmedIndex = $block.IndexOf(
        $confirmed,
        [StringComparison]::Ordinal)
    Assert-True ($confirmedIndex -ge 0) `
        'rollbackConfirmed nunca e marcado'
    Assert-True ($block.IndexOf(
        $confirmed,
        $confirmedIndex + $confirmed.Length,
        [StringComparison]::Ordinal) -lt 0) `
        'rollbackConfirmed e marcado mais de uma vez'
    foreach ($required in @(
        'Restore-FixedFileIfOwned $previousManifestPath',
        'Restore-FixedFileIfOwned $previousLegacyPath',
        'Restore-FixedFileIfOwned $previousSitePath',
        "'policy de recuperacao'",
        "'manifesto restaurado'",
        "'policy legada restaurada'",
        "'site restaurado'"
    )) {
        $requiredIndex = $block.IndexOf(
            $required,
            [StringComparison]::Ordinal)
        Assert-True ($requiredIndex -ge 0 -and
            $requiredIndex -lt $confirmedIndex) `
            "rollbackConfirmed antecede etapa obrigatoria: $required"
    }
}

Test-Case 'Android e desktop compartilham um unico escritor global' {
    $androidWorkflow = [IO.File]::ReadAllText(
        (Join-Path $PSScriptRoot '..\.github\workflows\release-android-publish.yml'))
    $desktopWorkflow = [IO.File]::ReadAllText(
        (Join-Path $PSScriptRoot '..\.github\workflows\release-desktop-publish.yml'))
    $pattern = '(?ms)^concurrency:\s*\r?\n\s+group:\s*([^\r\n]+)'
    $androidMatch = [regex]::Match($androidWorkflow, $pattern)
    $desktopMatch = [regex]::Match($desktopWorkflow, $pattern)
    Assert-True ($androidMatch.Success -and $desktopMatch.Success) `
        'grupo de concorrencia ausente'
    Assert-True ($androidMatch.Groups[1].Value.Trim() -ceq
        $desktopMatch.Groups[1].Value.Trim()) `
        'publicadores usam escritores diferentes'
    Assert-True ($androidMatch.Groups[1].Value.Trim() -ceq
        'desktop-production-publish') `
        'grupo global inesperado'
}

Test-Case 'workflow comercial fixa todas as actions por commit' {
    $workflow = $script:androidWorkflowSource
    $uses = @([regex]::Matches($workflow, '(?m)^\s+uses:\s+([^\s#]+)') |
        ForEach-Object { $_.Groups[1].Value })
    Assert-True ($uses.Count -ge 5) 'workflow tem actions insuficientes'
    foreach ($use in $uses) {
        Assert-True ($use -match '@[0-9a-f]{40}$') `
            "action sem pin imutavel: $use"
    }
}

Test-Case 'workflow publica no environment e chave desktop protegidos' {
    Assert-True ($script:androidWorkflowSource -match
        '(?m)^\s{4}environment:\s+desktop-production\s*$') `
        'job de publicacao nao usa desktop-production'
    Assert-True ($script:androidWorkflowSource.Contains(
        'DESKTOP_UPDATE_PRIVATE_KEY_BASE64: ' +
        '${{ secrets.DESKTOP_UPDATE_PRIVATE_KEY_BASE64 }}')) `
        'workflow nao le a chave protegida desktop'
    Assert-True ($script:androidWorkflowSource.Contains(
        '$env:DESKTOP_UPDATE_PRIVATE_KEY_BASE64')) `
        'workflow nao consome a chave protegida desktop'
    Assert-True (-not $script:androidWorkflowSource.Contains(
        'ANDROID_UPDATE_PRIVATE_KEY_BASE64')) `
        'workflow ainda referencia o secret Android obsoleto'
}

Test-Case 'whitelist GitHub aceita endpoints operacionais validos' {
    $shaA = 'a' * 40
    $shaB = 'B' * 40
    foreach ($endpoint in @(
        'repos/evil500se1/churchdaw',
        'repos/evil500se1/churchdaw/git/ref/tags/v1.5.8',
        "repos/evil500se1/churchdaw/git/tags/$shaA",
        'repos/evil500se1/churchdaw/git/ref/heads/main',
        'repos/evil500se1/churchdaw/git/ref/heads/release%2Fstable',
        "repos/evil500se1/churchdaw/compare/$shaA...$shaB"
    )) {
        Assert-AcceptsGitHubEndpoint $endpoint
    }
}

Test-Case 'whitelist GitHub rejeita bypasses e endpoints fora do escopo' {
    $sha = 'a' * 40
    foreach ($endpoint in @(
        'repos/evil500se1/churchdaw/issues',
        'repos/evil500se1/churchdaw?per_page=100',
        'repos/evil500se1/churchdaw/git/ref/heads/main?raw=1',
        'repos/evil500se1/churchdaw/git/ref/heads/release%',
        'repos/evil500se1/churchdaw/git/ref/heads/release%2',
        'repos/evil500se1/churchdaw/git/ref/heads/../main',
        'repos/evil500se1/churchdaw/git/ref/heads/%2E%2E%2Fmain',
        'repos/evil500se1/churchdaw/git/ref/heads/main;issues',
        "repos/evil500se1/churchdaw/compare/main...$sha",
        "repos/evil500se1/churchdaw/compare/$sha...main"
    )) {
        Assert-RejectsGitHubEndpoint $endpoint
    }
}

Test-Case 'compare identical realista dispensa head_commit inexistente' {
    $sha = 'a' * 40
    $comparison = [pscustomobject] @{
        status = 'identical'
        ahead_by = 0
        behind_by = 0
        total_commits = 0
        base_commit = [pscustomobject] @{ sha = $sha }
        merge_base_commit = [pscustomobject] @{ sha = $sha }
        commits = @()
    }
    Assert-GitHubComparisonContainsSourceCommit $comparison $sha $sha
}

Test-Case 'compare ahead realista vincula ultimo commit ao head remoto' {
    $base = 'a' * 40
    $head = 'b' * 40
    $comparison = [pscustomobject] @{
        status = 'ahead'
        ahead_by = 1
        behind_by = 0
        total_commits = 1
        base_commit = [pscustomobject] @{ sha = $base }
        merge_base_commit = [pscustomobject] @{ sha = $base }
        commits = @([pscustomobject] @{ sha = $head })
    }
    Assert-GitHubComparisonContainsSourceCommit $comparison $base $head
}

Test-Case 'compare ahead recusa ultimo commit diferente do head remoto' {
    $base = 'a' * 40
    $head = 'b' * 40
    $comparison = [pscustomobject] @{
        status = 'ahead'
        ahead_by = 1
        behind_by = 0
        total_commits = 1
        base_commit = [pscustomobject] @{ sha = $base }
        merge_base_commit = [pscustomobject] @{ sha = $base }
        commits = @([pscustomobject] @{ sha = ('c' * 40) })
    }
    $caught = $false
    try {
        Assert-GitHubComparisonContainsSourceCommit $comparison $base $head
    }
    catch { $caught = $true }
    Assert-True $caught 'compare ahead aceitou head remoto diferente'
}

if ($script:failed -ne 0) {
    throw "TestPublishAndroidRelease falhou: $script:failed caso(s)"
}

Write-Host "TEST_PUBLISH_ANDROID_RELEASE_OK $script:passed/$script:passed"
