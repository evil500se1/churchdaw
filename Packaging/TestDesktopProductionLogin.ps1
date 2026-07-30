[CmdletBinding()]
param(
    [string] $Email = $env:CHURCHDAW_QA_EMAIL,
    [string] $Password = $env:CHURCHDAW_QA_PASSWORD,
    [string] $BaseUrl = 'https://churchdaw.com/api/v1',
    [string] $Verifier = '',
    [switch] $AllowOverwrite
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Security

if ([string]::IsNullOrWhiteSpace($Verifier)) {
    $Verifier = Join-Path $PSScriptRoot (
        '..\build-licensing\Release\ChurchDAWLicenseVerifierTests.exe'
    )
}

function New-Base64UrlToken {
    param([Parameter(Mandatory = $true)][int] $Bytes)

    $buffer = New-Object byte[] $Bytes
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($buffer)
    }
    finally {
        $rng.Dispose()
    }
    return [Convert]::ToBase64String($buffer).
        TrimEnd('=').Replace('+', '-').Replace('/', '_')
}

function Get-AccountFile {
    param(
        [Parameter(Mandatory = $true)][string] $Folder,
        [Parameter(Mandatory = $true)][string] $Account
    )

    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Account))
    }
    finally {
        $sha.Dispose()
    }
    $name = -join ($hash | ForEach-Object { $_.ToString('x2') })
    return Join-Path $Folder ($name + '.credential')
}

function Protect-ChurchDAWSecret {
    param([Parameter(Mandatory = $true)][string] $Text)

    $plain = [Text.Encoding]::UTF8.GetBytes($Text)
    $entropy = [Text.Encoding]::ASCII.GetBytes(
        'ChurchDAW.DesktopLicensing.CredentialStore.v1'
    )
    try {
        return [System.Security.Cryptography.ProtectedData]::Protect(
            $plain,
            $entropy,
            [System.Security.Cryptography.DataProtectionScope]::CurrentUser
        )
    }
    finally {
        [Array]::Clear($plain, 0, $plain.Length)
    }
}

function New-DeviceProof {
    param(
        [Parameter(Mandatory = $true)][string] $DeviceSecret,
        [Parameter(Mandatory = $true)][string] $Message
    )

    $key = [Text.Encoding]::UTF8.GetBytes($DeviceSecret)
    $body = [Text.Encoding]::UTF8.GetBytes($Message)
    $hmac = New-Object Security.Cryptography.HMACSHA256
    try {
        $hmac.Key = $key
        return [Convert]::ToBase64String($hmac.ComputeHash($body))
    }
    finally {
        $hmac.Dispose()
        [Array]::Clear($key, 0, $key.Length)
        [Array]::Clear($body, 0, $body.Length)
    }
}

if (-not $BaseUrl.StartsWith('https://', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'O smoke de produção exige HTTPS.'
}
if ([string]::IsNullOrWhiteSpace($Email) -or
    [string]::IsNullOrEmpty($Password)) {
    throw 'Defina CHURCHDAW_QA_EMAIL e CHURCHDAW_QA_PASSWORD.'
}
if (-not (Test-Path -LiteralPath $Verifier -PathType Leaf)) {
    throw "Verificador de licença não encontrado: $Verifier"
}

$credentialFolder = Join-Path $env:APPDATA 'ChurchDAW\Licensing'
$deviceFile = Get-AccountFile $credentialFolder 'desktop-device-v1'
$sessionFile = Get-AccountFile $credentialFolder 'desktop-session-v1'
$targets = @($deviceFile, $sessionFile)
$existing = @($targets | Where-Object { Test-Path -LiteralPath $_ })
if ($existing.Count -gt 0 -and -not $AllowOverwrite) {
    throw 'O cofre ChurchDAW já contém uma sessão. O teste recusou sobrescrevê-la.'
}

$installationId = New-Base64UrlToken 16
$deviceSecret = New-Base64UrlToken 32
$payload = @{
    email = $Email.Trim().ToLowerInvariant()
    password = $Password
    installation_id = $installationId
    device_secret = $deviceSecret
    device_label = "$env:COMPUTERNAME (Windows QA)"
    platform = 'windows'
    app_version = '1.3.0'
} | ConvertTo-Json -Compress

$created = New-Object Collections.Generic.List[string]
$leasePath = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-windows-lease-' + [Guid]::NewGuid().ToString('N') + '.token'
)

try {
    $response = Invoke-RestMethod `
        -Uri ($BaseUrl.TrimEnd('/') + '/auth/login') `
        -Method Post `
        -ContentType 'application/json' `
        -Body $payload `
        -TimeoutSec 60

    $Password = $null
    $payload = $null

    foreach ($property in 'refresh_token', 'lease_token', 'server_time') {
        if (-not $response.PSObject.Properties[$property] -or
            [string]::IsNullOrWhiteSpace([string] $response.$property)) {
            throw "Resposta de login sem $property."
        }
    }

    # Exercise the same device-bound refresh performed by the desktop client.
    # This catches mismatches between the server's HMAC key representation and
    # the native implementation before a release is shipped.
    $timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $nonce = New-Base64UrlToken 24
    $refreshToken = [string] $response.refresh_token
    $proofMessage = $installationId + '|' + $timestamp + '|' +
        $nonce + '|' + $refreshToken
    $refreshPayload = @{
        refresh_token = $refreshToken
        installation_id = $installationId
        timestamp = $timestamp
        nonce = $nonce
        proof = New-DeviceProof $deviceSecret $proofMessage
    } | ConvertTo-Json -Compress
    $response = Invoke-RestMethod `
        -Uri ($BaseUrl.TrimEnd('/') + '/auth/refresh') `
        -Method Post `
        -ContentType 'application/json' `
        -Body $refreshPayload `
        -TimeoutSec 60
    $refreshToken = $null
    $proofMessage = $null
    $refreshPayload = $null

    foreach ($property in 'refresh_token', 'lease_token', 'server_time') {
        if (-not $response.PSObject.Properties[$property] -or
            [string]::IsNullOrWhiteSpace([string] $response.$property)) {
            throw "Resposta de refresh sem $property."
        }
    }

    [IO.File]::WriteAllText(
        $leasePath,
        [string] $response.lease_token,
        [Text.UTF8Encoding]::new($false)
    )
    & $Verifier $leasePath $installationId ([string] $response.server_time)
    if ($LASTEXITCODE -ne 0) {
        throw 'A licença recebida falhou na verificação RS256 local.'
    }

    if (-not (Test-Path -LiteralPath $credentialFolder)) {
        New-Item -ItemType Directory -Path $credentialFolder | Out-Null
    }

    $deviceJson = @{
        schema = 2
        installation_id = $installationId
        device_secret = $deviceSecret
        device_label = "$env:COMPUTERNAME (Windows QA)"
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllBytes(
        $deviceFile,
        (Protect-ChurchDAWSecret $deviceJson)
    )
    $created.Add($deviceFile)

    $sessionJson = @{
        schema = 2
        email = $Email.Trim().ToLowerInvariant()
        refresh_token = [string] $response.refresh_token
        lease_token = [string] $response.lease_token
        trusted_time = [int64] $response.server_time
    } | ConvertTo-Json -Compress
    [IO.File]::WriteAllBytes(
        $sessionFile,
        (Protect-ChurchDAWSecret $sessionJson)
    )
    $created.Add($sessionFile)

    [pscustomobject]@{
        Result = 'PASS'
        Email = $Email.Trim().ToLowerInvariant()
        InstallationId = $installationId
        CredentialFiles = $created.Count
        TokensPrinted = $false
    }
}
catch {
    foreach ($path in $created) {
        if (Test-Path -LiteralPath $path) {
            Remove-Item -LiteralPath $path -Force
        }
    }
    throw
}
finally {
    $Password = $null
    $payload = $null
    if (Test-Path -LiteralPath $leasePath) {
        Remove-Item -LiteralPath $leasePath -Force
    }
}
