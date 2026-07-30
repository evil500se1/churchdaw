Set-StrictMode -Version 3.0

$script:ChurchDAWCodeSigningEku = '1.3.6.1.5.5.7.3.3'

function Test-ChurchDAWIsWindows {
    return [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
}

function ConvertTo-ChurchDAWThumbprint {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Thumbprint
    )

    $normalized = ($Thumbprint -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
    if ($normalized.Length -ne 40 -or $normalized -notmatch '^[0-9A-F]{40}$') {
        throw "Thumbprint SHA-1 de certificado invalido: $Thumbprint"
    }

    return $normalized
}

function Assert-ChurchDAWWindowsSigningPolicy {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Commercial', 'Test')]
        [string] $ReleaseMode,

        [Parameter(Mandatory = $true)]
        [bool] $HasSigningIdentity,

        [bool] $AllowUnsignedTestBuild = $false,
        [bool] $SkipTimestamp = $false,
        [string] $ExpectedPublisher = '',
        [string[]] $ExpectedSignerThumbprints = @()
    )

    $normalizedThumbprints = @()
    foreach ($candidate in @($ExpectedSignerThumbprints)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $normalizedThumbprints += ConvertTo-ChurchDAWThumbprint $candidate
        }
    }
    $normalizedThumbprints = @($normalizedThumbprints | Select-Object -Unique)

    if ($ReleaseMode -eq 'Commercial') {
        if (-not $HasSigningIdentity) {
            throw 'Release comercial exige identidade Authenticode real'
        }
        if ($AllowUnsignedTestBuild) {
            throw '-AllowUnsignedTestBuild existe somente para -ReleaseMode Test'
        }
        if ($SkipTimestamp) {
            throw 'Release comercial exige timestamp RFC 3161'
        }
        if ([string]::IsNullOrWhiteSpace($ExpectedPublisher)) {
            throw 'Release comercial exige -ExpectedPublisher exato'
        }
        if ($normalizedThumbprints.Count -eq 0) {
            throw 'Release comercial exige ao menos um -ExpectedSignerThumbprints'
        }
    }
    elseif (-not $HasSigningIdentity -and -not $AllowUnsignedTestBuild) {
        throw ('Build de teste sem assinatura exige a confirmacao explicita ' +
               '-AllowUnsignedTestBuild')
    }

    return [pscustomobject] @{
        ReleaseMode = $ReleaseMode
        ExpectedPublisher = $ExpectedPublisher.Trim()
        ExpectedSignerThumbprints = $normalizedThumbprints
        RequireTimestamp = -not $SkipTimestamp
        RequirePublicChain = $ReleaseMode -eq 'Commercial'
    }
}

function Assert-ChurchDAWDesktopPolicyArtifactSet {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('Commercial', 'Test')]
        [string] $ReleaseMode,
        [Parameter(Mandatory = $true)]
        [long] $LatestVersionCode,
        [Parameter(Mandatory = $true)]
        [long] $MinimumVersionCode,
        [Parameter(Mandatory = $true)]
        [long] $WindowsInstallerBytes,
        [Parameter(Mandatory = $true)]
        [long] $MacInstallerBytes,
        [switch] $AllowCurrentVersionPolicyWithoutInstaller
    )

    $hasInstaller = $WindowsInstallerBytes -gt 0 -or
                    $MacInstallerBytes -gt 0

    if ($AllowCurrentVersionPolicyWithoutInstaller) {
        if ($ReleaseMode -ne 'Commercial') {
            throw ('-AllowCurrentVersionPolicyWithoutInstaller existe ' +
                   'somente para -ReleaseMode Commercial')
        }
        if ($hasInstaller) {
            throw ('-AllowCurrentVersionPolicyWithoutInstaller nao pode ser ' +
                   'usado quando ha instalador')
        }
        if ($LatestVersionCode -ne $MinimumVersionCode) {
            throw ('Politica comercial sem instalador exige ' +
                   'LatestVersionCode igual a MinimumVersionCode')
        }

        return 'current-version-policy-only'
    }

    if (-not $hasInstaller) {
        if ($ReleaseMode -eq 'Commercial') {
            throw 'Politica comercial exige ao menos um instalador'
        }

        return 'test-policy-only'
    }

    return 'installer-policy'
}

function Find-ChurchDAWSignTool {
    if (-not (Test-ChurchDAWIsWindows)) {
        throw 'signtool.exe so pode ser usado no Windows'
    }

    $fromPath = Get-Command signtool.exe -CommandType Application `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fromPath) {
        return $fromPath.Source
    }

    $roots = @()
    if (${env:ProgramFiles(x86)}) {
        $roots += Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    }
    if ($env:ProgramFiles) {
        $roots += Join-Path $env:ProgramFiles 'Windows Kits\10\bin'
    }

    $candidates = @()
    foreach ($root in $roots) {
        if (Test-Path -LiteralPath $root -PathType Container) {
            $candidates += Get-ChildItem -LiteralPath $root -Recurse `
                -Filter signtool.exe -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' }
        }
    }

    $selected = $candidates |
        Sort-Object @{ Expression = {
            try { [Version] $_.VersionInfo.FileVersion }
            catch { [Version] '0.0.0.0' }
        }; Descending = $true }, FullName -Descending |
        Select-Object -First 1

    if (-not $selected) {
        throw 'signtool.exe x64 nao encontrado. Instale o Windows SDK.'
    }

    return $selected.FullName
}

function Invoke-ChurchDAWNativeCapture {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,

        [string[]] $Arguments = @(),
        [string] $LogPath = '',
        [string] $Label = ''
    )

    $resolvedFilePath = $FilePath
    if ([IO.Path]::IsPathRooted($FilePath)) {
        if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
            throw "Executavel nativo nao encontrado: $FilePath"
        }
        $resolvedFilePath = [IO.Path]::GetFullPath($FilePath)
    }
    else {
        $command = Get-Command $FilePath -CommandType Application `
            -ErrorAction Stop | Select-Object -First 1
        if (-not $command) {
            throw "Executavel nativo nao encontrado: $FilePath"
        }
        $resolvedFilePath = $command.Source
    }

    $savedPreference = $ErrorActionPreference
    $exitCode = 9009
    $lines = @()
    $launched = $false
    $nativeExitSentinel = [int]::MinValue
    try {
        $ErrorActionPreference = 'Continue'
        # Nao crie uma variavel local $LASTEXITCODE: no Windows PowerShell 5.1
        # o processo nativo atualiza o escopo global, e uma local ficaria stale.
        $global:LASTEXITCODE = $nativeExitSentinel
        $lines = @(& $resolvedFilePath @Arguments 2>&1 |
            ForEach-Object { "$_" })
        if ($global:LASTEXITCODE -ne $nativeExitSentinel) {
            $exitCode = [int] $global:LASTEXITCODE
            $launched = $true
        }
    }
    catch {
        $lines += "Falha ao iniciar processo: $($_.Exception.Message)"
        $exitCode = 9009
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    $global:LASTEXITCODE = $exitCode

    foreach ($line in $lines) {
        Write-Host $line
    }

    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        $fullLogPath = [IO.Path]::GetFullPath($LogPath)
        $parent = Split-Path -Parent $fullLogPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }

        $header = if ([string]::IsNullOrWhiteSpace($Label)) {
            "[$([DateTimeOffset]::UtcNow.ToString('o'))]"
        }
        else {
            "[$([DateTimeOffset]::UtcNow.ToString('o'))] $Label"
        }
        $text = $header + [Environment]::NewLine
        if ($lines.Count -gt 0) {
            $text += ($lines -join [Environment]::NewLine) +
                [Environment]::NewLine
        }
        $text += "exit_code=$exitCode" + [Environment]::NewLine
        [IO.File]::AppendAllText(
            $fullLogPath,
            $text,
            [Text.UTF8Encoding]::new($false))
    }

    return [pscustomobject] @{
        ExitCode = [int] $exitCode
        Launched = $launched
        Output = $lines
    }
}

function ConvertTo-ChurchDAWUnixTimeSeconds {
    param(
        [Parameter(Mandatory = $true)]
        [DateTimeOffset] $Value
    )

    # DateTimeOffset.ToUnixTimeSeconds nao existe no .NET 4.5.2, que ainda
    # aparece em hosts Windows PowerShell 5.1 antigos.
    $epochTicks = [DateTime]::SpecifyKind(
        [DateTime] '1970-01-01T00:00:00',
        [DateTimeKind]::Utc).Ticks
    $deltaTicks = $Value.UtcDateTime.Ticks - $epochTicks
    return [long] [Math]::Floor(
        [double] $deltaTicks / [TimeSpan]::TicksPerSecond)
}

function Get-ChurchDAWSignToolVerifyArguments {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [bool] $RequireTimestamp = $true
    )

    $arguments = @('verify', '/pa', '/all')
    if ($RequireTimestamp) {
        $arguments += '/tw'
    }
    $arguments += @('/v', [IO.Path]::GetFullPath($Path))
    return $arguments
}

function Test-ChurchDAWCertificateHasCodeSigningEku {
    param(
        [Parameter(Mandatory = $true)]
        [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate
    )

    foreach ($extension in $Certificate.Extensions) {
        if ($extension.Oid.Value -eq '2.5.29.37') {
            $eku = [Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] $extension
            foreach ($oid in $eku.EnhancedKeyUsages) {
                if ($oid.Value -eq $script:ChurchDAWCodeSigningEku) {
                    return $true
                }
            }
        }
    }

    return $false
}

function Assert-ChurchDAWCertificateIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [Security.Cryptography.X509Certificates.X509Certificate2] $Certificate,

        [string] $ExpectedPublisher = '',
        [string[]] $ExpectedSignerThumbprints = @(),
        [bool] $RequirePrivateKey = $false,
        [bool] $RequirePublicChain = $false
    )

    if ($RequirePrivateKey -and -not $Certificate.HasPrivateKey) {
        throw 'O certificado selecionado nao possui chave privada'
    }
    if (-not (Test-ChurchDAWCertificateHasCodeSigningEku $Certificate)) {
        throw 'O certificado nao possui o EKU Code Signing'
    }

    $actualThumbprint = ConvertTo-ChurchDAWThumbprint $Certificate.Thumbprint
    $allowed = @()
    foreach ($candidate in @($ExpectedSignerThumbprints)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $allowed += ConvertTo-ChurchDAWThumbprint $candidate
        }
    }

    if ($allowed.Count -gt 0 -and $actualThumbprint -notin $allowed) {
        throw "Signer inesperado: thumbprint $actualThumbprint"
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPublisher) -and
        -not $Certificate.Subject.Trim().Equals(
            $ExpectedPublisher.Trim(),
            [StringComparison]::OrdinalIgnoreCase)) {
        throw ("Publisher inesperado: '$($Certificate.Subject)' " +
               "(esperado '$ExpectedPublisher')")
    }

    $now = [DateTimeOffset]::Now
    if ($now -lt [DateTimeOffset] $Certificate.NotBefore -or
        $now -gt [DateTimeOffset] $Certificate.NotAfter) {
        throw ('O certificado esta fora da validade: ' +
               "$($Certificate.NotBefore.ToString('o')) - " +
               $Certificate.NotAfter.ToString('o'))
    }

    $chainSubjects = @()
    if ($RequirePublicChain) {
        $chain = [Security.Cryptography.X509Certificates.X509Chain]::new()
        try {
            $chain.ChainPolicy.RevocationMode =
                [Security.Cryptography.X509Certificates.X509RevocationMode]::Online
            $chain.ChainPolicy.RevocationFlag =
                [Security.Cryptography.X509Certificates.X509RevocationFlag]::EntireChain
            $chain.ChainPolicy.VerificationFlags =
                [Security.Cryptography.X509Certificates.X509VerificationFlags]::NoFlag
            $chain.ChainPolicy.UrlRetrievalTimeout = [TimeSpan]::FromSeconds(30)

            $chainOk = $chain.Build($Certificate)
            $chainSubjects = @($chain.ChainElements |
                ForEach-Object { $_.Certificate.Subject })
            if (-not $chainOk) {
                $statuses = @($chain.ChainStatus | ForEach-Object {
                    "$($_.Status): $($_.StatusInformation.Trim())"
                }) -join '; '
                throw "A cadeia publica/revogacao do certificado falhou: $statuses"
            }
            if ($chain.ChainElements.Count -lt 2) {
                throw 'Certificado self-signed/local nao e aceito em release comercial'
            }

            $root = $chain.ChainElements[
                $chain.ChainElements.Count - 1].Certificate
            if ((ConvertTo-ChurchDAWThumbprint $root.Thumbprint) -eq
                $actualThumbprint) {
                throw 'Certificado self-signed nao e aceito em release comercial'
            }
        }
        finally {
            $chain.Dispose()
        }
    }

    return [pscustomobject] @{
        Publisher = $Certificate.Subject.Trim()
        SignerThumbprint = $actualThumbprint
        NotBefore = $Certificate.NotBefore.ToUniversalTime().ToString('o')
        NotAfter = $Certificate.NotAfter.ToUniversalTime().ToString('o')
        ChainSubjects = $chainSubjects
    }
}

function Assert-ChurchDAWAuthenticodeSignature {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [string] $SignTool = '',
        [string] $ExpectedPublisher = '',
        [string[]] $ExpectedSignerThumbprints = @(),
        [bool] $RequireTimestamp = $true,
        [bool] $RequirePublicChain = $true,
        [string] $VerificationLogPath = ''
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Binario para verificacao nao encontrado: $fullPath"
    }
    if ([string]::IsNullOrWhiteSpace($SignTool)) {
        $SignTool = Find-ChurchDAWSignTool
    }

    $verify = Invoke-ChurchDAWNativeCapture -FilePath $SignTool `
        -Arguments @(Get-ChurchDAWSignToolVerifyArguments `
            -Path $fullPath -RequireTimestamp $RequireTimestamp) `
        -LogPath $VerificationLogPath `
        -Label "signtool verify $([IO.Path]::GetFileName($fullPath))"
    if ($verify.ExitCode -ne 0) {
        $verifyFlags = if ($RequireTimestamp) {
            '/pa /all /tw /v'
        } else { '/pa /all /v' }
        throw ("signtool verify $verifyFlags recusou o arquivo " +
               "(codigo $($verify.ExitCode)): $fullPath")
    }

    $signature = Get-AuthenticodeSignature -LiteralPath $fullPath
    if ($signature.Status -ne 'Valid') {
        throw "Authenticode invalido ($($signature.Status)): $fullPath"
    }
    if ([string] $signature.SignatureType -ne 'Authenticode') {
        throw ("O arquivo nao possui assinatura Authenticode embutida " +
               "(tipo $($signature.SignatureType)): $fullPath")
    }
    if ($null -eq $signature.SignerCertificate) {
        throw "Certificado do signer ausente: $fullPath"
    }
    if ($RequireTimestamp -and
        $null -eq $signature.TimeStamperCertificate) {
        throw "Timestamp RFC 3161 ausente: $fullPath"
    }

    $identity = Assert-ChurchDAWCertificateIdentity `
        -Certificate $signature.SignerCertificate `
        -ExpectedPublisher $ExpectedPublisher `
        -ExpectedSignerThumbprints $ExpectedSignerThumbprints `
        -RequirePublicChain $RequirePublicChain

    return [pscustomobject] @{
        Path = $fullPath
        Status = [string] $signature.Status
        SignatureType = [string] $signature.SignatureType
        Publisher = $identity.Publisher
        SignerThumbprint = $identity.SignerThumbprint
        SignerNotBefore = $identity.NotBefore
        SignerNotAfter = $identity.NotAfter
        SignerChainSubjects = $identity.ChainSubjects
        TimeStamped = $null -ne $signature.TimeStamperCertificate
        TimeStamper = if ($signature.TimeStamperCertificate) {
            $signature.TimeStamperCertificate.Subject
        } else { '' }
        TimeStamperThumbprint = if ($signature.TimeStamperCertificate) {
            ConvertTo-ChurchDAWThumbprint `
                $signature.TimeStamperCertificate.Thumbprint
        } else { '' }
        Sha256 = (Get-FileHash -LiteralPath $fullPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        Bytes = [long] (Get-Item -LiteralPath $fullPath).Length
        SigntoolVerifyCode = $verify.ExitCode
    }
}

function Assert-ChurchDAWSameSigner {
    param(
        [Parameter(Mandatory = $true)]
        $First,

        [Parameter(Mandatory = $true)]
        $Second,

        [string] $FirstLabel = 'primeiro binario',
        [string] $SecondLabel = 'segundo binario'
    )

    $firstThumbprint = ConvertTo-ChurchDAWThumbprint `
        ([string] $First.SignerThumbprint)
    $secondThumbprint = ConvertTo-ChurchDAWThumbprint `
        ([string] $Second.SignerThumbprint)

    if ($firstThumbprint -ne $secondThumbprint) {
        throw ("Signers diferentes: $FirstLabel=$firstThumbprint; " +
               "$SecondLabel=$secondThumbprint")
    }
    if (-not ([string] $First.Publisher).Equals(
        [string] $Second.Publisher,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw ("Publishers diferentes: $FirstLabel='$($First.Publisher)'; " +
               "$SecondLabel='$($Second.Publisher)'")
    }
}

function Get-ChurchDAWPeInfo {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "PE nao encontrado: $fullPath"
    }

    $stream = [IO.File]::Open(
        $fullPath,
        [IO.FileMode]::Open,
        [IO.FileAccess]::Read,
        [IO.FileShare]::Read)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw "Cabecalho DOS invalido: $fullPath"
        }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0x40 -or $peOffset + 26 -gt $stream.Length) {
            throw "Offset PE invalido: $fullPath"
        }

        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw "Assinatura PE invalida: $fullPath"
        }
        $machine = $reader.ReadUInt16()
        $stream.Position = $peOffset + 24
        $optionalMagic = $reader.ReadUInt16()
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }

    return [pscustomobject] @{
        Path = $fullPath
        Machine = ('0x{0:X4}' -f $machine)
        OptionalHeaderMagic = ('0x{0:X4}' -f $optionalMagic)
        IsX64 = $machine -eq 0x8664 -and $optionalMagic -eq 0x020B
    }
}

function ConvertTo-ChurchDAWThreePartVersion {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Version,
        [string] $Label = 'versao'
    )

    $match = [regex]::Match($Version, '^\s*(\d+)\.(\d+)\.(\d+)(?:\.\d+)?')
    if (-not $match.Success) {
        throw "${Label} invalida: '$Version'"
    }
    return ('{0}.{1}.{2}' -f
        [int] $match.Groups[1].Value,
        [int] $match.Groups[2].Value,
        [int] $match.Groups[3].Value)
}

function Assert-ChurchDAWPeReleaseIdentity {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedVersion,

        [string] $ExpectedProductName = 'ChurchDAW'
    )

    $pe = Get-ChurchDAWPeInfo $Path
    if (-not $pe.IsX64) {
        throw ("Binario nao e PE32+ x64: machine=$($pe.Machine), " +
               "magic=$($pe.OptionalHeaderMagic), path=$($pe.Path)")
    }

    $expected = ConvertTo-ChurchDAWThreePartVersion `
        $ExpectedVersion 'versao esperada'
    $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($pe.Path)
    $fileVersion = ConvertTo-ChurchDAWThreePartVersion `
        ([string] $versionInfo.FileVersion) 'FileVersion'
    $productVersion = ConvertTo-ChurchDAWThreePartVersion `
        ([string] $versionInfo.ProductVersion) 'ProductVersion'

    if ($fileVersion -ne $expected -or $productVersion -ne $expected) {
        throw ("Versao PE divergente: FileVersion=$fileVersion; " +
               "ProductVersion=$productVersion; esperado=$expected; " +
               "path=$($pe.Path)")
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedProductName) -and
        -not ([string] $versionInfo.ProductName).Trim().Equals(
            $ExpectedProductName,
            [StringComparison]::Ordinal)) {
        throw ("ProductName inesperado: '$($versionInfo.ProductName)' " +
               "(esperado '$ExpectedProductName')")
    }

    return [pscustomobject] @{
        Path = $pe.Path
        Machine = $pe.Machine
        OptionalHeaderMagic = $pe.OptionalHeaderMagic
        FileVersion = $fileVersion
        ProductVersion = $productVersion
        ProductName = [string] $versionInfo.ProductName
        OriginalFilename = [string] $versionInfo.OriginalFilename
    }
}

function Initialize-ChurchDAWResourceReader {
    if ('ChurchDAW.ReleasePipeline.Win32Resources' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace ChurchDAW.ReleasePipeline
{
    public static class Win32Resources
    {
        private const uint LOAD_LIBRARY_AS_DATAFILE = 0x00000002;
        private const uint LOAD_LIBRARY_AS_IMAGE_RESOURCE = 0x00000020;
        private static readonly IntPtr RT_RCDATA = new IntPtr(10);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode,
            SetLastError = true)]
        private static extern IntPtr LoadLibraryEx(
            string fileName, IntPtr file, uint flags);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr FindResource(
            IntPtr module, IntPtr name, IntPtr type);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint SizeofResource(
            IntPtr module, IntPtr resource);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LoadResource(
            IntPtr module, IntPtr resource);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LockResource(IntPtr resourceData);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool FreeLibrary(IntPtr module);

        public static byte[] ReadRcData(string path, int resourceId)
        {
            IntPtr module = LoadLibraryEx(
                path,
                IntPtr.Zero,
                LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (module == IntPtr.Zero)
                throw new Win32Exception(
                    Marshal.GetLastWin32Error(), "LoadLibraryEx failed");

            try
            {
                IntPtr found = FindResource(
                    module, new IntPtr(resourceId), RT_RCDATA);
                if (found == IntPtr.Zero)
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(), "FindResource failed");

                uint size = SizeofResource(module, found);
                if (size == 0 || size > Int32.MaxValue)
                    throw new InvalidOperationException(
                        "Invalid RCDATA size: " + size);

                IntPtr loaded = LoadResource(module, found);
                if (loaded == IntPtr.Zero)
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(), "LoadResource failed");

                IntPtr pointer = LockResource(loaded);
                if (pointer == IntPtr.Zero)
                    throw new Win32Exception(
                        Marshal.GetLastWin32Error(), "LockResource failed");

                byte[] bytes = new byte[size];
                Marshal.Copy(pointer, bytes, 0, checked((int)size));
                return bytes;
            }
            finally
            {
                FreeLibrary(module);
            }
        }
    }
}
'@
}

function Export-ChurchDAWInstallerPayload {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Installer,

        [Parameter(Mandatory = $true)]
        [string] $Output,

        [int] $ResourceId = 1
    )

    if (-not (Test-ChurchDAWIsWindows)) {
        throw 'A extracao do payload PE precisa rodar no Windows'
    }

    $installerFull = [IO.Path]::GetFullPath($Installer)
    if (-not (Test-Path -LiteralPath $installerFull -PathType Leaf)) {
        throw "Instalador nao encontrado: $installerFull"
    }

    Initialize-ChurchDAWResourceReader
    $bytes = [ChurchDAW.ReleasePipeline.Win32Resources]::ReadRcData(
        $installerFull, $ResourceId)

    $outputFull = [IO.Path]::GetFullPath($Output)
    $parent = Split-Path -Parent $outputFull
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [IO.File]::WriteAllBytes($outputFull, $bytes)

    return [pscustomobject] @{
        Installer = $installerFull
        Payload = $outputFull
        ResourceId = $ResourceId
        Bytes = [long] $bytes.Length
        Sha256 = (Get-FileHash -LiteralPath $outputFull `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-ChurchDAWFileRecord {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [string] $Role = ''
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Arquivo do release nao encontrado: $fullPath"
    }

    return [ordered] @{
        role = $Role
        file = [IO.Path]::GetFileName($fullPath)
        bytes = [long] (Get-Item -LiteralPath $fullPath).Length
        sha256 = (Get-FileHash -LiteralPath $fullPath `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-ChurchDAWGitState {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Repository
    )

    $repo = [IO.Path]::GetFullPath($Repository)
    $inside = @(& git -C $repo rev-parse --is-inside-work-tree 2>$null)
    if ($LASTEXITCODE -ne 0 -or ($inside -join '').Trim() -ne 'true') {
        throw "Diretorio nao e um worktree Git: $repo"
    }

    $commit = (@(& git -C $repo rev-parse HEAD)).Trim()
    if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-fA-F]{40}$') {
        throw 'Nao foi possivel resolver o commit Git'
    }
    $branch = (@(& git -C $repo branch --show-current)).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Nao foi possivel resolver a branch Git'
    }
    $status = @(& git -C $repo status --porcelain=v1 `
        --untracked-files=normal)
    if ($LASTEXITCODE -ne 0) {
        throw 'git status falhou'
    }

    return [pscustomobject] @{
        Commit = $commit.ToLowerInvariant()
        Branch = $branch
        IsClean = $status.Count -eq 0
        Status = @($status)
    }
}
