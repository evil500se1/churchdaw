param(
    [string] $PrivateKey = ''
)

$ErrorActionPreference = 'Stop'
$signer = Join-Path $PSScriptRoot 'SignAndroidUpdatePolicy.ps1'
$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'

if ([string]::IsNullOrWhiteSpace($PrivateKey)) {
    $PrivateKey = Join-Path $env:USERPROFILE (
        'ChurchDAW-Secrets\churchdaw-update-policy-private.pem'
    )
}

if (-not (Test-Path -LiteralPath $signer -PathType Leaf)) {
    throw "Assinador nao encontrado: $signer"
}

if (-not (Test-Path -LiteralPath $PrivateKey -PathType Leaf)) {
    throw "Chave privada de teste nao encontrada: $PrivateKey"
}

if (-not (Test-Path -LiteralPath $openssl -PathType Leaf)) {
    throw "OpenSSL de teste nao encontrado: $openssl"
}

$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-policy-tests-' + [Guid]::NewGuid().ToString('N')
)
$seedOutput = Join-Path $temporaryDirectory 'seed.json'
$catalogManifest = Join-Path $temporaryDirectory 'manifest-fixture.json'
$catalogFixtureV1 =
    '{"schema_version":1,"libraries":[{"id":"fixture-a"}]}'
$catalogFixtureV2 =
    '{"schema_version":1,"libraries":[{"id":"fixture-a"},{"id":"fixture-b"}]}'
$utf8 = [Text.UTF8Encoding]::new($false)
$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$passed = [Collections.Generic.List[string]]::new()

function New-SignArguments {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Output,
        [int] $PolicySerial = 100,
        [int] $MinimumVersionCode = 5,
        [ValidateSet('normal', 'rollback')]
        [string] $Mode = 'normal',
        [int] $RevokesPolicySerial = 0
    )

    @{
        PolicySerial = $PolicySerial
        LatestVersionCode = 5
        MinimumVersionCode = $MinimumVersionCode
        LatestVersionName = '5.0.0-test'
        UpdateMessage = 'Teste automatizado local'
        Mode = $Mode
        RevokesPolicySerial = $RevokesPolicySerial
        ValidDays = 1
        PrivateKey = $PrivateKey
        CatalogManifest = $catalogManifest
        Output = $Output
    }
}

function Get-SignedPayload {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $envelope = (
        [IO.File]::ReadAllText($Path, $strictUtf8) |
            ConvertFrom-Json
    )
    $payloadBytes = [Convert]::FromBase64String(
        [string] $envelope.payload
    )
    return (
        $strictUtf8.GetString($payloadBytes) |
            ConvertFrom-Json
    )
}

function Convert-ToSignedLegacyPolicy {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $envelope = (
        [IO.File]::ReadAllText($Path, $strictUtf8) |
            ConvertFrom-Json
    )
    $payload = Get-SignedPayload -Path $Path
    $payload.PSObject.Properties.Remove('catalog_sha256')
    if (@($payload.PSObject.Properties).Count -ne 12) {
        throw 'Fixture legado nao ficou com exatamente 12 campos'
    }

    $legacyPayloadText = $payload | ConvertTo-Json -Compress
    $suffix = [Guid]::NewGuid().ToString('N')
    $legacyPayloadFile = Join-Path $temporaryDirectory (
        "legacy-payload-$suffix.json"
    )
    $legacySignatureFile = Join-Path $temporaryDirectory (
        "legacy-payload-$suffix.sig"
    )
    try {
        [IO.File]::WriteAllText(
            $legacyPayloadFile,
            $legacyPayloadText,
            $utf8
        )
        & $openssl dgst -sha256 `
            -sign $PrivateKey `
            -out $legacySignatureFile `
            $legacyPayloadFile
        if ($LASTEXITCODE -ne 0) {
            throw 'OpenSSL nao assinou a fixture legada'
        }

        $legacyPayloadBytes = [IO.File]::ReadAllBytes($legacyPayloadFile)
        $legacySignatureBytes = [IO.File]::ReadAllBytes($legacySignatureFile)
        if ($legacySignatureBytes.Length -ne 384) {
            throw 'Fixture legada nao recebeu assinatura RSA-3072'
        }

        $envelope.payload = [Convert]::ToBase64String(
            $legacyPayloadBytes
        )
        $envelope.signature = [Convert]::ToBase64String(
            $legacySignatureBytes
        )
        [IO.File]::WriteAllText(
            $Path,
            ($envelope | ConvertTo-Json -Compress),
            $utf8
        )
    }
    finally {
        if (Test-Path -LiteralPath $legacyPayloadFile -PathType Leaf) {
            Remove-Item -LiteralPath $legacyPayloadFile -Force
        }
        if (Test-Path -LiteralPath $legacySignatureFile -PathType Leaf) {
            Remove-Item -LiteralPath $legacySignatureFile -Force
        }
    }
}

function Invoke-ExpectedSuccess {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable] $Arguments,
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    & $signer @Arguments | Out-Null
    if (-not (Test-Path -LiteralPath $Arguments.Output -PathType Leaf)) {
        throw "Teste '$Name' nao produziu a politica esperada"
    }

    $script:passed.Add($Name)
}

function Invoke-ExpectedFailure {
    param(
        [Parameter(Mandatory = $true)]
        [hashtable] $Arguments,
        [Parameter(Mandatory = $true)]
        [string] $ExpectedMessage,
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $beforeHash = $null
    if (Test-Path -LiteralPath $Arguments.Output -PathType Leaf) {
        $beforeHash = (
            Get-FileHash -LiteralPath $Arguments.Output -Algorithm SHA256
        ).Hash
    }

    $sentinel = "TESTE_NAO_FALHOU_$Name"
    try {
        & $signer @Arguments | Out-Null
        throw $sentinel
    }
    catch {
        if ($_.Exception.Message -eq $sentinel) {
            throw "Teste '$Name' deveria ter falhado"
        }
        if ($_.Exception.Message -notmatch $ExpectedMessage) {
            throw (
                "Teste '$Name' falhou pelo motivo inesperado: " +
                $_.Exception.Message
            )
        }
    }

    if ($null -ne $beforeHash) {
        $afterHash = (
            Get-FileHash -LiteralPath $Arguments.Output -Algorithm SHA256
        ).Hash
        if ($afterHash -cne $beforeHash) {
            throw "Teste '$Name' alterou o envelope rejeitado"
        }
    }

    $script:passed.Add($Name)
}

function Copy-Seed {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $path = Join-Path $temporaryDirectory "$Name.json"
    Copy-Item -LiteralPath $seedOutput -Destination $path
    return $path
}

New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
try {
    [IO.File]::WriteAllText($catalogManifest, $catalogFixtureV1, $utf8)

    Invoke-ExpectedSuccess `
        -Arguments (New-SignArguments -Output $seedOutput) `
        -Name 'primeira assinatura sem politica anterior'

    $expectedCatalogHash = (
        Get-FileHash -LiteralPath $catalogManifest -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    $seedPayload = Get-SignedPayload -Path $seedOutput
    $seedPayloadFieldCount = @(
        $seedPayload.PSObject.Properties
    ).Count
    if ($seedPayloadFieldCount -ne 13 -or
        [string] $seedPayload.catalog_sha256 -cne $expectedCatalogHash -or
        [string] $seedPayload.catalog_sha256 -cnotmatch
            '^[0-9a-f]{64}$') {
        throw (
            'Payload inicial nao contem exatamente 13 campos e o ' +
            'SHA-256 lowercase exato do manifesto fixture'
        )
    }
    $passed.Add('payload de 13 campos vinculado ao manifesto fixture')

    $normalOutput = Copy-Seed -Name 'normal-valida'
    Invoke-ExpectedSuccess `
        -Arguments (New-SignArguments `
            -Output $normalOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 5) `
        -Name 'normal com serial maior e piso preservado'

    $replayOutput = Copy-Seed -Name 'replay'
    Invoke-ExpectedFailure `
        -Arguments (New-SignArguments `
            -Output $replayOutput `
            -PolicySerial 100 `
            -MinimumVersionCode 5) `
        -ExpectedMessage 'PolicySerial deve ser maior' `
        -Name 'replay de serial'

    $lowerFloorOutput = Copy-Seed -Name 'normal-reduz-piso'
    Invoke-ExpectedFailure `
        -Arguments (New-SignArguments `
            -Output $lowerFloorOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 4) `
        -ExpectedMessage 'Policy normal nao pode reduzir' `
        -Name 'normal reduz piso'

    $wrongRevokeOutput = Copy-Seed -Name 'rollback-revoga-incorreto'
    Invoke-ExpectedFailure `
        -Arguments (New-SignArguments `
            -Output $wrongRevokeOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 4 `
            -Mode 'rollback' `
            -RevokesPolicySerial 99) `
        -ExpectedMessage 'Rollback deve revogar exatamente' `
        -Name 'rollback revoga serial incorreto'

    $noReductionOutput = Copy-Seed -Name 'rollback-sem-reducao'
    Invoke-ExpectedFailure `
        -Arguments (New-SignArguments `
            -Output $noReductionOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 5 `
            -Mode 'rollback' `
            -RevokesPolicySerial 100) `
        -ExpectedMessage 'Rollback deve reduzir' `
        -Name 'rollback sem reduzir piso'

    $validRollbackOutput = Copy-Seed -Name 'rollback-valido'
    Invoke-ExpectedSuccess `
        -Arguments (New-SignArguments `
            -Output $validRollbackOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 4 `
            -Mode 'rollback' `
            -RevokesPolicySerial 100) `
        -Name 'rollback valido'

    $tamperedOutput = Copy-Seed -Name 'assinatura-adulterada'
    $tamperedEnvelope = (
        [IO.File]::ReadAllText($tamperedOutput, $utf8) |
            ConvertFrom-Json
    )
    $tamperedSignature = [Convert]::FromBase64String(
        [string] $tamperedEnvelope.signature
    )
    $tamperedSignature[0] = $tamperedSignature[0] -bxor 1
    $tamperedEnvelope.signature = [Convert]::ToBase64String(
        $tamperedSignature
    )
    [IO.File]::WriteAllText(
        $tamperedOutput,
        ($tamperedEnvelope | ConvertTo-Json -Compress),
        $utf8
    )
    Invoke-ExpectedFailure `
        -Arguments (New-SignArguments `
            -Output $tamperedOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 5) `
        -ExpectedMessage 'Assinatura da politica anterior invalida' `
        -Name 'assinatura anterior adulterada'

    $legacyOutput = Copy-Seed -Name 'bootstrap-legado'
    Convert-ToSignedLegacyPolicy -Path $legacyOutput
    $legacyPayload = Get-SignedPayload -Path $legacyOutput
    if (@($legacyPayload.PSObject.Properties).Count -ne 12 -or
        $null -ne $legacyPayload.PSObject.Properties['catalog_sha256']) {
        throw 'Fixture de bootstrap nao representa payload legado de 12 campos'
    }
    Invoke-ExpectedSuccess `
        -Arguments (New-SignArguments `
            -Output $legacyOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 5) `
        -Name 'migracao assinada de 12 para 13 campos'
    $migratedPayload = Get-SignedPayload -Path $legacyOutput
    if (@($migratedPayload.PSObject.Properties).Count -ne 13 -or
        [string] $migratedPayload.catalog_sha256 -cne
            $expectedCatalogHash -or
        [long] $migratedPayload.policy_serial -ne 101) {
        throw 'Migracao legada nao produziu a politica nova esperada'
    }

    [IO.File]::WriteAllText($catalogManifest, $catalogFixtureV2, $utf8)
    $mutatedCatalogHash = (
        Get-FileHash -LiteralPath $catalogManifest -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($mutatedCatalogHash -ceq $expectedCatalogHash -or
        [string] $seedPayload.catalog_sha256 -ceq $mutatedCatalogHash) {
        throw 'Alteracao do fixture nao foi detectada pelo SHA-256'
    }
    $passed.Add('alteracao do catalogo detectavel na politica existente')

    $catalogUpdateOutput = Copy-Seed -Name 'catalogo-atualizado'
    Invoke-ExpectedSuccess `
        -Arguments (New-SignArguments `
            -Output $catalogUpdateOutput `
            -PolicySerial 101 `
            -MinimumVersionCode 5) `
        -Name 'nova politica acompanha catalogo alterado'
    $updatedCatalogPayload = Get-SignedPayload -Path $catalogUpdateOutput
    if ([string] $updatedCatalogPayload.catalog_sha256 -cne
            $mutatedCatalogHash -or
        [string] $updatedCatalogPayload.catalog_sha256 -ceq
            $expectedCatalogHash) {
        throw 'Nova politica nao refletiu a alteracao do catalogo'
    }

    [pscustomobject] @{
        Passed = $passed.Count
        Tests = $passed -join '; '
        TemporaryFilesRemoved = $true
        OfficialPolicyTouched = $false
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
