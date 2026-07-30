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

Write-Host ''
if ($script:failures -ne 0) {
    throw ("WINDOWS_RELEASE_PIPELINE_TESTS_FAILED: " +
           "$script:failures/$script:checks")
}
Write-Host "WINDOWS_RELEASE_PIPELINE_TESTS_OK: $script:checks/$script:checks" `
    -ForegroundColor Green
