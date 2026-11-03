[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $WindowsInstaller,

    [Parameter(Mandatory = $true)]
    [string] $MacInstaller,

    # Gerado no Mac somente apos notarizacao, stapler e Gatekeeper aprovados.
    [Parameter(Mandatory = $true)]
    [string] $MacVerificationReport,

    [Parameter(Mandatory = $true)]
    [string] $ReleasePolicy,

    [Parameter(Mandatory = $true)]
    [string] $RollbackPolicy,

    [Parameter(Mandatory = $true)]
    [string] $PreviousPolicy,

    [Parameter(Mandatory = $true)]
    [string] $CredentialPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')]
    [string] $Version,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 1000000000)]
    [int] $VersionCode,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 1000000000)]
    [int] $MinimumVersionCode,

    [Parameter(Mandatory = $true)]
    [ValidateRange(2, 1000000000)]
    [int] $PolicySerial,

    [Parameter(Mandatory = $true)]
    [string] $AuditLog,

    [string] $ExpectedWindowsPublisher = '',

    [ValidatePattern('^$|^[0-9a-fA-F]{40}$')]
    [string] $ExpectedWindowsThumbprint = '',

    [switch] $AllowUnsignedWindowsInstallerForEmergency,

    [ValidateLength(0, 240)]
    [string] $EmergencyReason = '',

    [ValidatePattern('^[A-Za-z0-9.-]+$')]
    [string] $FtpsHost = 'churchdaw.com',

    [ValidateRange(1, 65535)]
    [int] $FtpsPort = 21,

    # A conta FTPS ja entra chrootada em public_html. Usar o caminho absoluto
    # do servidor faz o preflight falhar antes de qualquer upload.
    [string] $RemoteDirectory = '/app/desktop',
    [string] $PublicBaseUrl = 'https://churchdaw.com/app/desktop'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')

# Antes de abrir credenciais ou tocar no servidor: um DMG apenas assinado nao
# pode voltar a entrar na policy (incidente 1.6.30, Unnotarized Developer ID).
Assert-ChurchDAWMacVerification -Installer $MacInstaller `
    -ReportPath $MacVerificationReport -ExpectedVersion $Version


$script:curl = (Get-Command curl.exe -ErrorAction Stop).Source
$script:runToken = [DateTime]::UtcNow.ToString('yyyyMMddHHmmss') + '-' +
    [Guid]::NewGuid().ToString('N').Substring(0, 12)
$script:events = [Collections.Generic.List[object]]::new()
$script:tempRoot = ''
$script:netrcPath = ''
$script:policyPromotionAttempted = $false
$script:rollbackConfirmed = $false
$utf8 = [Text.UTF8Encoding]::new($false)

function Add-AuditEvent {
    param([string] $Step, [string] $Status, [string] $Detail = '')

    $script:events.Add([ordered] @{
        utc = [DateTime]::UtcNow.ToString('o')
        step = $Step
        status = $Status
        detail = $Detail
    })
}

function Get-FileRecord {
    param([string] $Path)

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    [pscustomobject] @{
        name = $item.Name
        bytes = [long] $item.Length
        sha256 = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).
            Hash.ToLowerInvariant()
    }
}

function Assert-Record {
    param([string] $Path, $Expected, [string] $Label)

    $actual = Get-FileRecord $Path
    if ($actual.bytes -ne $Expected.bytes -or
        $actual.sha256 -ne $Expected.sha256) {
        throw ("$Label diverge: $($actual.bytes)/$($actual.sha256) != " +
            "$($Expected.bytes)/$($Expected.sha256)")
    }
}

function Assert-SafeRemoteName {
    param([string] $Name)

    if ($Name -notmatch '^[A-Za-z0-9._+-]{1,180}$' -or
        $Name.Contains('..')) {
        throw "Nome remoto inseguro: $Name"
    }
}

function Get-FtpsDirectoryUrl {
    return "ftp://$FtpsHost`:$FtpsPort$RemoteDirectory/"
}

function Invoke-Curl {
    param([string[]] $Arguments, [string] $Label, [switch] $AllowFailure)

    $output = @(& $script:curl @Arguments 2>&1)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        $safeOutput = @($output | ForEach-Object {
            ([string] $_).Replace($script:netrcPath, '<netrc>')
        }) -join '; '
        throw "$Label falhou (curl $exitCode): $safeOutput"
    }
    [pscustomobject] @{ ExitCode = $exitCode; Output = $output }
}

function Get-FtpsNames {
    $output = Join-Path $script:tempRoot (
        'list-' + [Guid]::NewGuid().ToString('N') + '.txt')
    $null = Invoke-Curl -Label 'listar diretorio FTPS' -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '90',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $script:netrcPath,
        '--output', $output, '--list-only', (Get-FtpsDirectoryUrl)
    )
    @([IO.File]::ReadAllLines($output) | ForEach-Object {
        $line = ([string] $_).Trim()
        if ($line) {
            (($line -split '\s+')[-1] -split '[/\\]')[-1].Trim()
        }
    } | Where-Object { $_ -and $_ -ne '.' -and $_ -ne '..' } |
        Sort-Object -Unique)
}

function Test-FtpsFileExists {
    param([string] $Name)

    Assert-SafeRemoteName $Name
    return @((Get-FtpsNames) | Where-Object {
        $_.Equals($Name, [StringComparison]::OrdinalIgnoreCase)
    }).Count -gt 0
}

function Upload-FtpsFile {
    param([string] $Path, [string] $Name, [int] $MaxTimeSeconds = 900)

    Assert-SafeRemoteName $Name
    $url = (Get-FtpsDirectoryUrl) + [Uri]::EscapeDataString($Name)
    $null = Invoke-Curl -Label "upload $Name" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', "$MaxTimeSeconds",
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $script:netrcPath,
        '--upload-file', $Path, $url
    )
}

function Download-FtpsFile {
    param([string] $Name, [string] $Output, [int] $MaxTimeSeconds = 900)

    Assert-SafeRemoteName $Name
    $url = (Get-FtpsDirectoryUrl) + [Uri]::EscapeDataString($Name)
    $null = Invoke-Curl -Label "download $Name" -Arguments @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', "$MaxTimeSeconds",
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $script:netrcPath,
        '--output', $Output, $url
    )
}

function Invoke-FtpsQuote {
    param([string[]] $Commands, [string] $Label)

    $arguments = @(
        '--disable', '--silent', '--show-error', '--fail',
        '--connect-timeout', '30', '--max-time', '90',
        '--insecure', '--ssl-reqd', '--tlsv1.2', '--ftp-pasv',
        '--netrc-file', $script:netrcPath
    )
    foreach ($command in $Commands) {
        $arguments += @('--quote', $command)
    }
    $arguments += @('--list-only', (Get-FtpsDirectoryUrl))
    $null = Invoke-Curl -Label $Label -Arguments $arguments
}

function Rename-FtpsFile {
    param([string] $From, [string] $To)

    Assert-SafeRemoteName $From
    Assert-SafeRemoteName $To
    $prefix = if ($RemoteDirectory -eq '/') { '' } else { $RemoteDirectory }
    Invoke-FtpsQuote @("RNFR $prefix/$From", "RNTO $prefix/$To") `
        "rename $From -> $To"
}

function Remove-FtpsFile {
    param([string] $Name)

    Assert-SafeRemoteName $Name
    if (Test-FtpsFileExists $Name) {
        $prefix = if ($RemoteDirectory -eq '/') { '' } else { $RemoteDirectory }
        Invoke-FtpsQuote @("DELE $prefix/$Name") "remover temporario $Name"
    }
}

function Assert-FtpsRecord {
    param([string] $Name, $Expected, [string] $Label)

    if (-not (Test-FtpsFileExists $Name)) {
        throw "$Label nao existe no FTPS"
    }
    $download = Join-Path $script:tempRoot (
        'verify-' + [Guid]::NewGuid().ToString('N') + '-' + $Name)
    Download-FtpsFile $Name $download
    Assert-Record $download $Expected $Label
}

function Download-HttpsExact {
    param([string] $Name, $Expected, [string] $Label, [int] $Attempts = 20)

    Assert-SafeRemoteName $Name
    for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
        $output = Join-Path $script:tempRoot (
            "https-$attempt-" + [Guid]::NewGuid().ToString('N') + '-' + $Name)
        $url = "$PublicBaseUrl/$Name`?churchdaw_release_probe=$($script:runToken)-$attempt"
        $result = Invoke-Curl -AllowFailure -Label "$Label tentativa $attempt" `
            -Arguments @(
                '--disable', '--silent', '--show-error', '--fail', '--location',
                '--connect-timeout', '30', '--max-time', '900',
                '--header', 'Cache-Control: no-cache',
                '--output', $output, $url
            )
        if ($result.ExitCode -eq 0 -and
            (Test-Path -LiteralPath $output -PathType Leaf)) {
            try {
                Assert-Record $output $Expected $Label
                return $output
            }
            catch {
                if ($attempt -eq $Attempts) { throw }
            }
        }
        if ($attempt -lt $Attempts) { Start-Sleep -Seconds 2 }
    }
    throw "$Label nao convergiu em $Attempts tentativas"
}

function Publish-VersionedFile {
    param([string] $Path)

    $record = Get-FileRecord $Path
    Assert-SafeRemoteName $record.name
    if (Test-FtpsFileExists $record.name) {
        Assert-FtpsRecord $record.name $record "arquivo remoto $($record.name)"
        Add-AuditEvent 'artifact' 'reused-identical' $record.name
        return $record
    }

    $temporary = "$($record.name).tmp.$($script:runToken)"
    if (Test-FtpsFileExists $temporary) {
        throw "Temporario remoto preexistente: $temporary"
    }
    Upload-FtpsFile $Path $temporary
    try {
        Assert-FtpsRecord $temporary $record "round-trip $($record.name)"
        if (Test-FtpsFileExists $record.name) {
            Assert-FtpsRecord $record.name $record "arquivo surgido $($record.name)"
            Remove-FtpsFile $temporary
            Add-AuditEvent 'artifact' 'reused-raced-identical' $record.name
            return $record
        }
        Rename-FtpsFile $temporary $record.name
    }
    catch {
        try { Remove-FtpsFile $temporary } catch { }
        throw
    }
    Assert-FtpsRecord $record.name $record "arquivo final $($record.name)"
    Add-AuditEvent 'artifact' 'uploaded' $record.name
    return $record
}

function Replace-FixedFile {
    param([string] $Path, [string] $Name, $ExpectedCurrent)

    $record = Get-FileRecord $Path
    $temporary = "$Name.tmp.$($script:runToken)"
    if (Test-FtpsFileExists $temporary) {
        throw "Temporario remoto preexistente: $temporary"
    }
    Upload-FtpsFile $Path $temporary 120
    try {
        Assert-FtpsRecord $temporary $record "round-trip $Name"
        Assert-FtpsRecord $Name $ExpectedCurrent "estado anterior de $Name"
        Rename-FtpsFile $temporary $Name
    }
    catch {
        try { Remove-FtpsFile $temporary } catch { }
        throw
    }
    Assert-FtpsRecord $Name $record "estado final de $Name"
    return $record
}

function Invoke-FtpsPreflight {
    $a = "churchdaw-probe-a-$($script:runToken).txt"
    $b = "churchdaw-probe-b-$($script:runToken).txt"
    $final = "churchdaw-probe-final-$($script:runToken).txt"
    $aPath = Join-Path $script:tempRoot $a
    $bPath = Join-Path $script:tempRoot $b
    [IO.File]::WriteAllText($aPath, 'probe-a', $utf8)
    [IO.File]::WriteAllText($bPath, 'probe-b', $utf8)
    try {
        foreach ($name in @($a, $b, $final)) {
            if (Test-FtpsFileExists $name) {
                throw "Probe remoto ja existe: $name"
            }
        }
        Upload-FtpsFile $aPath $a 120
        Rename-FtpsFile $a $final
        Assert-FtpsRecord $final (Get-FileRecord $aPath) 'probe rename'
        Upload-FtpsFile $bPath $b 120
        Rename-FtpsFile $b $final
        Assert-FtpsRecord $final (Get-FileRecord $bPath) 'probe replace'
        Add-AuditEvent 'ftps-preflight' 'passed' $RemoteDirectory
    }
    finally {
        foreach ($name in @($a, $b, $final)) {
            try { Remove-FtpsFile $name } catch { }
        }
    }
}

function Read-PolicyPayload {
    param([string] $Path)

    $envelope = [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8) |
        ConvertFrom-Json
    if ($envelope.schema_version -ne 1 -or
        $envelope.algorithm -cne 'SHA256withRSA' -or
        ($envelope.key_id -cne 'churchdaw-update-2026-01' -and $envelope.key_id -cne 'churchdaw-update-2026-08')) {
        throw "Envelope de policy invalido: $Path"
    }
    $json = [Text.Encoding]::UTF8.GetString(
        [Convert]::FromBase64String([string] $envelope.payload))
    return $json | ConvertFrom-Json
}

function Assert-PolicyInputs {
    param($WindowsRecord, $MacRecord)

    $previous = Read-PolicyPayload $PreviousPolicy
    $release = Read-PolicyPayload $ReleasePolicy
    $rollback = Read-PolicyPayload $RollbackPolicy
    if ([int] $previous.policy_serial -ne $PolicySerial - 1) {
        throw 'Serial da policy anterior inesperado'
    }
    if ([int] $release.policy_serial -ne $PolicySerial -or
        [int] $release.latest_version_code -ne $VersionCode -or
        [int] $release.minimum_version_code -ne $MinimumVersionCode -or
        [string] $release.latest_version_name -cne $Version -or
        [string] $release.mode -cne 'normal') {
        throw 'Payload da policy de release nao corresponde aos parametros'
    }
    if ([string] $release.windows_installer_url -cne
            "$PublicBaseUrl/$($WindowsRecord.name)" -or
        [string] $release.windows_installer_sha256 -cne $WindowsRecord.sha256 -or
        [long] $release.windows_installer_bytes -ne $WindowsRecord.bytes -or
        [string] $release.mac_installer_url -cne
            "$PublicBaseUrl/$($MacRecord.name)" -or
        [string] $release.mac_installer_sha256 -cne $MacRecord.sha256 -or
        [long] $release.mac_installer_bytes -ne $MacRecord.bytes) {
        throw 'Hashes, tamanhos ou URLs da policy nao correspondem aos instaladores'
    }
    if ([int] $rollback.policy_serial -ne $PolicySerial + 1 -or
        [string] $rollback.mode -cne 'rollback' -or
        [int] $rollback.revokes_policy_serial -ne $PolicySerial) {
        throw 'Policy de rollback nao revoga corretamente a release'
    }
}

function Write-AuditLog {
    param([string] $Status, [string] $Failure = '')

    $parent = Split-Path -Parent ([IO.Path]::GetFullPath($AuditLog))
    if ($parent) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
    $document = [ordered] @{
        schema_version = 1
        status = $Status
        failure = $Failure
        version = $Version
        version_code = $VersionCode
        policy_serial = $PolicySerial
        minimum_version_code = $MinimumVersionCode
        rollback_confirmed = $script:rollbackConfirmed
        events = @($script:events)
    }
    [IO.File]::WriteAllText(
        [IO.Path]::GetFullPath($AuditLog),
        ($document | ConvertTo-Json -Depth 8),
        $utf8)
}

$inputs = @($WindowsInstaller, $MacInstaller, $ReleasePolicy,
    $RollbackPolicy, $PreviousPolicy, $CredentialPath)
foreach ($path in $inputs) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Entrada ausente: $path"
    }
}

# Este publicador e uma rota direta para producao e, portanto, aplica a mesma
# garantia Authenticode do pipeline comercial completo. A unica excecao e uma
# recuperacao explicitamente auditada para voltar ao modelo historico de
# policy RS256 + SHA-256 quando um certificado de teste bloqueia o updater.
$rawWindowsSignature = Get-AuthenticodeSignature -LiteralPath $WindowsInstaller
$windowsSignature = $null
if ($AllowUnsignedWindowsInstallerForEmergency) {
    if ([string]::IsNullOrWhiteSpace($EmergencyReason)) {
        throw 'A excecao unsigned exige -EmergencyReason'
    }
    if ($rawWindowsSignature.Status -ne 'NotSigned') {
        throw ('A excecao unsigned aceita somente arquivo realmente sem ' +
               "assinatura; status atual: $($rawWindowsSignature.Status)")
    }
}
else {
    if ([string]::IsNullOrWhiteSpace($ExpectedWindowsPublisher) -or
        [string]::IsNullOrWhiteSpace($ExpectedWindowsThumbprint)) {
        throw ('Publicacao normal exige -ExpectedWindowsPublisher e ' +
               '-ExpectedWindowsThumbprint')
    }
    $windowsSignature = Assert-ChurchDAWAuthenticodeSignature `
        -Path $WindowsInstaller `
        -ExpectedPublisher $ExpectedWindowsPublisher `
        -ExpectedSignerThumbprints @($ExpectedWindowsThumbprint) `
        -RequireTimestamp $true `
        -RequirePublicChain $true
}

$windowsRecord = Get-FileRecord $WindowsInstaller
$macRecord = Get-FileRecord $MacInstaller
$previousRecord = Get-FileRecord $PreviousPolicy
$releaseRecord = Get-FileRecord $ReleasePolicy
$rollbackRecord = Get-FileRecord $RollbackPolicy
$expectedWindowsName = "ChurchDAW-$Version-Setup.exe"
$emergencyWindowsNamePattern = '^ChurchDAW-' +
    [regex]::Escape($Version) + '-r[1-9][0-9]*-Setup\.exe$'
$windowsNameAccepted = $windowsRecord.name -ceq $expectedWindowsName -or
    ($AllowUnsignedWindowsInstallerForEmergency -and
     $windowsRecord.name -cmatch $emergencyWindowsNamePattern)
if (-not $windowsNameAccepted -or
    $macRecord.name -cne "ChurchDAW-$Version.dmg") {
    throw 'Os nomes dos instaladores nao correspondem a versao solicitada'
}
Assert-PolicyInputs $windowsRecord $macRecord

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$script:tempRoot = Join-Path $tempBase (
    'ChurchDAW-publish-local-' + $script:runToken)
[IO.Directory]::CreateDirectory($script:tempRoot) | Out-Null
$script:netrcPath = Join-Path $script:tempRoot 'curl.netrc'

$failure = ''
try {
    $usedNetrc = $false
    try {
        $credential = Import-Clixml -LiteralPath $CredentialPath
        if ($credential -is [Management.Automation.PSCredential]) {
            $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR(
                $credential.Password)
            try {
                $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
                $quote = {
                    param([string] $Value)
                    '"' + $Value.Replace('\', '\\').Replace('"', '\"') + '"'
                }
                $netrc = 'machine ' + $FtpsHost + ' login ' +
                    (& $quote $credential.UserName) + ' password ' + (& $quote $password)
                [IO.File]::WriteAllText($script:netrcPath, $netrc, $utf8)
                $password = $null
                $usedNetrc = $true
            }
            finally {
                [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
            }
        }
    }
    catch {
        $fallbackNetrcs = @(
            (Join-Path $env:USERPROFILE '_netrc'),
            (Join-Path $env:USERPROFILE '.netrc')
        )
        foreach ($f in $fallbackNetrcs) {
            if (Test-Path $f) {
                Copy-Item $f $script:netrcPath -Force
                $usedNetrc = $true
                break
            }
        }
        if (-not $usedNetrc) {
            throw "Nao foi possivel carregar credenciais FTPS: $($_.Exception.Message)"
        }
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $null = & icacls.exe $script:netrcPath '/inheritance:r' "/grant:r" `
        "$identity`:(F)"
    if ($LASTEXITCODE -ne 0) { throw 'Nao foi possivel restringir o netrc' }

    Add-AuditEvent 'inputs' 'verified' (
        "windows=$($windowsRecord.sha256);mac=$($macRecord.sha256);" +
        "previous=$($previousRecord.sha256);release=$($releaseRecord.sha256)")
    if ($AllowUnsignedWindowsInstallerForEmergency) {
        Add-AuditEvent 'windows-authenticode' 'emergency-unsigned' `
            $EmergencyReason.Trim()
    }
    else {
        Add-AuditEvent 'windows-authenticode' 'verified' (
            "publisher=$($windowsSignature.Publisher);" +
            "thumbprint=$($windowsSignature.SignerThumbprint);" +
            "timestamped=$($windowsSignature.TimeStamped)")
    }
    Invoke-FtpsPreflight

    Assert-FtpsRecord 'update-policy-v1.json' $previousRecord `
        'policy publica anterior no FTPS'
    $null = Download-HttpsExact 'update-policy-v1.json' $previousRecord `
        'policy publica anterior no HTTPS'

    $publishedWindows = Publish-VersionedFile $WindowsInstaller
    $publishedMac = Publish-VersionedFile $MacInstaller
    $null = Download-HttpsExact $publishedWindows.name $publishedWindows `
        'instalador Windows publico'
    $null = Download-HttpsExact $publishedMac.name $publishedMac `
        'instalador macOS publico'

    Assert-FtpsRecord 'update-policy-v1.json' $previousRecord `
        'policy imediatamente antes da promocao no FTPS'
    $null = Download-HttpsExact 'update-policy-v1.json' $previousRecord `
        'policy imediatamente antes da promocao no HTTPS'

    $script:policyPromotionAttempted = $true
    $null = Replace-FixedFile $ReleasePolicy 'update-policy-v1.json' `
        $previousRecord
    $null = Download-HttpsExact 'update-policy-v1.json' $releaseRecord `
        'policy serial nova no HTTPS'
    Add-AuditEvent 'policy-last' 'confirmed' (
        "serial=$PolicySerial;sha=$($releaseRecord.sha256)")
    Write-AuditLog 'published'

    Write-Host 'PUBLISH_LOCAL_DESKTOP_RELEASE_OK'
    Write-Host "Windows: $($windowsRecord.name) $($windowsRecord.sha256)"
    Write-Host "macOS: $($macRecord.name) $($macRecord.sha256)"
    Write-Host "Policy serial: $PolicySerial $($releaseRecord.sha256)"
}
catch {
    $failure = $_.Exception.Message
    Add-AuditEvent 'publisher' 'failed' $failure
    if ($script:policyPromotionAttempted) {
        try {
            $active = Join-Path $script:tempRoot 'active-policy-on-failure.json'
            Download-FtpsFile 'update-policy-v1.json' $active 120
            $activeRecord = Get-FileRecord $active
            if ($activeRecord.sha256 -notin @(
                    $previousRecord.sha256, $releaseRecord.sha256,
                    $rollbackRecord.sha256)) {
                throw "Policy ativa desconhecida: $($activeRecord.sha256)"
            }
            if ($activeRecord.sha256 -ne $rollbackRecord.sha256) {
                $null = Replace-FixedFile $RollbackPolicy `
                    'update-policy-v1.json' $activeRecord
            }
            $null = Download-HttpsExact 'update-policy-v1.json' $rollbackRecord `
                'policy de rollback no HTTPS'
            $script:rollbackConfirmed = $true
            Add-AuditEvent 'rollback' 'confirmed' (
                "serial=$($PolicySerial + 1);sha=$($rollbackRecord.sha256)")
        }
        catch {
            $failure += '; ROLLBACK NAO CONFIRMADO: ' + $_.Exception.Message
            Add-AuditEvent 'rollback' 'critical-failure' $_.Exception.Message
        }
    }
    Write-AuditLog 'failed' $failure
    throw $failure
}
finally {
    if ($script:tempRoot) {
        $resolved = [IO.Path]::GetFullPath($script:tempRoot)
        if ($resolved.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and
            [IO.Path]::GetFileName($resolved).StartsWith(
                'ChurchDAW-publish-local-',
                [StringComparison]::Ordinal)) {
            try { [IO.Directory]::Delete($resolved, $true) } catch { }
        }
    }
}
