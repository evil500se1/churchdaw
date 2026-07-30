# Assina e verifica um PE Windows com Authenticode.
#
# O modo padrao e Commercial e falha fechado: identidade esperada, cadeia
# publica, EKU Code Signing, timestamp RFC 3161 e verificacao completa do
# signtool sao obrigatorios. Excecoes existem somente em -ReleaseMode Test.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Installer,

    [string] $PfxPath = '',
    [string] $PfxPassword = '',
    [string] $SubjectName = '',
    [string] $Thumbprint = '',
    [switch] $MachineStore,

    [ValidateSet('Commercial', 'Test')]
    [string] $ReleaseMode = 'Commercial',
    [string] $ExpectedPublisher = '',
    [Alias('ExpectedThumbprint')]
    [string[]] $ExpectedSignerThumbprints = @(),

    [string] $TimestampUrl = 'http://timestamp.digicert.com',
    [switch] $SkipTimestamp,
    [switch] $AllowResignForTest,
    [string] $VerificationLogPath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')

if (-not (Test-ChurchDAWIsWindows)) {
    throw 'A assinatura Authenticode precisa rodar no Windows'
}

$installerPath = [IO.Path]::GetFullPath($Installer)
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Binario nao encontrado: $installerPath"
}

$selectors = 0
if (-not [string]::IsNullOrWhiteSpace($PfxPath)) { $selectors++ }
if (-not [string]::IsNullOrWhiteSpace($Thumbprint)) { $selectors++ }
if (-not [string]::IsNullOrWhiteSpace($SubjectName)) { $selectors++ }
if ($selectors -ne 1) {
    throw ('Informe exatamente uma identidade: -PfxPath, -Thumbprint ou ' +
           '-SubjectName')
}
if ($ReleaseMode -eq 'Commercial' -and $AllowResignForTest) {
    throw '-AllowResignForTest existe somente para -ReleaseMode Test'
}

$policy = Assert-ChurchDAWWindowsSigningPolicy `
    -ReleaseMode $ReleaseMode `
    -HasSigningIdentity $true `
    -SkipTimestamp ([bool] $SkipTimestamp) `
    -ExpectedPublisher $ExpectedPublisher `
    -ExpectedSignerThumbprints $ExpectedSignerThumbprints

if (-not $SkipTimestamp) {
    $timestampUri = $null
    if (-not [Uri]::TryCreate(
        $TimestampUrl,
        [UriKind]::Absolute,
        [ref] $timestampUri) -or
        $timestampUri.Scheme -notin @('http', 'https') -or
        [string]::IsNullOrWhiteSpace($timestampUri.Host) -or
        -not [string]::IsNullOrWhiteSpace($timestampUri.UserInfo)) {
        throw "URL de timestamp invalida: $TimestampUrl"
    }
}

$existing = Get-AuthenticodeSignature -LiteralPath $installerPath
if ($existing.Status -ne 'NotSigned' -and -not $AllowResignForTest) {
    throw ('O arquivo ja possui assinatura. Use um artefato limpo; ' +
           '-AllowResignForTest e permitido somente em teste.')
}

$storeLocation = if ($MachineStore) { 'LocalMachine' } else { 'CurrentUser' }
$storePath = "Cert:\$storeLocation\My"
$identityArguments = @()
$selectedCertificate = $null
$selectorDescription = ''
$pfxCertificates = $null

try {
    if (-not [string]::IsNullOrWhiteSpace($Thumbprint)) {
        $requested = ConvertTo-ChurchDAWThumbprint $Thumbprint
        $matches = @(Get-ChildItem -LiteralPath $storePath -ErrorAction Stop |
            Where-Object {
                (ConvertTo-ChurchDAWThumbprint $_.Thumbprint) -eq $requested
            })
        if ($matches.Count -ne 1) {
            throw ("Certificado $requested nao encontrado de forma unica em " +
                   $storePath)
        }
        $selectedCertificate = $matches[0]
        $identityArguments = @('/sha1', $requested, '/s', 'My')
        if ($MachineStore) { $identityArguments += '/sm' }
        $selectorDescription = "$storeLocation/My:$requested"
    }
    elseif (-not [string]::IsNullOrWhiteSpace($SubjectName)) {
        $matches = @(Get-ChildItem -LiteralPath $storePath -ErrorAction Stop |
            Where-Object {
                $_.HasPrivateKey -and
                $_.Subject.IndexOf(
                    $SubjectName,
                    [StringComparison]::OrdinalIgnoreCase) -ge 0
            })
        if ($matches.Count -ne 1) {
            throw ("-SubjectName encontrou $($matches.Count) certificados em " +
                   "$storePath; selecione por -Thumbprint")
        }
        $selectedCertificate = $matches[0]
        $resolvedThumbprint = ConvertTo-ChurchDAWThumbprint `
            $selectedCertificate.Thumbprint
        $identityArguments = @('/sha1', $resolvedThumbprint, '/s', 'My')
        if ($MachineStore) { $identityArguments += '/sm' }
        $selectorDescription = "$storeLocation/My:$resolvedThumbprint"
    }
    else {
        $pfxFull = [IO.Path]::GetFullPath($PfxPath)
        if (-not (Test-Path -LiteralPath $pfxFull -PathType Leaf)) {
            throw "PFX nao encontrado: $pfxFull"
        }

        $pfxCertificates =
            [Security.Cryptography.X509Certificates.X509Certificate2Collection]::new()
        $flagType =
            [Security.Cryptography.X509Certificates.X509KeyStorageFlags]
        $storageFlags = $flagType::DefaultKeySet
        if ([Enum]::GetNames($flagType) -contains 'EphemeralKeySet') {
            $storageFlags = [Enum]::Parse(
                $flagType, 'EphemeralKeySet')
        }
        elseif ($ReleaseMode -eq 'Commercial') {
            throw ('Este .NET nao suporta importacao efemera de PFX. ' +
                   'Use um certificado no Windows Certificate Store por ' +
                   '-Thumbprint ou atualize o .NET antes do release.')
        }
        $pfxCertificates.Import($pfxFull, $PfxPassword, $storageFlags)
        $leafCandidates = @($pfxCertificates | Where-Object {
            $_.HasPrivateKey -and
            (Test-ChurchDAWCertificateHasCodeSigningEku $_)
        })
        if ($leafCandidates.Count -ne 1) {
            throw ("O PFX precisa conter exatamente um certificado Code " +
                   "Signing com chave privada; encontrados: " +
                   $leafCandidates.Count)
        }

        $selectedCertificate = $leafCandidates[0]
        $identityArguments = @('/f', $pfxFull)
        if (-not [string]::IsNullOrEmpty($PfxPassword)) {
            $identityArguments += @('/p', $PfxPassword)
        }
        $selectorDescription = "PFX:$([IO.Path]::GetFileName($pfxFull))"
    }

    $preflightIdentity = Assert-ChurchDAWCertificateIdentity `
        -Certificate $selectedCertificate `
        -ExpectedPublisher $policy.ExpectedPublisher `
        -ExpectedSignerThumbprints $policy.ExpectedSignerThumbprints `
        -RequirePrivateKey $true `
        -RequirePublicChain $false
}
finally {
    if ($pfxCertificates) {
        foreach ($certificate in $pfxCertificates) {
            $certificate.Dispose()
        }
    }
}

$signTool = Find-ChurchDAWSignTool
$signArguments = @(
    'sign',
    '/v',
    '/fd', 'sha256',
    '/d', 'ChurchDAW',
    '/du', 'https://churchdaw.com'
) + $identityArguments

if (-not $SkipTimestamp) {
    $signArguments += @('/tr', $TimestampUrl, '/td', 'sha256')
}
$signArguments += $installerPath

Write-Host "Assinando $([IO.Path]::GetFileName($installerPath))..."
$signResult = Invoke-ChurchDAWNativeCapture -FilePath $signTool `
    -Arguments $signArguments `
    -LogPath $VerificationLogPath `
    -Label "signtool sign $([IO.Path]::GetFileName($installerPath))"
if ($signResult.ExitCode -ne 0) {
    throw "signtool sign falhou (codigo $($signResult.ExitCode))"
}

Write-Host 'Verificando assinatura, timestamp, publisher e cadeia...'
$verified = Assert-ChurchDAWAuthenticodeSignature `
    -Path $installerPath `
    -SignTool $signTool `
    -ExpectedPublisher $policy.ExpectedPublisher `
    -ExpectedSignerThumbprints $policy.ExpectedSignerThumbprints `
    -RequireTimestamp $policy.RequireTimestamp `
    -RequirePublicChain $policy.RequirePublicChain `
    -VerificationLogPath $VerificationLogPath

[pscustomobject] @{
    Installer = $verified.Path
    ReleaseMode = $ReleaseMode
    Status = $verified.Status
    SignatureType = $verified.SignatureType
    Publisher = $verified.Publisher
    SignerThumbprint = $verified.SignerThumbprint
    SignerNotBefore = $verified.SignerNotBefore
    SignerNotAfter = $verified.SignerNotAfter
    SignerChainSubjects = $verified.SignerChainSubjects
    TimeStamped = $verified.TimeStamped
    TimeStamper = $verified.TimeStamper
    TimeStamperThumbprint = $verified.TimeStamperThumbprint
    Sha256 = $verified.Sha256
    Bytes = $verified.Bytes
    SigntoolVerify = 'aceito'
    SigntoolVerifyArguments = if ($policy.RequireTimestamp) {
        '/pa /all /tw /v'
    } else { '/pa /all /v' }
    CertificateSelector = $selectorDescription
    VerificationLog = if ([string]::IsNullOrWhiteSpace(
        $VerificationLogPath)) { '' } else {
        [IO.Path]::GetFullPath($VerificationLogPath)
    }
}
