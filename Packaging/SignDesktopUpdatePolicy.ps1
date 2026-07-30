# Assina a politica de atualizacao OBRIGATORIA do ChurchDAW no Windows/macOS.
#
# Irmao do SignAndroidUpdatePolicy.ps1 e usa a MESMA chave privada -- uma so
# chave para guardar, uma so para eventualmente trocar.
#
# O que este script protege:
#   - o SHA-256 de cada instalador e calculado do ARQUIVO REAL, nunca digitado;
#   - o serial so sobe, entao uma politica antiga nao pode ser reapresentada;
#   - uma politica "normal" nao consegue BAIXAR o minimum_version_code (so um
#     rollback declarado consegue, e ele precisa revogar o serial anterior);
#   - depois de assinar, confere a assinatura com a chave PUBLICA oficial: se a
#     privada for a errada, nada e gravado.
#
# Uso tipico:
#   .\SignDesktopUpdatePolicy.ps1 -PolicySerial 1 `
#       -LatestVersionCode 10200 -MinimumVersionCode 10100 `
#       -LatestVersionName 1.2.0 `
#       -UpdateMessage "Correcao de estabilidade no motor de audio." `
#       -WindowsInstaller ..\dist\ChurchDAW-1.2.0-Setup.exe `
#       -WindowsInstallerUrl https://churchdaw.com/app/desktop/ChurchDAW-1.2.0-Setup.exe

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

    [string] $WindowsInstaller = '',
    [string] $WindowsInstallerUrl = '',
    [string] $MacInstaller = '',
    [string] $MacInstallerUrl = '',

    [ValidateSet('Commercial', 'Test')]
    [string] $ReleaseMode = 'Commercial',
    [string] $ExpectedWindowsPublisher = '',
    [Alias('ExpectedWindowsThumbprint')]
    [string[]] $ExpectedWindowsSignerThumbprints = @(),
    [switch] $AllowUnsignedWindowsInstallerForTest,
    [switch] $AllowCurrentVersionPolicyWithoutInstaller,
    [string] $WindowsVerificationLogPath = '',

    [ValidateSet('normal', 'rollback')]
    [string] $Mode = 'normal',
    [int] $RevokesPolicySerial = 0,
    [ValidateRange(1, 180)]
    [int] $ValidDays = 90,
    [string] $PrivateKey = '',
    [string] $Output = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')
$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'
$productId = 'com.churchdaw.app'
$keyId = 'churchdaw-update-2026-01'
$requiredHostSuffix = 'churchdaw.com'
$publicKey = Join-Path $PSScriptRoot 'AndroidUpdatePolicyPublicKey.pem'

function Assert-TrustedUrl {
    param([string] $Url, [string] $Label)

    if ($Url -notmatch '^https://') {
        throw "$Label precisa ser HTTPS: $Url"
    }
    if ($Url.Length -gt 512) {
        throw "$Label longa demais"
    }

    $host_ = ([Uri] $Url).Host.ToLowerInvariant()
    if ($host_ -ne $requiredHostSuffix -and
        -not $host_.EndsWith('.' + $requiredHostSuffix)) {
        throw "$Label precisa apontar para $requiredHostSuffix (veio de $host_)"
    }
}

# Devolve url/sha256/bytes de uma plataforma, ou vazios se ela nao entra nesta
# publicacao. O hash SEMPRE sai do arquivo -- e ele que o app vai conferir antes
# de executar qualquer coisa.
function Resolve-Installer {
    param([string] $Path, [string] $Url, [string] $Label)

    if ([string]::IsNullOrWhiteSpace($Path) -and
        [string]::IsNullOrWhiteSpace($Url)) {
        return [pscustomobject] @{ Url = ''; Sha256 = ''; Bytes = [long] 0 }
    }

    if ([string]::IsNullOrWhiteSpace($Path) -or
        [string]::IsNullOrWhiteSpace($Url)) {
        throw "$Label exige o arquivo E a URL, ou nenhum dos dois"
    }

    $full = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "$Label nao encontrado: $full"
    }

    Assert-TrustedUrl -Url $Url -Label "URL do $Label"

    $item = Get-Item -LiteralPath $full
    if ($item.Length -le 0 -or $item.Length -gt 1073741824) {
        throw "$Label tem tamanho invalido: $($item.Length) bytes"
    }

    # O nome do arquivo na URL precisa bater com o arquivo assinado, senao o app
    # baixa outra coisa e o SHA-256 reprova depois do download inteiro.
    $urlName = [IO.Path]::GetFileName(([Uri] $Url).AbsolutePath)
    if ($urlName -cne $item.Name) {
        throw ("${Label}: o nome na URL ($urlName) difere do arquivo " +
               "($($item.Name))")
    }

    [pscustomobject] @{
        Url = $Url
        Sha256 = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
        Bytes = [long] $item.Length
    }
}

function Read-VerifiedPreviousPolicy {
    param([string] $Path, [string] $TemporaryDirectory)

    $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
    try {
        $envelope = [IO.File]::ReadAllText($Path, $strictUtf8) | ConvertFrom-Json
    }
    catch {
        throw "Envelope anterior invalido: $($_.Exception.Message)"
    }

    if ($null -eq $envelope -or
        $envelope.schema_version -ne 1 -or
        $envelope.algorithm -cne 'SHA256withRSA' -or
        $envelope.key_id -cne $keyId) {
        throw 'Envelope anterior tem metadados invalidos'
    }

    $payloadBytes = [Convert]::FromBase64String([string] $envelope.payload)
    $signatureBytes = [Convert]::FromBase64String([string] $envelope.signature)
    if ($signatureBytes.Length -ne 384) {
        throw 'Assinatura anterior nao tem 384 bytes'
    }

    $payloadFile = Join-Path $TemporaryDirectory 'previous-payload.json'
    $signatureFile = Join-Path $TemporaryDirectory 'previous-payload.sig'
    [IO.File]::WriteAllBytes($payloadFile, $payloadBytes)
    [IO.File]::WriteAllBytes($signatureFile, $signatureBytes)

    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $openssl dgst -sha256 -verify $publicKey -signature $signatureFile $payloadFile | Out-Null
        $code = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $saved }

    if ($code -ne 0) {
        throw 'Assinatura da politica anterior invalida'
    }

    $previous = [Text.UTF8Encoding]::new($false, $true).GetString($payloadBytes) |
        ConvertFrom-Json

    [pscustomobject] @{
        PolicySerial = [long] $previous.policy_serial
        MinimumVersionCode = [long] $previous.minimum_version_code
    }
}

#------------------------------------------------------------------ validacoes
if ([string]::IsNullOrWhiteSpace($PrivateKey)) {
    $PrivateKey = Join-Path $env:USERPROFILE `
        'ChurchDAW-Secrets\churchdaw-update-policy-private.pem'
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $PSScriptRoot `
        'DesktopHosting\app\desktop\update-policy-v1.json'
}

if (-not (Test-Path -LiteralPath $openssl -PathType Leaf)) {
    throw "OpenSSL nao encontrado: $openssl"
}
if (-not (Test-Path -LiteralPath $PrivateKey -PathType Leaf)) {
    throw "Chave privada nao encontrada: $PrivateKey"
}
if (-not (Test-Path -LiteralPath $publicKey -PathType Leaf)) {
    throw "Chave publica nao encontrada: $publicKey"
}

if ($PolicySerial -lt 1 -or $LatestVersionCode -lt 1 -or
    $MinimumVersionCode -lt 1 -or $LatestVersionCode -lt $MinimumVersionCode) {
    throw 'Serial ou faixa de versoes invalida'
}
if ($Mode -eq 'rollback' -and $RevokesPolicySerial -lt 1) {
    throw 'Rollback exige RevokesPolicySerial positivo'
}
if ($Mode -eq 'normal' -and $RevokesPolicySerial -ne 0) {
    throw 'Politica normal nao revoga outro serial'
}
if ($UpdateMessage.IndexOfAny([char[]] "`0`r`n") -ge 0) {
    throw 'A mensagem nao pode conter NUL nem quebras de linha'
}

$windows = Resolve-Installer -Path $WindowsInstaller -Url $WindowsInstallerUrl `
    -Label 'instalador do Windows'
$mac = Resolve-Installer -Path $MacInstaller -Url $MacInstallerUrl `
    -Label 'instalador do macOS'

# Uma politica que exige uma versao nova sem publicar NENHUM instalador nao
# consegue ser cumprida por ninguem. A unica excecao comercial e uma politica
# explicitamente marcada que apenas confirma a versao corrente
# (latest == minimum); ela remove o 404 do endpoint sem anunciar um download.
$artifactSet = Assert-ChurchDAWDesktopPolicyArtifactSet `
    -ReleaseMode $ReleaseMode `
    -LatestVersionCode $LatestVersionCode `
    -MinimumVersionCode $MinimumVersionCode `
    -WindowsInstallerBytes $windows.Bytes `
    -MacInstallerBytes $mac.Bytes `
    -AllowCurrentVersionPolicyWithoutInstaller:(
        $AllowCurrentVersionPolicyWithoutInstaller)

if ($artifactSet -eq 'test-policy-only') {
    Write-Warning ('Politica de teste sem instalador: ela so vai AVISAR, ' +
                   'nunca obrigar.')
}
elseif ($artifactSet -eq 'current-version-policy-only') {
    Write-Host ('Politica comercial da versao corrente: nenhum instalador ' +
                'foi anunciado.')
}
if ($ReleaseMode -eq 'Commercial' -and
    $AllowUnsignedWindowsInstallerForTest) {
    throw ('-AllowUnsignedWindowsInstallerForTest existe somente para ' +
           '-ReleaseMode Test')
}

$windowsAuthenticode = $null
if ($windows.Bytes -gt 0) {
    $windowsFullPath = [IO.Path]::GetFullPath($WindowsInstaller)
    $currentSignature = Get-AuthenticodeSignature `
        -LiteralPath $windowsFullPath

    if ($ReleaseMode -eq 'Test' -and
        $AllowUnsignedWindowsInstallerForTest -and
        $currentSignature.Status -eq 'NotSigned') {
        Write-Warning ('Instalador Windows NAO assinado aceito somente porque ' +
                       'o modo Test foi solicitado explicitamente.')
    }
    else {
        if ($ReleaseMode -eq 'Commercial' -and
            [string]::IsNullOrWhiteSpace($ExpectedWindowsPublisher)) {
            throw ('Politica comercial para Windows exige ' +
                   '-ExpectedWindowsPublisher')
        }
        if ($ReleaseMode -eq 'Commercial' -and
            @($ExpectedWindowsSignerThumbprints |
              Where-Object { -not [string]::IsNullOrWhiteSpace($_) }).Count -eq 0) {
            throw ('Politica comercial para Windows exige ' +
                   '-ExpectedWindowsSignerThumbprints')
        }

        $windowsAuthenticode = Assert-ChurchDAWAuthenticodeSignature `
            -Path $windowsFullPath `
            -ExpectedPublisher $ExpectedWindowsPublisher `
            -ExpectedSignerThumbprints $ExpectedWindowsSignerThumbprints `
            -RequireTimestamp ($ReleaseMode -eq 'Commercial') `
            -RequirePublicChain ($ReleaseMode -eq 'Commercial') `
            -VerificationLogPath $WindowsVerificationLogPath
    }
}

$now = [DateTimeOffset]::UtcNow
$payloadObject = [ordered] @{
    schema_version = 1
    product_id = $productId
    policy_serial = $PolicySerial
    mode = $Mode
    revokes_policy_serial = $RevokesPolicySerial
    latest_version_code = $LatestVersionCode
    minimum_version_code = $MinimumVersionCode
    latest_version_name = $LatestVersionName
    issued_at = ConvertTo-ChurchDAWUnixTimeSeconds $now
    expires_at = ConvertTo-ChurchDAWUnixTimeSeconds `
        $now.AddDays($ValidDays)
    update_message = $UpdateMessage
    windows_installer_url = $windows.Url
    windows_installer_sha256 = $windows.Sha256
    windows_installer_bytes = $windows.Bytes
    mac_installer_url = $mac.Url
    mac_installer_sha256 = $mac.Sha256
    mac_installer_bytes = $mac.Bytes
}

$payload = $payloadObject | ConvertTo-Json -Compress
$utf8 = [Text.UTF8Encoding]::new($false)
$temporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-desktop-policy-' + [Guid]::NewGuid().ToString('N'))
$payloadFile = Join-Path $temporaryDirectory 'payload.json'
$signatureFile = Join-Path $temporaryDirectory 'payload.sig'
$outputFullPath = [IO.Path]::GetFullPath($Output)

New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null
try {
    if (Test-Path -LiteralPath $outputFullPath -PathType Leaf) {
        $previous = Read-VerifiedPreviousPolicy -Path $outputFullPath `
            -TemporaryDirectory $temporaryDirectory

        if ([long] $PolicySerial -le $previous.PolicySerial) {
            throw ("PolicySerial deve ser maior que o anterior " +
                   "($($previous.PolicySerial))")
        }
        if ($Mode -eq 'normal' -and
            [long] $MinimumVersionCode -lt $previous.MinimumVersionCode) {
            throw ("Politica normal nao pode reduzir minimum_version_code de " +
                   "$($previous.MinimumVersionCode) para $MinimumVersionCode")
        }
        if ($Mode -eq 'rollback') {
            if ([long] $RevokesPolicySerial -ne $previous.PolicySerial) {
                throw ("Rollback deve revogar exatamente o serial anterior " +
                       "($($previous.PolicySerial))")
            }
            if ([long] $MinimumVersionCode -ge $previous.MinimumVersionCode) {
                throw ("Rollback deve reduzir minimum_version_code abaixo de " +
                       "$($previous.MinimumVersionCode)")
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
        throw "Assinatura tem $($signatureBytes.Length) bytes; esperado 384"
    }

    # Ultima linha de defesa: a assinatura recem-feita precisa passar na chave
    # PUBLICA que esta dentro do app. Chave privada errada morre aqui.
    $saved = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $openssl dgst -sha256 -verify $publicKey -signature $signatureFile $payloadFile | Out-Null
        $verifyCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $saved }

    if ($verifyCode -ne 0) {
        throw ('A chave privada nao corresponde a chave publica oficial; ' +
               'nada foi gravado')
    }

    $envelope = [ordered] @{
        schema_version = 1
        algorithm = 'SHA256withRSA'
        key_id = $keyId
        payload = [Convert]::ToBase64String($payloadBytes)
        signature = [Convert]::ToBase64String($signatureBytes)
    } | ConvertTo-Json -Compress

    New-Item -ItemType Directory -Path (Split-Path -Parent $outputFullPath) -Force | Out-Null
    [IO.File]::WriteAllText($outputFullPath, $envelope, $utf8)

    [pscustomobject] @{
        Output = $outputFullPath
        PolicySerial = $PolicySerial
        LatestVersionCode = $LatestVersionCode
        MinimumVersionCode = $MinimumVersionCode
        WindowsInstaller = $windows.Url
        WindowsSha256 = $windows.Sha256
        WindowsPublisher = if ($windowsAuthenticode) {
            $windowsAuthenticode.Publisher
        } else { '' }
        WindowsSignerThumbprint = if ($windowsAuthenticode) {
            $windowsAuthenticode.SignerThumbprint
        } else { '' }
        MacInstaller = $mac.Url
        MacSha256 = $mac.Sha256
        ExpiresAt = $payloadObject.expires_at
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryDirectory) {
        Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
    }
}
