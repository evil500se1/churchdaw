[CmdletBinding()]
param(
    [string] $BaseUrl = 'https://churchdaw.com'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Net.Http

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-trial-smoke-' + [Guid]::NewGuid().ToString('N')
)
New-Item -ItemType Directory -Path $temporaryRoot | Out-Null

try {
    $openssl = 'C:\Program Files\Git\mingw64\bin\openssl.exe'
    if (-not (Test-Path -LiteralPath $openssl -PathType Leaf)) {
        throw 'OpenSSL de teste nao encontrado.'
    }
    $privateKey = Join-Path $temporaryRoot 'device.pem'
    $publicDer = Join-Path $temporaryRoot 'device.der'
    & $openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 `
        -out $privateKey 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw 'Falha ao gerar chave efemera do smoke.'
    }
    & $openssl pkey -in $privateKey -pubout -outform DER `
        -out $publicDer 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw 'Falha ao exportar chave efemera do smoke.'
    }

    $publicKey = [Convert]::ToBase64String(
        [IO.File]::ReadAllBytes($publicDer)
    ).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $suffix = [Guid]::NewGuid().ToString('N')
    $payload = @{
        installation_id = 'smoke-' + $suffix
        device_public_key = $publicKey
        device_fingerprint = 'churchdaw.smoke.' + $suffix
        device_label = 'ChurchDAW backend smoke'
    } | ConvertTo-Json -Compress

    $reserve = Invoke-RestMethod `
        -Uri ($BaseUrl.TrimEnd('/') + '/api/v1/trial/reserve') `
        -Method Post `
        -ContentType 'application/json' `
        -Body $payload `
        -TimeoutSec 60
    $invalidReservation = ($reserve.mode -ne 'trial_setup') -or
        ([int] $reserve.trial_seconds -ne 1200)
    if ($invalidReservation) {
        throw 'Reserva da demonstracao retornou estado invalido.'
    }

    $installationId = 'smoke-' + $suffix
    $timestamp = [int64] (
        ([DateTime]::UtcNow - [DateTime] '1970-01-01').TotalSeconds
    )
    $nonce = [Convert]::ToBase64String(
        [Guid]::NewGuid().ToByteArray()
    )
    $message = $installationId + '|' + $timestamp + '|' + $nonce +
        '|' + [string] $reserve.setup_token
    $messagePath = Join-Path $temporaryRoot 'proof-message.txt'
    $signaturePath = Join-Path $temporaryRoot 'proof-signature.bin'
    [IO.File]::WriteAllText(
        $messagePath,
        $message,
        [Text.UTF8Encoding]::new($false)
    )
    & $openssl dgst -sha256 -sign $privateKey `
        -out $signaturePath $messagePath 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw 'Falha ao assinar prova efemera do aparelho.'
    }
    $startPayload = @{
        installation_id = $installationId
        timestamp = $timestamp
        nonce = $nonce
        proof = [Convert]::ToBase64String(
            [IO.File]::ReadAllBytes($signaturePath)
        )
    } | ConvertTo-Json -Compress
    $trial = Invoke-RestMethod `
        -Uri ($BaseUrl.TrimEnd('/') + '/api/v1/trial/start') `
        -Method Post `
        -Headers @{
            Authorization = 'Bearer ' + [string] $reserve.setup_token
        } `
        -ContentType 'application/json' `
        -Body $startPayload `
        -TimeoutSec 60
    $invalidStart = ($trial.mode -ne 'trial') -or
        ([int] $trial.expires_in -lt 1150) -or
        ([int] $trial.expires_in -gt 1200)
    if ($invalidStart) {
        throw 'Inicio da demonstracao retornou estado invalido.'
    }

    $handler = [Net.Http.HttpClientHandler]::new()
    $client = [Net.Http.HttpClient]::new($handler)
    try {
        $downloadUrl = $BaseUrl.TrimEnd('/')
        $downloadUrl +=
            '/api/v1/library/files/church-live-v1-segment-01.zip'
        $request = [Net.Http.HttpRequestMessage]::new(
            [Net.Http.HttpMethod]::Get,
            $downloadUrl
        )
        $request.Headers.Authorization =
            [Net.Http.Headers.AuthenticationHeaderValue]::new(
                'Bearer',
                [string] $trial.lease_token
            )
        $request.Headers.Range =
            [Net.Http.Headers.RangeHeaderValue]::new(0, 1023)
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        $bytes = $response.Content.ReadAsByteArrayAsync().
            GetAwaiter().GetResult()
        $contentRange = if ($response.Content.Headers.ContentRange) {
            $response.Content.Headers.ContentRange.ToString()
        } else {
            ''
        }
        $invalidDownload = ([int] $response.StatusCode -ne 206) -or
            ($bytes.Length -ne 1024) -or
            ([Text.Encoding]::ASCII.GetString($bytes, 0, 2) -ne 'PK')
        if ($invalidDownload) {
            throw 'Download protegido por faixa falhou.'
        }

        [pscustomobject]@{
            ReserveMode = $reserve.mode
            ActiveMode = $trial.mode
            TrialSeconds = [int] $reserve.trial_seconds
            ActiveSeconds = [int] $trial.expires_in
            SetupWindowSeconds = (
                [int64] $reserve.setup_expires_at -
                [int64] $reserve.server_time
            )
            DownloadStatus = [int] $response.StatusCode
            ContentRange = $contentRange
            Bytes = $bytes.Length
            ZipSignature = $true
        }
    }
    finally {
        $client.Dispose()
        $handler.Dispose()
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
