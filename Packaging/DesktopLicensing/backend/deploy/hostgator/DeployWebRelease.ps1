[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9._-]{1,64}$')]
    [string] $Release,

    [Parameter(Mandatory = $true)]
    [string] $Archive,

    [Parameter(Mandatory = $true)]
    [string] $DeployTokenFile,

    [Parameter(Mandatory = $true)]
    [string] $NetrcPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9.-]+$')]
    [string] $FtpsHost,

    [ValidateRange(1, 65535)]
    [int] $FtpsPort = 21,

    [ValidatePattern('^/(?:[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)*)?$')]
    [string] $RemoteWebRoot = '/',

    [ValidatePattern('^https://[A-Za-z0-9.-]+$')]
    [string] $PublicBaseUrl = 'https://churchdaw.com',

    [switch] $ExpectCheckoutEnabled
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

Add-Type -AssemblyName System.IO.Compression.FileSystem
$utf8NoBom = [Text.UTF8Encoding]::new($false, $true)
$writeUtf8 = [Text.UTF8Encoding]::new($false)
$curl = (Get-Command curl.exe -CommandType Application -ErrorAction Stop |
    Select-Object -First 1).Source

function ConvertTo-LowerHex {
    param([Parameter(Mandatory = $true)] [byte[]] $Bytes)

    return ([BitConverter]::ToString($Bytes)).Replace('-', '').ToLowerInvariant()
}

function New-RandomBytes {
    param([ValidateRange(1, 4096)] [int] $Count)

    $bytes = [byte[]]::new($Count)
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) }
    finally { $rng.Dispose() }
    # Impede o pipeline do Windows PowerShell 5 de desembrulhar byte[] em
    # Object[], o que quebraria Buffer.BlockCopy e as APIs criptograficas.
    return ,$bytes
}

function Get-FileRecord {
    param([Parameter(Mandatory = $true)] [string] $Path)

    $item = Get-Item -LiteralPath $Path
    return [pscustomobject] @{
        Bytes = [long] $item.Length
        Sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).
            Hash.ToLowerInvariant()
    }
}

function Invoke-Curl {
    param(
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [Parameter(Mandatory = $true)] [string] $Label,
        [switch] $AllowFailure
    )

    $escaped = foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') {
            '"' + ($argument -replace '"', '\"') + '"'
        }
        else { $argument }
    }
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $curl
    $startInfo.Arguments = $escaped -join ' '
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "$Label nao iniciou" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(180000)) {
            try { $process.Kill() } catch {}
            throw "$Label excedeu 180 segundos"
        }
        $process.WaitForExit()
        $stdout = [string] $stdoutTask.GetAwaiter().GetResult()
        $stderr = [string] $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -and -not $AllowFailure) {
            $safeError = $stderr.Replace($NetrcPath, '<netrc>')
            throw "$Label falhou (curl $($process.ExitCode)): $safeError"
        }
        return [pscustomobject] @{
            ExitCode = [int] $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
        }
    }
    finally { $process.Dispose() }
}

function Get-FtpsUrl {
    param([Parameter(Mandatory = $true)] [string] $Name)

    if ($Name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,159}$' -or
        $Name.Contains('..') -or [IO.Path]::GetFileName($Name) -cne $Name) {
        throw "Nome remoto inseguro: $Name"
    }
    $prefix = if ($RemoteWebRoot -eq '/') { '' } else { $RemoteWebRoot }
    return "ftp://${FtpsHost}:$FtpsPort$prefix/$Name"
}

function Send-FtpsFile {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] [string] $Name
    )

    $null = Invoke-Curl -Label "upload $Name" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '180',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath,
        '--upload-file', $Path, (Get-FtpsUrl $Name)
    )
}

function Receive-FtpsFile {
    param(
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] [string] $Output
    )

    $null = Invoke-Curl -Label "download $Name" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '180',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath,
        '--output', $Output, (Get-FtpsUrl $Name)
    )
}

function Remove-FtpsFile {
    param([Parameter(Mandatory = $true)] [string] $Name)

    $remotePath = if ($RemoteWebRoot -eq '/') {
        "/$Name"
    }
    else { "$RemoteWebRoot/$Name" }
    $directoryUrl = if ($RemoteWebRoot -eq '/') {
        "ftp://${FtpsHost}:$FtpsPort/"
    }
    else { "ftp://${FtpsHost}:$FtpsPort$RemoteWebRoot/" }
    $result = Invoke-Curl -AllowFailure -Label "remover $Name" -Arguments @(
        '--disable', '--silent', '--show-error',
        '--connect-timeout', '30', '--max-time', '60',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath,
        '--quote', "DELE $remotePath",
        '--list-only', $directoryUrl
    )
    return $result.ExitCode -eq 0
}

function Assert-FtpsRoundTrip {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] [string] $Probe
    )

    Receive-FtpsFile $Name $Probe
    $expected = Get-FileRecord $Path
    $actual = Get-FileRecord $Probe
    if ($actual.Bytes -ne $expected.Bytes -or
        $actual.Sha256 -cne $expected.Sha256) {
        throw "Round-trip FTPS divergiu para $Name"
    }
}

function Invoke-JsonRequest {
    param(
        [Parameter(Mandatory = $true)] [string] $Url,
        [Parameter(Mandatory = $true)] [string] $BodyPath,
        [Parameter(Mandatory = $true)] [string] $HeadersPath,
        [Parameter(Mandatory = $true)] [string] $ResponsePath,
        [Parameter(Mandatory = $true)] [int] $ExpectedStatus,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    $result = Invoke-Curl -AllowFailure -Label $Label -Arguments @(
        '--disable', '--silent', '--show-error', '--location',
        '--proto', '=https', '--tlsv1.2',
        '--connect-timeout', '30', '--max-time', '180',
        '--request', 'POST', '--header', "@$HeadersPath",
        '--data-binary', "@$BodyPath", '--output', $ResponsePath,
        '--write-out', '%{http_code}', $Url
    )
    $status = 0
    if (-not [int]::TryParse($result.Stdout.Trim(), [ref] $status) -or
        $result.ExitCode -ne 0 -or $status -ne $ExpectedStatus) {
        $body = if (Test-Path -LiteralPath $ResponsePath) {
            [IO.File]::ReadAllText($ResponsePath, $utf8NoBom)
        }
        else { '' }
        throw "$Label retornou curl=$($result.ExitCode) HTTP=$status body=$body"
    }
    try {
        return [IO.File]::ReadAllText($ResponsePath, $utf8NoBom) |
            ConvertFrom-Json
    }
    catch { throw "$Label retornou JSON invalido" }
}

function Get-PublicJson {
    param(
        [Parameter(Mandatory = $true)] [string] $Url,
        [Parameter(Mandatory = $true)] [string] $Output,
        [Parameter(Mandatory = $true)] [string] $Label
    )

    $result = Invoke-Curl -AllowFailure -Label $Label -Arguments @(
        '--disable', '--silent', '--show-error', '--location',
        '--proto', '=https', '--tlsv1.2',
        '--connect-timeout', '30', '--max-time', '60',
        '--header', 'Cache-Control: no-cache', '--output', $Output,
        '--write-out', '%{http_code}', $Url
    )
    $status = 0
    if (-not [int]::TryParse($result.Stdout.Trim(), [ref] $status) -or
        $result.ExitCode -ne 0 -or $status -ne 200) {
        throw "$Label retornou curl=$($result.ExitCode) HTTP=$status"
    }
    try {
        return [IO.File]::ReadAllText($Output, $utf8NoBom) | ConvertFrom-Json
    }
    catch { throw "$Label retornou JSON invalido" }
}

foreach ($required in @($Archive, $DeployTokenFile, $NetrcPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $required"
    }
}
$archiveRecord = Get-FileRecord $Archive
if ($archiveRecord.Bytes -lt 1024 -or $archiveRecord.Bytes -gt 8MB) {
    throw 'Archive fora do intervalo permitido pelo bootstrap'
}
$deployToken = [IO.File]::ReadAllText($DeployTokenFile, $utf8NoBom).Trim()
if ($deployToken -notmatch '^[A-Za-z0-9_-]{64}$') {
    throw 'Token de deploy invalido'
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-web-deploy-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempRoot | Out-Null
$stageSucceeded = $false
$bootstrapUploaded = $false
$encryptedUploaded = $false
$bootstrapName = ''
$encryptedName = ''
try {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $entries = @($zip.Entries | Where-Object {
            ($_.FullName.Replace('\', '/')) -match
                '^public_html/(?<name>churchdaw-deploy-[a-f0-9]{24}\.php)$'
        })
        if ($entries.Count -ne 1) {
            throw "Archive precisa conter um bootstrap; encontrou $($entries.Count)"
        }
        $bootstrapName = [regex]::Match(
            $entries[0].FullName.Replace('\', '/'),
            '^public_html/(?<name>churchdaw-deploy-[a-f0-9]{24}\.php)$'
        ).Groups['name'].Value
        $bootstrapPath = Join-Path $tempRoot $bootstrapName
        $source = $entries[0].Open()
        $target = [IO.File]::Open(
            $bootstrapPath, [IO.FileMode]::CreateNew,
            [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $source.CopyTo($target) }
        finally { $target.Dispose(); $source.Dispose() }
    }
    finally { $zip.Dispose() }

    $tokenBytes = $utf8NoBom.GetBytes($deployToken)
    $encryptionHmac = [Security.Cryptography.HMACSHA256]::new($tokenBytes)
    $authenticationHmac = [Security.Cryptography.HMACSHA256]::new($tokenBytes)
    try {
        $encryptionKey = $encryptionHmac.ComputeHash(
            $utf8NoBom.GetBytes('archive-encryption'))
        $macKey = $authenticationHmac.ComputeHash(
            $utf8NoBom.GetBytes('archive-authentication'))
    }
    finally {
        $encryptionHmac.Dispose()
        $authenticationHmac.Dispose()
    }
    $iv = New-RandomBytes 16
    $plain = [IO.File]::ReadAllBytes($Archive)
    $aes = [Security.Cryptography.Aes]::Create()
    try {
        $aes.Mode = [Security.Cryptography.CipherMode]::CBC
        $aes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
        $aes.Key = $encryptionKey
        $aes.IV = $iv
        $encryptor = $aes.CreateEncryptor()
        try { $ciphertext = $encryptor.TransformFinalBlock($plain, 0, $plain.Length) }
        finally { $encryptor.Dispose() }
    }
    finally { $aes.Dispose() }

    $macInput = [byte[]]::new($iv.Length + $ciphertext.Length)
    [Buffer]::BlockCopy($iv, 0, $macInput, 0, $iv.Length)
    [Buffer]::BlockCopy($ciphertext, 0, $macInput, $iv.Length, $ciphertext.Length)
    $macHmac = [Security.Cryptography.HMACSHA256]::new($macKey)
    try { $mac = $macHmac.ComputeHash($macInput) }
    finally { $macHmac.Dispose() }

    # Prova local de cifragem antes de qualquer upload.
    $decryptAes = [Security.Cryptography.Aes]::Create()
    try {
        $decryptAes.Mode = [Security.Cryptography.CipherMode]::CBC
        $decryptAes.Padding = [Security.Cryptography.PaddingMode]::PKCS7
        $decryptAes.Key = $encryptionKey
        $decryptAes.IV = $iv
        $decryptor = $decryptAes.CreateDecryptor()
        try {
            $roundTrip = $decryptor.TransformFinalBlock(
                $ciphertext, 0, $ciphertext.Length)
        }
        finally { $decryptor.Dispose() }
    }
    finally { $decryptAes.Dispose() }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $roundTripHash = ConvertTo-LowerHex $sha.ComputeHash($roundTrip) }
    finally { $sha.Dispose() }
    if ($roundTrip.Length -ne $plain.Length -or
        $roundTripHash -cne $archiveRecord.Sha256) {
        throw 'Prova local da cifragem falhou'
    }

    $encryptedName = 'churchdaw-stage-' +
        (ConvertTo-LowerHex (New-RandomBytes 12)) + '.bin'
    $encryptedPath = Join-Path $tempRoot $encryptedName
    [IO.File]::WriteAllBytes($encryptedPath, $ciphertext)
    $stageBodyPath = Join-Path $tempRoot 'stage.json'
    $activateBodyPath = Join-Path $tempRoot 'activate.json'
    $cleanupBodyPath = Join-Path $tempRoot 'cleanup.json'
    [IO.File]::WriteAllText($stageBodyPath, ([ordered] @{
        action = 'stage-file'
        archive_sha256 = $archiveRecord.Sha256
        encrypted_name = $encryptedName
        iv = [Convert]::ToBase64String($iv)
        mac = [Convert]::ToBase64String($mac)
    } | ConvertTo-Json -Compress), $writeUtf8)
    [IO.File]::WriteAllText(
        $activateBodyPath, '{"action":"activate"}', $writeUtf8)
    [IO.File]::WriteAllText(
        $cleanupBodyPath, '{"action":"cleanup"}', $writeUtf8)
    $headersPath = Join-Path $tempRoot 'headers.txt'
    [IO.File]::WriteAllText(
        $headersPath,
        "X-ChurchDAW-Deploy: $deployToken`r`nContent-Type: application/json`r`n",
        $writeUtf8)

    $preHealthPath = Join-Path $tempRoot 'health-before.json'
    $preHealth = Get-PublicJson "$PublicBaseUrl/api/v1/health" `
        $preHealthPath 'health anterior'
    if ([string] $preHealth.release -ceq $Release) {
        throw 'Release candidata ja esta ativa antes do deploy'
    }

    Send-FtpsFile $bootstrapPath $bootstrapName
    $bootstrapUploaded = $true
    Assert-FtpsRoundTrip $bootstrapPath $bootstrapName `
        (Join-Path $tempRoot 'bootstrap-roundtrip.php')
    Send-FtpsFile $encryptedPath $encryptedName
    $encryptedUploaded = $true
    Assert-FtpsRoundTrip $encryptedPath $encryptedName `
        (Join-Path $tempRoot 'archive-roundtrip.bin')

    $endpoint = "$PublicBaseUrl/$bootstrapName"
    $stageReply = Invoke-JsonRequest $endpoint $stageBodyPath $headersPath `
        (Join-Path $tempRoot 'stage-response.json') 200 'stage'
    if ($stageReply.ok -ne $true -or $stageReply.staged -ne $true -or
        [string] $stageReply.release -cne $Release -or
        [string] $stageReply.archive_sha256 -cne $archiveRecord.Sha256) {
        throw 'Resposta de stage incoerente'
    }
    $stageSucceeded = $true
    $encryptedUploaded = $false

    $activateReply = Invoke-JsonRequest $endpoint $activateBodyPath `
        $headersPath (Join-Path $tempRoot 'activate-response.json') `
        200 'activate'
    $expectedCheckout = [bool] $ExpectCheckoutEnabled
    $migrations = @($activateReply.migrations)
    if ($activateReply.ok -ne $true -or
        [string] $activateReply.release -cne $Release -or
        [bool] $activateReply.checkout_enabled -ne $expectedCheckout -or
        $migrations -cnotcontains '016_woovi_checkout_plans.sql') {
        throw 'Resposta de activate incoerente'
    }

    $postHealth = Get-PublicJson `
        "$PublicBaseUrl/api/v1/health?release=$Release" `
        (Join-Path $tempRoot 'health-after.json') 'health posterior'
    if ([string] $postHealth.release -cne $Release -or
        [bool] $postHealth.checkout_enabled -ne $expectedCheckout -or
        $null -eq $postHealth.schema -or
        [string] $postHealth.schema.current -cne
            '016_woovi_checkout_plans.sql' -or
        [string] $postHealth.schema.expected -cne
            '016_woovi_checkout_plans.sql' -or
        [bool] $postHealth.schema.ready -ne $true) {
        throw 'Health posterior incoerente'
    }

    $cleanupReply = Invoke-JsonRequest $endpoint $cleanupBodyPath `
        $headersPath (Join-Path $tempRoot 'cleanup-response.json') `
        200 'cleanup'
    if ($cleanupReply.ok -ne $true -or $cleanupReply.removed -ne $true) {
        throw 'Bootstrap nao confirmou a propria remocao'
    }
    $bootstrapUploaded = $false

    [pscustomobject] @{
        Ok = $true
        PreviousRelease = [string] $preHealth.release
        Release = $Release
        CheckoutEnabled = $expectedCheckout
        ArchiveBytes = $archiveRecord.Bytes
        ArchiveSha256 = $archiveRecord.Sha256
        BootstrapRemoved = $true
        MigrationCount = $migrations.Count
    }
}
catch {
    if ($encryptedUploaded -and $encryptedName -ne '') {
        try { $null = Remove-FtpsFile $encryptedName } catch {}
    }
    if ($bootstrapUploaded -and -not $stageSucceeded -and
        $bootstrapName -ne '') {
        try { $null = Remove-FtpsFile $bootstrapName } catch {}
    }
    throw
}
finally {
    if (Test-Path -LiteralPath $tempRoot -PathType Container) {
        $expectedPrefix = [IO.Path]::GetFullPath(
            [IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        $resolved = [IO.Path]::GetFullPath($tempRoot).TrimEnd('\') + '\'
        if ($resolved.StartsWith(
                $expectedPrefix, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($tempRoot) -like
                'churchdaw-web-deploy-*') {
            Remove-Item -LiteralPath $tempRoot -Recurse -Force `
                -ErrorAction SilentlyContinue
        }
    }
    $deployToken = $null
    $tokenBytes = $null
    $encryptionKey = $null
    $macKey = $null
    $plain = $null
    $roundTrip = $null
}
