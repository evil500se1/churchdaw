[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Candidate,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string] $ExpectedPreviousSha256,

    [Parameter(Mandatory = $true)]
    [string] $NetrcPath,

    [ValidatePattern('^[A-Za-z0-9.-]+$')]
    [string] $FtpsHost = 'churchdaw.com',

    [ValidateRange(1, 65535)]
    [int] $FtpsPort = 21,

    [string] $PublicUrl = 'https://churchdaw.com/'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

foreach ($required in @($Candidate, $NetrcPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Arquivo obrigatorio ausente: $required"
    }
}
if ($PublicUrl -cne 'https://churchdaw.com/') {
    throw 'A publicacao aceita somente a URL oficial do ChurchDAW'
}

$curl = (Get-Command curl.exe -ErrorAction Stop).Source
$runToken = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) "churchdaw-site-$runToken"
New-Item -ItemType Directory -Path $tempRoot -ErrorAction Stop | Out-Null

function Get-Record {
    param([string] $Path)

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    [pscustomobject] @{
        bytes = [long] $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).
            Hash.ToLowerInvariant()
    }
}

function Assert-Record {
    param([string] $Path, $Expected, [string] $Label)

    $actual = Get-Record $Path
    if ($actual.bytes -ne $Expected.bytes -or
        $actual.sha256 -cne $Expected.sha256) {
        throw "$Label diverge do arquivo esperado"
    }
}

function Invoke-Curl {
    param(
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [Parameter(Mandatory = $true)] [string] $Label,
        [switch] $AllowFailure
    )

    $output = @(& $curl @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "$Label falhou (curl $exitCode)"
    }
    [pscustomobject] @{ ExitCode = [int] $exitCode; Output = $output }
}

function Get-FtpsUrl {
    param([string] $Name = '')

    $suffix = if ($Name) { [Uri]::EscapeDataString($Name) } else { '' }
    "ftp://$FtpsHost`:$FtpsPort/$suffix"
}

function Get-FtpsArguments {
    @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '120',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $NetrcPath
    )
}

function Get-RemoteNames {
    $result = Invoke-Curl -Label 'listar raiz FTPS' -Arguments @(
        (Get-FtpsArguments) + @('--list-only', (Get-FtpsUrl))
    )
    @($result.Output | ForEach-Object {
        $line = ([string] $_).Trim()
        if ($line) { (($line -split '\s+')[-1] -split '[/\\]')[-1] }
    } | Where-Object { $_ -and $_ -ne '.' -and $_ -ne '..' } |
        Sort-Object -Unique)
}

function Test-RemoteFile {
    param([string] $Name)

    @((Get-RemoteNames) | Where-Object {
        $_.Equals($Name, [StringComparison]::Ordinal)
    }).Count -eq 1
}

function Download-Ftps {
    param([string] $Name, [string] $Output)

    $null = Invoke-Curl -Label "baixar $Name" -Arguments @(
        (Get-FtpsArguments) + @('--output', $Output, (Get-FtpsUrl $Name))
    )
}

function Upload-Ftps {
    param([string] $InputPath, [string] $Name)

    $null = Invoke-Curl -Label "enviar $Name" -Arguments @(
        (Get-FtpsArguments) + @('--upload-file', $InputPath,
            (Get-FtpsUrl $Name))
    )
}

function Invoke-FtpsQuote {
    param([string[]] $Commands, [string] $Label)

    $arguments = Get-FtpsArguments
    foreach ($command in $Commands) {
        $arguments += @('--quote', $command)
    }
    $arguments += @('--list-only', (Get-FtpsUrl))
    $null = Invoke-Curl -Label $Label -Arguments $arguments
}

function Rename-Ftps {
    param([string] $From, [string] $To)

    Invoke-FtpsQuote @("RNFR /$From", "RNTO /$To") `
        "renomear $From para $To"
}

function Remove-FtpsIfPresent {
    param([string] $Name)

    if (Test-RemoteFile $Name) {
        Invoke-FtpsQuote @("DELE /$Name") "remover $Name"
    }
}

function Download-Https {
    param([string] $Output, [string] $Probe)

    $null = Invoke-Curl -Label 'baixar site publico' -Arguments @(
        '--disable', '--silent', '--show-error', '--fail', '--location',
        '--proto', '=https', '--tlsv1.2', '--connect-timeout', '30',
        '--max-time', '120', '--header', 'Cache-Control: no-cache',
        '--output', $Output, "$PublicUrl`?churchdaw_site_probe=$Probe"
    )
}

$candidateRecord = Get-Record $Candidate
$expectedPrevious = $ExpectedPreviousSha256.ToLowerInvariant()
$temporaryName = "index.html.candidate-$runToken"
$backupName = "index.html.backup-$runToken-before-group-ui"
$baselineFtps = Join-Path $tempRoot 'index-before-ftps.html'
$baselineHttps = Join-Path $tempRoot 'index-before-https.html'
$roundTrip = Join-Path $tempRoot 'candidate-roundtrip.html'
$finalFtps = Join-Path $tempRoot 'index-final-ftps.html'
$promotedHttps = Join-Path $tempRoot 'index-final-https.html'
$currentRenamed = $false
$candidatePromoted = $false

try {
    if (-not (Test-RemoteFile 'index.html')) {
        throw 'index.html atual nao existe no FTPS'
    }
    if (Test-RemoteFile $temporaryName -or Test-RemoteFile $backupName) {
        throw 'Os nomes seguros desta publicacao ja existem no servidor'
    }

    Download-Ftps 'index.html' $baselineFtps
    $baselineRecord = Get-Record $baselineFtps
    if ($baselineRecord.sha256 -cne $expectedPrevious) {
        throw 'O site FTPS mudou desde a ultima verificacao; publicacao cancelada'
    }

    Download-Https $baselineHttps "$runToken-before"
    Assert-Record $baselineHttps $baselineRecord 'baseline HTTPS'

    Upload-Ftps $Candidate $temporaryName
    Download-Ftps $temporaryName $roundTrip
    Assert-Record $roundTrip $candidateRecord 'round-trip do candidato'

    Rename-Ftps 'index.html' $backupName
    $currentRenamed = $true
    try {
        Rename-Ftps $temporaryName 'index.html'
        $candidatePromoted = $true
    }
    catch {
        Rename-Ftps $backupName 'index.html'
        $currentRenamed = $false
        throw
    }

    Download-Ftps 'index.html' $finalFtps
    Assert-Record $finalFtps $candidateRecord 'index.html final no FTPS'

    $httpsVerified = $false
    for ($attempt = 1; $attempt -le 30; ++$attempt) {
        $result = Invoke-Curl -AllowFailure -Label 'verificar site HTTPS' `
            -Arguments @(
                '--disable', '--silent', '--show-error', '--fail', '--location',
                '--proto', '=https', '--tlsv1.2', '--connect-timeout', '30',
                '--max-time', '60', '--header', 'Cache-Control: no-cache',
                '--output', $promotedHttps,
                "$PublicUrl`?churchdaw_site_probe=$runToken-$attempt"
            )
        if ($result.ExitCode -eq 0 -and
            (Test-Path -LiteralPath $promotedHttps -PathType Leaf)) {
            $publicRecord = Get-Record $promotedHttps
            if ($publicRecord.bytes -eq $candidateRecord.bytes -and
                $publicRecord.sha256 -ceq $candidateRecord.sha256) {
                $httpsVerified = $true
                break
            }
        }
        Start-Sleep -Seconds 2
    }
    if (-not $httpsVerified) {
        throw 'O site publicado nao convergiu no HTTPS'
    }

    [ordered] @{
        status = 'published'
        sha256 = $candidateRecord.sha256
        bytes = $candidateRecord.bytes
        backup = $backupName
        public_url = $PublicUrl
    } | ConvertTo-Json -Compress
}
finally {
    if (-not $candidatePromoted) {
        try { Remove-FtpsIfPresent $temporaryName } catch {}
    }
    if ($currentRenamed -and -not $candidatePromoted -and
        -not (Test-RemoteFile 'index.html') -and
        (Test-RemoteFile $backupName)) {
        Rename-Ftps $backupName 'index.html'
    }
}
