[CmdletBinding()]
param(
    [string] $BaseUrl = 'https://churchdaw.com'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Net.Http

$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-library-smoke-' + [Guid]::NewGuid().ToString('N')
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
    $installationId = 'library-smoke-' + $suffix
    $reservePayload = @{
        installation_id = $installationId
        device_public_key = $publicKey
        device_fingerprint = 'churchdaw.library-smoke.' + $suffix
        device_label = 'ChurchDAW production library catalog smoke'
    } | ConvertTo-Json -Compress

    $reserve = Invoke-RestMethod `
        -Uri ($BaseUrl.TrimEnd('/') + '/api/v1/trial/reserve') `
        -Method Post `
        -ContentType 'application/json' `
        -Body $reservePayload `
        -TimeoutSec 60
    if ($reserve.mode -ne 'trial_setup' -or
        ([int] $reserve.trial_seconds) -ne 1200) {
        throw 'Reserva da demonstracao retornou estado invalido.'
    }

    $timestamp = [int64] (
        ([DateTime]::UtcNow - [DateTime] '1970-01-01').TotalSeconds
    )
    $nonce = [Convert]::ToBase64String([Guid]::NewGuid().ToByteArray())
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
        -Headers @{ Authorization = 'Bearer ' + [string] $reserve.setup_token } `
        -ContentType 'application/json' `
        -Body $startPayload `
        -TimeoutSec 60
    if ($trial.mode -ne 'trial' -or
        ([int] $trial.expires_in) -lt 1150 -or
        ([int] $trial.expires_in) -gt 1200) {
        throw 'Inicio da demonstracao retornou estado invalido.'
    }

    $manifest = Invoke-RestMethod `
        -Uri ($BaseUrl.TrimEnd('/') + '/app/android/manifest.json') `
        -TimeoutSec 60
    $library = @($manifest.libraries | Where-Object id -eq 'church-live')
    if ($library.Count -ne 1 -or @($library[0].parts).Count -ne 30) {
        throw 'Catalogo Church Live nao contem exatamente 30 partes.'
    }
    $parts = @($library[0].parts)
    $declaredTotal = [int64] ($parts |
        Measure-Object download_bytes -Sum).Sum
    if ($declaredTotal -ne [int64] $library[0].download_bytes) {
        throw 'Soma das partes diverge do tamanho declarado da biblioteca.'
    }

    $handler = [Net.Http.HttpClientHandler]::new()
    $client = [Net.Http.HttpClient]::new($handler)
    try {
        $results = for ($index = 0; $index -lt $parts.Count; $index++) {
            $part = $parts[$index]
            $request = [Net.Http.HttpRequestMessage]::new(
                [Net.Http.HttpMethod]::Get,
                [string] $part.download_url
            )
            $request.Headers.Authorization =
                [Net.Http.Headers.AuthenticationHeaderValue]::new(
                    'Bearer',
                    [string] $trial.lease_token
                )
            $request.Headers.Range =
                [Net.Http.Headers.RangeHeaderValue]::new(0, 1023)
            $response = $client.SendAsync($request).GetAwaiter().GetResult()
            try {
                $bytes = $response.Content.ReadAsByteArrayAsync().
                    GetAwaiter().GetResult()
                $contentRange = if ($response.Content.Headers.ContentRange) {
                    $response.Content.Headers.ContentRange
                } else {
                    $null
                }
                $expected = [int64] $part.download_bytes
                $valid = (
                    ([int] $response.StatusCode) -eq 206 -and
                    $bytes.Length -eq 1024 -and
                    $contentRange -and
                    ([int64] $contentRange.From) -eq 0 -and
                    ([int64] $contentRange.To) -eq 1023 -and
                    ([int64] $contentRange.Length) -eq $expected -and
                    [Text.Encoding]::ASCII.GetString($bytes, 0, 2) -eq 'PK'
                )
                [pscustomobject]@{
                    Part = $index + 1
                    Status = [int] $response.StatusCode
                    ExpectedBytes = $expected
                    ContentRange = if ($contentRange) {
                        $contentRange.ToString()
                    } else {
                        ''
                    }
                    ZipSignature = (
                        [Text.Encoding]::ASCII.GetString($bytes, 0, 2) -eq 'PK'
                    )
                    Pass = $valid
                }
            }
            finally {
                $response.Dispose()
                $request.Dispose()
            }
        }
    }
    finally {
        $client.Dispose()
        $handler.Dispose()
    }

    $failed = @($results | Where-Object { -not $_.Pass })
    if ($failed.Count -gt 0) {
        $failed | Format-Table -AutoSize
        throw "$($failed.Count) parte(s) do catalogo falharam."
    }

    [pscustomobject]@{
        ReserveMode = [string] $reserve.mode
        ActiveMode = [string] $trial.mode
        TrialSeconds = [int] $reserve.trial_seconds
        ActiveSeconds = [int] $trial.expires_in
        Parts = $results.Count
        DownloadBytes = $declaredTotal
        RangeBytesVerified = ($results.Count * 1024)
        Passed = $results.Count
    }
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
