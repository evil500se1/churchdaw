# Testes leves do pipeline PowerShell. Nao configura nem compila C++.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')

$script:checks = 0
$script:failures = 0

function Test-Case {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] [scriptblock] $Body
    )

    $script:checks++
    try {
        & $Body
        Write-Host "OK  $Name"
    }
    catch {
        $script:failures++
        Write-Host "FAIL $Name - $($_.Exception.Message)" -ForegroundColor Red
    }
}

function Assert-True {
    param([bool] $Condition, [string] $Message)
    if (-not $Condition) { throw $Message }
}

function Assert-Throws {
    param([scriptblock] $Body, [string] $Message)

    $threw = $false
    try { & $Body | Out-Null }
    catch { $threw = $true }
    if (-not $threw) { throw $Message }
}

function Assert-ThrowsMessage {
    param(
        [scriptblock] $Body,
        [string] $ExpectedMessage,
        [string] $Message
    )

    try { & $Body | Out-Null }
    catch {
        if ($_.Exception.Message -cne $ExpectedMessage) {
            throw ("$Message; erro inesperado: $($_.Exception.Message)")
        }
        return
    }
    throw $Message
}

$knownThumbprint = '00112233445566778899AABBCCDDEEFF00112233'

Test-Case 'normaliza thumbprint com separadores' {
    $actual = ConvertTo-ChurchDAWThumbprint `
        '00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff 00 11 22 33'
    Assert-True ($actual -eq $knownThumbprint) 'normalizacao divergente'
}

Test-Case 'recusa thumbprint curto' {
    Assert-Throws {
        ConvertTo-ChurchDAWThumbprint 'AABB'
    } 'thumbprint curto foi aceito'
}

Test-Case 'Commercial recusa ausencia de identidade' {
    Assert-Throws {
        Assert-ChurchDAWWindowsSigningPolicy `
            -ReleaseMode Commercial `
            -HasSigningIdentity $false `
            -ExpectedPublisher 'CN=ChurchDAW' `
            -ExpectedSignerThumbprints $knownThumbprint
    } 'Commercial unsigned foi aceito'
}

Test-Case 'Commercial exige publisher esperado' {
    Assert-Throws {
        Assert-ChurchDAWWindowsSigningPolicy `
            -ReleaseMode Commercial `
            -HasSigningIdentity $true `
            -ExpectedSignerThumbprints $knownThumbprint
    } 'Commercial sem publisher foi aceito'
}

Test-Case 'Commercial exige thumbprint esperado' {
    Assert-Throws {
        Assert-ChurchDAWWindowsSigningPolicy `
            -ReleaseMode Commercial `
            -HasSigningIdentity $true `
            -ExpectedPublisher 'CN=ChurchDAW'
    } 'Commercial sem thumbprint foi aceito'
}

Test-Case 'Commercial recusa SkipTimestamp' {
    Assert-Throws {
        Assert-ChurchDAWWindowsSigningPolicy `
            -ReleaseMode Commercial `
            -HasSigningIdentity $true `
            -SkipTimestamp $true `
            -ExpectedPublisher 'CN=ChurchDAW' `
            -ExpectedSignerThumbprints $knownThumbprint
    } 'Commercial sem timestamp foi aceito'
}

Test-Case 'Test unsigned exige opt-in explicito' {
    Assert-Throws {
        Assert-ChurchDAWWindowsSigningPolicy `
            -ReleaseMode Test `
            -HasSigningIdentity $false
    } 'Test unsigned implicito foi aceito'
}

Test-Case 'Test unsigned explicito e aceito' {
    $policy = Assert-ChurchDAWWindowsSigningPolicy `
        -ReleaseMode Test `
        -HasSigningIdentity $false `
        -AllowUnsignedTestBuild $true
    Assert-True ($policy.ReleaseMode -eq 'Test') 'modo inesperado'
    Assert-True (-not $policy.RequirePublicChain) `
        'teste exigiu cadeia publica'
}

Test-Case 'politica comercial sem instalador recusa por padrao' {
    Assert-Throws {
        Assert-ChurchDAWDesktopPolicyArtifactSet `
            -ReleaseMode Commercial `
            -LatestVersionCode 10400 `
            -MinimumVersionCode 10400 `
            -WindowsInstallerBytes 0 `
            -MacInstallerBytes 0
    } 'politica comercial sem instalador foi aceita implicitamente'
}

Test-Case 'politica comercial corrente pode omitir instalador explicitamente' {
    $kind = Assert-ChurchDAWDesktopPolicyArtifactSet `
        -ReleaseMode Commercial `
        -LatestVersionCode 10400 `
        -MinimumVersionCode 10400 `
        -WindowsInstallerBytes 0 `
        -MacInstallerBytes 0 `
        -AllowCurrentVersionPolicyWithoutInstaller
    Assert-True ($kind -eq 'current-version-policy-only') `
        'classificacao inesperada'
}

Test-Case 'politica sem instalador nao pode anunciar faixa divergente' {
    Assert-Throws {
        Assert-ChurchDAWDesktopPolicyArtifactSet `
            -ReleaseMode Commercial `
            -LatestVersionCode 10500 `
            -MinimumVersionCode 10400 `
            -WindowsInstallerBytes 0 `
            -MacInstallerBytes 0 `
            -AllowCurrentVersionPolicyWithoutInstaller
    } 'politica sem instalador anunciou versao nova'
}

Test-Case 'excecao sem instalador recusa artefato presente' {
    Assert-Throws {
        Assert-ChurchDAWDesktopPolicyArtifactSet `
            -ReleaseMode Commercial `
            -LatestVersionCode 10400 `
            -MinimumVersionCode 10400 `
            -WindowsInstallerBytes 123 `
            -MacInstallerBytes 0 `
            -AllowCurrentVersionPolicyWithoutInstaller
    } 'excecao policy-only aceitou instalador'
}

Test-Case 'rollback comercial pode omitir instaladores explicitamente' {
    $kind = Assert-ChurchDAWDesktopPolicyArtifactSet `
        -ReleaseMode Commercial `
        -LatestVersionCode 10507 `
        -MinimumVersionCode 10400 `
        -WindowsInstallerBytes 0 `
        -MacInstallerBytes 0 `
        -AllowRollbackPolicyWithoutInstaller
    Assert-True ($kind -eq 'rollback-policy-only') `
        'classificacao de rollback inesperada'
}

Test-Case 'rollback sem instalador recusa artefato presente' {
    Assert-Throws {
        Assert-ChurchDAWDesktopPolicyArtifactSet `
            -ReleaseMode Commercial `
            -LatestVersionCode 10507 `
            -MinimumVersionCode 10400 `
            -WindowsInstallerBytes 123 `
            -MacInstallerBytes 0 `
            -AllowRollbackPolicyWithoutInstaller
    } 'rollback policy-only aceitou instalador'
}

Test-Case 'excecoes de policy sem instalador sao mutuamente exclusivas' {
    Assert-Throws {
        Assert-ChurchDAWDesktopPolicyArtifactSet `
            -ReleaseMode Commercial `
            -LatestVersionCode 10400 `
            -MinimumVersionCode 10400 `
            -WindowsInstallerBytes 0 `
            -MacInstallerBytes 0 `
            -AllowCurrentVersionPolicyWithoutInstaller `
            -AllowRollbackPolicyWithoutInstaller
    } 'duas excecoes policy-only foram aceitas juntas'
}

Test-Case 'rollback nao pode usar excecao current-version policy-only' {
    Assert-ThrowsMessage {
        & (Join-Path $PSScriptRoot 'SignDesktopUpdatePolicy.ps1') `
            -PolicySerial 8 `
            -LatestVersionCode 10507 `
            -MinimumVersionCode 10507 `
            -LatestVersionName 1.5.7 `
            -UpdateMessage rollback `
            -Mode rollback `
            -RevokesPolicySerial 7 `
            -AllowCurrentVersionPolicyWithoutInstaller
    } '-AllowCurrentVersionPolicyWithoutInstaller exige -Mode normal' `
        'rollback aceitou a excecao errada de policy-only'
}

Test-Case 'argumentos signtool verify sao fail-closed' {
    $arguments = @(Get-ChurchDAWSignToolVerifyArguments `
        (Join-Path $env:WINDIR 'System32\notepad.exe'))
    $expected = @('verify', '/pa', '/all', '/tw', '/v')
    Assert-True ($arguments.Count -eq 6) 'quantidade de argumentos inesperada'
    for ($index = 0; $index -lt $expected.Count; $index++) {
        Assert-True ($arguments[$index] -ceq $expected[$index]) `
            "argumento $index divergente"
    }
}

Test-Case 'somente Test sem timestamp pode omitir /tw' {
    $arguments = @(Get-ChurchDAWSignToolVerifyArguments `
        -Path (Join-Path $env:WINDIR 'System32\notepad.exe') `
        -RequireTimestamp $false)
    Assert-True ($arguments -contains '/pa') '/pa ausente'
    Assert-True ($arguments -contains '/all') '/all ausente'
    Assert-True ($arguments -contains '/v') '/v ausente'
    Assert-True ($arguments -notcontains '/tw') '/tw nao foi omitido'
}

Test-Case 'falha de launch nativo nunca vira exit code zero' {
    Assert-Throws {
        Invoke-ChurchDAWNativeCapture `
            -FilePath (Join-Path $env:WINDIR `
                'System32\churchdaw-nao-existe.exe')
    } 'executavel inexistente foi tratado como sucesso'
}

Test-Case 'captura exit code real de processo nativo' {
    $result = Invoke-ChurchDAWNativeCapture `
        -FilePath (Join-Path $env:WINDIR 'System32\cmd.exe') `
        -Arguments @('/d', '/c', 'exit', '7')
    Assert-True $result.Launched 'processo valido nao marcou Launched'
    Assert-True ($result.ExitCode -eq 7) 'exit code nativo nao foi preservado'
}

Test-Case 'conversao Unix funciona sem ToUnixTimeSeconds' {
    $epoch = [DateTimeOffset]::new(
        1970, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
    Assert-True (
        (ConvertTo-ChurchDAWUnixTimeSeconds $epoch) -eq 0) `
        'epoch divergente'
    Assert-True (
        (ConvertTo-ChurchDAWUnixTimeSeconds $epoch.AddSeconds(12345)) `
            -eq 12345) 'segundos Unix divergentes'
}

Test-Case 'gate de mesmo signer aceita identidade igual' {
    $first = [pscustomobject] @{
        SignerThumbprint = $knownThumbprint
        Publisher = 'CN=ChurchDAW'
    }
    $second = [pscustomobject] @{
        SignerThumbprint = $knownThumbprint.ToLowerInvariant()
        Publisher = 'cn=churchdaw'
    }
    Assert-ChurchDAWSameSigner $first $second
}

Test-Case 'gate de mesmo signer recusa thumbprint diferente' {
    $first = [pscustomobject] @{
        SignerThumbprint = $knownThumbprint
        Publisher = 'CN=ChurchDAW'
    }
    $second = [pscustomobject] @{
        SignerThumbprint = '11112233445566778899AABBCCDDEEFF00112233'
        Publisher = 'CN=ChurchDAW'
    }
    Assert-Throws {
        Assert-ChurchDAWSameSigner $first $second
    } 'signers diferentes foram aceitos'
}

Test-Case 'le PE x64 do Windows sem executar binario' {
    $systemPe = Join-Path $env:WINDIR 'System32\notepad.exe'
    $pe = Get-ChurchDAWPeInfo $systemPe
    Assert-True $pe.IsX64 "PE do sistema nao foi reconhecido como x64"
    Assert-True ($pe.Machine -eq '0x8664') 'machine inesperada'
}

Test-Case 'registro de arquivo usa SHA-256 e bytes reais' {
    $temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) (
        'churchdaw-pipeline-test-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
    try {
        $fixture = Join-Path $temporaryDirectory 'fixture.bin'
        [IO.File]::WriteAllBytes($fixture, [byte[]] (1, 2, 3, 4))
        $record = Get-ChurchDAWFileRecord $fixture 'fixture'
        Assert-True ($record.bytes -eq 4) 'bytes divergentes'
        Assert-True ($record.sha256 -eq
            '9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a') `
            'SHA-256 divergente'
    }
    finally {
        $resolved = [IO.Path]::GetFullPath($temporaryDirectory)
        $tempRoot = [IO.Path]::GetFullPath(
            [IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if ($resolved.StartsWith(
                $tempRoot,
                [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolved).StartsWith(
                'churchdaw-pipeline-test-',
                [StringComparison]::Ordinal)) {
            Remove-Item -LiteralPath $resolved -Recurse -Force
        }
    }
}

Test-Case 'todos os scripts PowerShell do pipeline fazem parse' {
    foreach ($name in @(
        'WindowsRelease.Common.ps1',
        'SignDesktopInstaller.ps1',
        'SignDesktopUpdatePolicy.ps1',
        'BuildDesktopRelease.ps1',
        'VerifyWindowsReleaseArtifact.ps1',
        'PublishDesktopRelease.ps1',
        'TestWindowsReleasePipeline.ps1'
    )) {
        $tokens = $null
        $errors = $null
        [Management.Automation.Language.Parser]::ParseFile(
            (Join-Path $PSScriptRoot $name),
            [ref] $tokens,
            [ref] $errors) | Out-Null
        if ($errors.Count -ne 0) {
            throw "$name possui $($errors.Count) erros de parse"
        }
    }
}

Test-Case 'publisher limita rede dentro da janela de rollback' {
    $publisher = [IO.File]::ReadAllText(
        (Join-Path $PSScriptRoot 'PublishDesktopRelease.ps1'))
    Assert-True (-not $publisher.Contains("'--retry'")) `
        'publisher reintroduziu retry interno do curl'
    Assert-True ($publisher.Contains(
        '$tagDeadline = [DateTimeOffset]::UtcNow.AddSeconds(30)')) `
        'deadline total da revalidacao da tag ausente'
    Assert-True ($publisher.Contains(
        '$elapsed.TotalMinutes -ge 20.0')) `
        'gate pre-mutacao nao reserva 70 minutos do job'
}

Test-Case 'publisher revalida tag antes da policy e dos RNTOs' {
    $publisher = [IO.File]::ReadAllText(
        (Join-Path $PSScriptRoot 'PublishDesktopRelease.ps1'))
    Assert-True ([regex]::IsMatch(
        $publisher,
        'Assert-RemoteReleaseTag\s+\$policyPromotionAttempted = \$true')) `
        'tag nao e revalidada imediatamente antes da tentativa de policy'
    $renameGates = [regex]::Matches(
        $publisher,
        '-BeforeRename\s+\{\s*Assert-RemoteReleaseTag\s*\}')
    Assert-True ($renameGates.Count -eq 2) `
        'site e policy nao possuem exatamente dois gates pre-RNTO'
}

Test-Case 'Git aceita release em detached HEAD no PowerShell 5.1' {
    $testRepo = Join-Path ([IO.Path]::GetTempPath()) (
        'churchdaw-git-state-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $testRepo | Out-Null
    try {
        & git -C $testRepo init -q -b main
        if ($LASTEXITCODE -ne 0) { throw 'git init falhou' }
        & git -C $testRepo -c user.name=Fixture -c user.email=fixture@example.invalid `
            commit -q --allow-empty -m fixture
        if ($LASTEXITCODE -ne 0) { throw 'git commit de fixture falhou' }
        $named = Get-ChurchDAWGitState $testRepo
        Assert-True ($named.Branch -ceq 'main' -and $named.IsClean) 'branch inicial invalida'
        & git -C $testRepo checkout -q --detach HEAD
        if ($LASTEXITCODE -ne 0) { throw 'git checkout falhou' }
        $detached = Get-ChurchDAWGitState $testRepo
        Assert-True ($detached.Branch -ceq '' -and $detached.IsClean) 'detached HEAD invalido'
        Assert-True ($detached.Commit -ceq $named.Commit) 'commit mudou ao destacar HEAD'
        [IO.File]::WriteAllText((Join-Path $testRepo 'changed.txt'), 'fixture')
        Assert-True (-not (Get-ChurchDAWGitState $testRepo).IsClean) 'arquivo pendente ignorado'
    } finally {
        Remove-Item -LiteralPath $testRepo -Recurse -Force
    }
}

Test-Case 'publicacao exige laudo do Mac ligado ao DMG exato' {
    $root = Join-Path ([IO.Path]::GetTempPath()) ('churchdaw-mac-gate-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $root | Out-Null
    try {
        $dmg = Join-Path $root 'ChurchDAW-1.6.31.dmg'
        $reportPath = Join-Path $root 'verification.json'
        [IO.File]::WriteAllText($dmg, 'verified fixture')
        Assert-Throws { Assert-ChurchDAWMacVerification $dmg $reportPath '1.6.31' } 'laudo ausente aceito'
        $checks = @{}
        foreach ($key in @('developer_id','secure_timestamp','hardened_runtime',
            'app_notarization','app_stapled','app_gatekeeper','dmg_notarization',
            'dmg_stapled','dmg_gatekeeper','legacy_dmg_gatekeeper','mounted_app_gatekeeper','mounted_executable_matches')) { $checks[$key]=$true }
        $report = @{ schema_version=1; product_id='com.churchdaw.app'; version='1.6.31';
            bytes=(Get-Item $dmg).Length; sha256=(Get-FileHash $dmg -Algorithm SHA256).Hash.ToLowerInvariant(); checks=$checks }
        [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4))
        Assert-ChurchDAWMacVerification $dmg $reportPath '1.6.31'
        Assert-Throws { Assert-ChurchDAWMacVerification $dmg $reportPath '1.6.32' } 'laudo de outra versao aceito'
        $checks.dmg_notarization=$false
        [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4))
        Assert-Throws { Assert-ChurchDAWMacVerification $dmg $reportPath '1.6.31' } 'DMG nao notarizado aceito'
        $checks.dmg_notarization=$true
        [IO.File]::WriteAllText($reportPath, ($report | ConvertTo-Json -Depth 4))
        [IO.File]::WriteAllText($dmg, 'tampered fixture')
        Assert-Throws { Assert-ChurchDAWMacVerification $dmg $reportPath '1.6.31' } 'DMG adulterado aceito'
    } finally { Remove-Item -LiteralPath $root -Recurse -Force }
}

Write-Host ''
if ($script:failures -ne 0) {
    throw ("WINDOWS_RELEASE_PIPELINE_TESTS_FAILED: " +
           "$script:failures/$script:checks")
}
Write-Host "WINDOWS_RELEASE_PIPELINE_TESTS_OK: $script:checks/$script:checks" `
    -ForegroundColor Green
