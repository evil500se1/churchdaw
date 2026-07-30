param(
    [Parameter(Mandatory = $true)]
    [int] $PolicySerial,
    [Parameter(Mandatory = $true)]
    [int] $LatestVersionCode,
    [Parameter(Mandatory = $true)]
    [int] $MinimumVersionCode,
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Za-z._+-]{1,48}$')]
    [string] $LatestVersionName,
    [Parameter(Mandatory = $true)]
    [ValidateLength(0, 640)]
    [string] $UpdateMessage,
    [ValidatePattern('^https://churchdaw\.com/app/android/ChurchDAW-[0-9A-Za-z._+-]{1,48}(?:-signed)?\.apk$')]
    [string] $UpdateApkUrl = 'https://churchdaw.com/app/android/ChurchDAW-1.5.0.apk',
    [ValidateSet('normal', 'rollback')]
    [string] $Mode = 'normal',
    [int] $RevokesPolicySerial = 0,
    [ValidateRange(1, 180)]
    [int] $ValidDays = 90,
    [string] $PrivateKey = '',
    [string] $CatalogManifest = '',
    [string] $Output = ''
)

$ErrorActionPreference = 'Stop'
$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'
$packageId = 'com.churchdaw.app'
$playStoreUrl = $UpdateApkUrl
$keyId = 'churchdaw-update-2026-01'
$publicKey = Join-Path $PSScriptRoot 'AndroidUpdatePolicyPublicKey.pem'

function Get-RequiredPositiveInt64Property {
    param(
        [Parameter(Mandatory = $true)]
        [object] $Object,
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Politica anterior sem campo obrigatorio: $Name"
    }

    if ($property.Value -isnot [int] -and
        $property.Value -isnot [long]) {
        throw "Politica anterior tem tipo invalido: $Name"
    }

    $text = [Convert]::ToString(
        $property.Value,
        [Globalization.CultureInfo]::InvariantCulture
    )
    if ($text -notmatch '^[1-9][0-9]{0,18}$') {
        throw "Politica anterior tem campo invalido: $Name"
    }

    $value = [long] 0
    if (-not [long]::TryParse(
        $text,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref] $value
    )) {
        throw "Politica anterior tem campo fora da faixa: $Name"
    }

    return $value
}

function Get-RequiredNonNegativeInt64Property {
    param(
        [Parameter(Mandatory = $true)]
        [object] $Object,
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Politica anterior sem campo obrigatorio: $Name"
    }
    if ($property.Value -isnot [int] -and
        $property.Value -isnot [long]) {
        throw "Politica anterior tem tipo invalido: $Name"
    }

    $text = [Convert]::ToString(
        $property.Value,
        [Globalization.CultureInfo]::InvariantCulture
    )
    if ($text -notmatch '^(0|[1-9][0-9]{0,18})$') {
        throw "Politica anterior tem campo invalido: $Name"
    }

    $value = [long] 0
    if (-not [long]::TryParse(
        $text,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref] $value
    )) {
        throw "Politica anterior tem campo fora da faixa: $Name"
    }

    return $value
}

function Get-RequiredSha256Property {
    param(
        [Parameter(Mandatory = $true)]
        [object] $Object,
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "Politica anterior sem campo obrigatorio: $Name"
    }

    $value = [string] $property.Value
    if ($value -cnotmatch '^[0-9a-f]{64}$') {
        throw "Politica anterior tem SHA-256 invalido: $Name"
    }

    return $value
}

function Read-VerifiedPreviousPolicy {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,
        [Parameter(Mandatory = $true)]
        [string] $TemporaryDirectory,
        [Parameter(Mandatory = $true)]
        [string] $OpenSslPath,
        [Parameter(Mandatory = $true)]
        [string] $PublicKeyPath
    )

    $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
    try {
        $envelopeText = [IO.File]::ReadAllText($Path, $strictUtf8)
        $envelope = $envelopeText | ConvertFrom-Json
    }
    catch {
        throw "Envelope da politica anterior invalido: $($_.Exception.Message)"
    }

    if ($null -eq $envelope -or
        $envelope.schema_version -ne 2 -or
        $envelope.algorithm -cne 'SHA256withRSA' -or
        $envelope.key_id -cne $keyId) {
        throw 'Envelope da politica anterior tem metadados invalidos'
    }

    try {
        $previousPayloadBytes = [Convert]::FromBase64String(
            [string] $envelope.payload
        )
        $previousSignatureBytes = [Convert]::FromBase64String(
            [string] $envelope.signature
        )
    }
    catch {
        throw 'Envelope da politica anterior contem Base64 invalido'
    }

    if ($previousPayloadBytes.Length -lt 2 -or
        $previousSignatureBytes.Length -ne 384) {
        throw 'Envelope da politica anterior tem tamanhos invalidos'
    }

    $previousPayloadFile = Join-Path $TemporaryDirectory 'previous-payload.json'
    $previousSignatureFile = Join-Path $TemporaryDirectory 'previous-payload.sig'
    [IO.File]::WriteAllBytes($previousPayloadFile, $previousPayloadBytes)
    [IO.File]::WriteAllBytes($previousSignatureFile, $previousSignatureBytes)

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $verificationOutput = & $OpenSslPath dgst -sha256 `
            -verify $PublicKeyPath `
            -signature $previousSignatureFile `
            $previousPayloadFile 2>&1
        $verificationExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($verificationExitCode -ne 0) {
        throw 'Assinatura da politica anterior invalida'
    }

    try {
        $previousPayloadText = $strictUtf8.GetString($previousPayloadBytes)
        $previousPayload = $previousPayloadText | ConvertFrom-Json
    }
    catch {
        throw "Payload da politica anterior invalido: $($_.Exception.Message)"
    }

    if ($null -eq $previousPayload) {
        throw 'Payload da politica anterior vazio'
    }

    $basePayloadFields = @(
        'schema_version',
        'package_id',
        'policy_serial',
        'mode',
        'revokes_policy_serial',
        'latest_version_code',
        'minimum_version_code',
        'latest_version_name',
        'issued_at',
        'expires_at',
        'update_message',
        'play_store_url'
    )
    $hasCatalogSha256 = $null -ne (
        $previousPayload.PSObject.Properties['catalog_sha256']
    )
    $expectedPayloadFields = @($basePayloadFields)
    if ($hasCatalogSha256) {
        $expectedPayloadFields += 'catalog_sha256'
    }
    $actualPayloadFields = @(
        $previousPayload.PSObject.Properties.Name
    )
    if ($actualPayloadFields.Count -ne $expectedPayloadFields.Count) {
        throw 'Payload da politica anterior tem quantidade de campos invalida'
    }
    foreach ($expectedField in $expectedPayloadFields) {
        if ($actualPayloadFields -cnotcontains $expectedField) {
            throw (
                'Payload da politica anterior tem conjunto de campos invalido'
            )
        }
    }

    if ($previousPayload.schema_version -isnot [int] -or
        $previousPayload.schema_version -ne 2 -or
        $previousPayload.package_id -isnot [string] -or
        $previousPayload.package_id -cne $packageId) {
        throw 'Payload da politica anterior tem metadados invalidos'
    }

    $previousSerial = Get-RequiredPositiveInt64Property `
        -Object $previousPayload `
        -Name 'policy_serial'
    $previousLatestVersion = Get-RequiredPositiveInt64Property `
        -Object $previousPayload `
        -Name 'latest_version_code'
    $previousMinimumVersion = Get-RequiredPositiveInt64Property `
        -Object $previousPayload `
        -Name 'minimum_version_code'
    $previousRevokesSerial = Get-RequiredNonNegativeInt64Property `
        -Object $previousPayload `
        -Name 'revokes_policy_serial'
    $previousIssuedAt = Get-RequiredPositiveInt64Property `
        -Object $previousPayload `
        -Name 'issued_at'
    $previousExpiresAt = Get-RequiredPositiveInt64Property `
        -Object $previousPayload `
        -Name 'expires_at'

    $previousMode = $previousPayload.mode
    if ($previousMode -isnot [string] -or
        ($previousMode -cne 'normal' -and
         $previousMode -cne 'rollback') -or
        ($previousMode -ceq 'normal' -and
         $previousRevokesSerial -ne 0) -or
        ($previousMode -ceq 'rollback' -and
         $previousRevokesSerial -lt 1) -or
        $previousLatestVersion -lt $previousMinimumVersion -or
        $previousExpiresAt -le $previousIssuedAt) {
        throw 'Payload da politica anterior tem valores incoerentes'
    }

    $previousVersionName = $previousPayload.latest_version_name
    $previousMessage = $previousPayload.update_message
    $previousPlayStoreUrl = $previousPayload.play_store_url
    if ($previousVersionName -isnot [string] -or
        $previousVersionName -cnotmatch '^[0-9A-Za-z._+-]{1,48}$' -or
        $previousMessage -isnot [string] -or
        $previousMessage.Length -gt 640 -or
        $previousMessage.IndexOfAny([char[]] "`0`r`n") -ge 0 -or
        $previousPlayStoreUrl -isnot [string] -or
        $previousPlayStoreUrl -cnotmatch
            '^https://churchdaw\.com/app/android/ChurchDAW-[0-9A-Za-z._+-]{1,48}(?:-signed)?\.apk$') {
        throw 'Payload da politica anterior tem strings invalidas'
    }

    $previousCatalogSha256 = ''
    if ($hasCatalogSha256) {
        $previousCatalogSha256 = Get-RequiredSha256Property `
            -Object $previousPayload `
            -Name 'catalog_sha256'
    }

    [pscustomobject] @{
        PolicySerial = $previousSerial
        MinimumVersionCode = $previousMinimumVersion
        CatalogSha256 = $previousCatalogSha256
        IsLegacyWithoutCatalogSha256 = -not $hasCatalogSha256
    }
}

if ([string]::IsNullOrWhiteSpace($PrivateKey)) {
    $PrivateKey = Join-Path $env:USERPROFILE (
        'ChurchDAW-Secrets\churchdaw-update-policy-private.pem'
    )
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $PSScriptRoot (
        'AndroidHosting\app\android\update-policy-v2.json'
    )
}

if ([string]::IsNullOrWhiteSpace($CatalogManifest)) {
    $CatalogManifest = Join-Path $PSScriptRoot (
        'AndroidHosting\app\android\manifest.json'
    )
}

if (-not (Test-Path -LiteralPath $openssl -PathType Leaf)) {
    throw "OpenSSL nao encontrado: $openssl"
}

if (-not (Test-Path -LiteralPath $PrivateKey -PathType Leaf)) {
    throw "Chave privada da politica nao encontrada: $PrivateKey"
}

if (-not (Test-Path -LiteralPath $publicKey -PathType Leaf)) {
    throw "Chave publica da politica nao encontrada: $publicKey"
}

if (-not (Test-Path -LiteralPath $CatalogManifest -PathType Leaf)) {
    throw "Manifesto do catalogo nao encontrado: $CatalogManifest"
}

if ($PolicySerial -lt 1 -or
    $LatestVersionCode -lt 1 -or
    $MinimumVersionCode -lt 1 -or
    $LatestVersionCode -lt $MinimumVersionCode) {
    throw 'Serial/faixa de versoes invalida'
}

if ($Mode -eq 'rollback' -and $RevokesPolicySerial -lt 1) {
    throw 'Rollback exige RevokesPolicySerial positivo'
}

if ($Mode -eq 'normal' -and $RevokesPolicySerial -ne 0) {
    throw 'Policy normal nao pode revogar outro serial'
}

if ($UpdateMessage.IndexOfAny([char[]] "`0`r`n") -ge 0) {
    throw 'A mensagem nao pode conter NUL ou quebras de linha'
}

$now = [DateTimeOffset]::UtcNow
$issuedAt = $now.ToUnixTimeSeconds()
$expiresAt = $now.AddDays($ValidDays).ToUnixTimeSeconds()
$catalogManifestFullPath = [IO.Path]::GetFullPath($CatalogManifest)
$catalogSha256 = (
    Get-FileHash -LiteralPath $catalogManifestFullPath -Algorithm SHA256
).Hash.ToLowerInvariant()
$payloadObject = [ordered] @{
    schema_version = 2
    package_id = $packageId
    policy_serial = $PolicySerial
    mode = $Mode
    revokes_policy_serial = $RevokesPolicySerial
    latest_version_code = $LatestVersionCode
    minimum_version_code = $MinimumVersionCode
    latest_version_name = $LatestVersionName
    catalog_sha256 = $catalogSha256
    issued_at = $issuedAt
    expires_at = $expiresAt
    update_message = $UpdateMessage
    play_store_url = $playStoreUrl
}
$payload = $payloadObject | ConvertTo-Json -Compress
$utf8 = [Text.UTF8Encoding]::new($false)
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-policy-' + [Guid]::NewGuid().ToString('N')
)
$payloadFile = Join-Path $temporaryDirectory 'payload.json'
$signatureFile = Join-Path $temporaryDirectory 'payload.sig'
$outputFullPath = [IO.Path]::GetFullPath($Output)

New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
try {
    if (Test-Path -LiteralPath $outputFullPath -PathType Leaf) {
        $previousPolicy = Read-VerifiedPreviousPolicy `
            -Path $outputFullPath `
            -TemporaryDirectory $temporaryDirectory `
            -OpenSslPath $openssl `
            -PublicKeyPath $publicKey

        if ([long] $PolicySerial -le $previousPolicy.PolicySerial) {
            throw (
                "PolicySerial deve ser maior que o serial anterior " +
                "($($previousPolicy.PolicySerial))"
            )
        }

        if ($Mode -eq 'normal' -and
            [long] $MinimumVersionCode -lt
                $previousPolicy.MinimumVersionCode) {
            throw (
                "Policy normal nao pode reduzir minimum_version_code " +
                "de $($previousPolicy.MinimumVersionCode) para " +
                "$MinimumVersionCode"
            )
        }

        if ($Mode -eq 'rollback') {
            if ([long] $RevokesPolicySerial -ne
                $previousPolicy.PolicySerial) {
                throw (
                    "Rollback deve revogar exatamente o serial anterior " +
                    "($($previousPolicy.PolicySerial))"
                )
            }

            if ([long] $MinimumVersionCode -ge
                $previousPolicy.MinimumVersionCode) {
                throw (
                    "Rollback deve reduzir minimum_version_code abaixo de " +
                    "$($previousPolicy.MinimumVersionCode)"
                )
            }
        }
    }

    [IO.File]::WriteAllText($payloadFile, $payload, $utf8)
    & $openssl dgst -sha256 -sign $PrivateKey -out $signatureFile $payloadFile
    if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $signatureFile -PathType Leaf)) {
        throw 'OpenSSL nao conseguiu assinar a politica'
    }

    $payloadBytes = [IO.File]::ReadAllBytes($payloadFile)
    $signatureBytes = [IO.File]::ReadAllBytes($signatureFile)
    if ($signatureBytes.Length -ne 384) {
        throw "Assinatura RSA-3072 tem $($signatureBytes.Length) bytes; esperado 384"
    }

    $savedErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $newSignatureVerification = & $openssl dgst -sha256 `
            -verify $publicKey `
            -signature $signatureFile `
            $payloadFile 2>&1
        $newSignatureVerificationExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorActionPreference
    }
    if ($newSignatureVerificationExitCode -ne 0) {
        throw (
            'A chave privada nao corresponde a chave publica oficial; ' +
            'a politica nao foi gravada'
        )
    }

    $envelope = [ordered] @{
        schema_version = 2
        algorithm = 'SHA256withRSA'
        key_id = $keyId
        payload = [Convert]::ToBase64String($payloadBytes)
        signature = [Convert]::ToBase64String($signatureBytes)
    } | ConvertTo-Json -Compress

    $outputDirectory = Split-Path -Parent $outputFullPath
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    [IO.File]::WriteAllText($outputFullPath, $envelope, $utf8)

    [pscustomobject] @{
        Output = $outputFullPath
        PolicySerial = $PolicySerial
        LatestVersionCode = $LatestVersionCode
        MinimumVersionCode = $MinimumVersionCode
        CatalogManifest = $catalogManifestFullPath
        CatalogSha256 = $catalogSha256
        IssuedAt = $issuedAt
        ExpiresAt = $expiresAt
        Sha256 = (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
