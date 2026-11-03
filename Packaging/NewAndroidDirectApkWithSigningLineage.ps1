param(
    [Parameter(Mandatory = $true)]
    [string] $InputApk,

    [Parameter(Mandatory = $true)]
    [string] $OutputApk,

    [Parameter(Mandatory = $true)]
    [string] $OutputLineage,

    [string] $ApkSigner,

    [Parameter(Mandatory = $true)]
    [string] $LegacyF25Keystore,

    [Parameter(Mandatory = $true)]
    [string] $LegacyF25Alias,

    [Parameter(Mandatory = $true)]
    [string] $LegacyF25StorePasswordEnvironmentVariable,

    [Parameter(Mandatory = $true)]
    [string] $LegacyF25KeyPasswordEnvironmentVariable,

    [Parameter(Mandatory = $true)]
    [string] $Legacy415aKeystore,

    [Parameter(Mandatory = $true)]
    [string] $Legacy415aAlias,

    [Parameter(Mandatory = $true)]
    [string] $Legacy415aStorePasswordEnvironmentVariable,

    [Parameter(Mandatory = $true)]
    [string] $Legacy415aKeyPasswordEnvironmentVariable,

    [Parameter(Mandatory = $true)]
    [string] $CurrentKeystore,

    [Parameter(Mandatory = $true)]
    [string] $CurrentAlias,

    [Parameter(Mandatory = $true)]
    [string] $CurrentStorePasswordEnvironmentVariable,

    [Parameter(Mandatory = $true)]
    [string] $CurrentKeyPasswordEnvironmentVariable,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedLegacyF25CertificateSha256,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedLegacy415aCertificateSha256,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedCurrentCertificateSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Get-NormalizedCertificateSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Value,

        [Parameter(Mandatory = $true)]
        [string] $Label
    )

    $normalized = ($Value -replace '[^0-9a-fA-F]', '').ToLowerInvariant()
    if ($normalized -notmatch '^[0-9a-f]{64}$') {
        throw "$Label precisa ser um SHA-256 de certificado valido"
    }
    return $normalized
}

function Assert-SafeEnvironmentSecret {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name
    )

    if ($Name -notmatch '^[A-Za-z_][A-Za-z0-9_]{0,127}$') {
        throw "Nome de variavel de segredo invalido: $Name"
    }
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrEmpty($value) -or
        $value.IndexOfAny([char[]] "`0`r`n") -ge 0) {
        throw "Segredo Android ausente ou invalido: $Name"
    }
}

function Resolve-ExistingFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [string] $Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label nao encontrado: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-Keytool {
    if (-not [string]::IsNullOrWhiteSpace($env:JAVA_HOME)) {
        foreach ($name in @('keytool.exe', 'keytool')) {
            $candidate = Join-Path $env:JAVA_HOME "bin/$name"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    }

    $command = Get-Command keytool -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($null -eq $command) {
        throw 'keytool nao encontrado no JAVA_HOME nem no PATH'
    }
    return $command.Source
}

function Resolve-ApkSigner {
    param([string] $RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        return Resolve-ExistingFile $RequestedPath 'apksigner'
    }

    $sdkRoot = if (-not [string]::IsNullOrWhiteSpace($env:ANDROID_HOME)) {
        $env:ANDROID_HOME
    } else {
        $env:ANDROID_SDK_ROOT
    }
    if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
        $buildToolsRoot = Join-Path $sdkRoot 'build-tools'
        if (Test-Path -LiteralPath $buildToolsRoot -PathType Container) {
            $candidate = Get-ChildItem -LiteralPath $buildToolsRoot -Directory |
                Sort-Object {
                    try { [version] $_.Name } catch { [version] '0.0' }
                } -Descending |
                ForEach-Object {
                    foreach ($name in @('apksigner.bat', 'apksigner')) {
                        $path = Join-Path $_.FullName $name
                        if (Test-Path -LiteralPath $path -PathType Leaf) {
                            $path
                        }
                    }
                } | Select-Object -First 1
            if (-not [string]::IsNullOrWhiteSpace($candidate)) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
    }

    foreach ($name in @('apksigner', 'apksigner.bat')) {
        $command = Get-Command $name -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $command) {
            return $command.Source
        }
    }
    throw 'apksigner nao encontrado no Android SDK nem no PATH'
}

function Assert-KeystoreCertificate {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Keytool,

        [Parameter(Mandatory = $true)]
        [string] $Keystore,

        [Parameter(Mandatory = $true)]
        [string] $Alias,

        [Parameter(Mandatory = $true)]
        [string] $StorePasswordEnvironmentVariable,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedSha256,

        [Parameter(Mandatory = $true)]
        [string] $CertificateOutput
    )

    $previousPreference = $ErrorActionPreference
    try {
        # Windows PowerShell 5 transforma texto nativo em stderr em um
        # NativeCommandError. keytool usa stderr ate para a mensagem de sucesso.
        $ErrorActionPreference = 'Continue'
        & $Keytool -exportcert -keystore $Keystore `
            -storepass:env $StorePasswordEnvironmentVariable `
            -alias $Alias -file $CertificateOutput 2>&1 | Out-Null
        $keytoolExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($keytoolExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $CertificateOutput -PathType Leaf)) {
        throw "Nao foi possivel exportar o certificado de $Keystore"
    }

    $actual = (Get-FileHash -LiteralPath $CertificateOutput -Algorithm SHA256).
        Hash.ToLowerInvariant()
    if ($actual -cne $ExpectedSha256) {
        throw "Certificado Android inesperado em $Keystore`: $actual"
    }
}

function Invoke-ApkSigner {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,

        [Parameter(Mandatory = $true)]
        [string[]] $Arguments,

        [Parameter(Mandatory = $true)]
        [string] $FailureMessage
    )

    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        throw "$FailureMessage (codigo $exitCode)"
    }
}

function Assert-ApkForSdkRange {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Executable,

        [Parameter(Mandatory = $true)]
        [string] $Apk,

        [Parameter(Mandatory = $true)]
        [int] $MinimumSdk,

        [Nullable[int]] $MaximumSdk,

        [Parameter(Mandatory = $true)]
        [string] $RequiredSchemePattern,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedCertificateSha256
    )

    $arguments = @(
        'verify', '--verbose', '--print-certs',
        '--min-sdk-version', [string] $MinimumSdk
    )
    if ($null -ne $MaximumSdk) {
        $arguments += @('--max-sdk-version', [string] $MaximumSdk)
    }
    $arguments += $Apk

    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $verification = @(& $Executable @arguments 2>&1 | ForEach-Object {
            [string] $_
        })
        $verificationExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($verificationExitCode -ne 0 -or
        -not ($verification -match $RequiredSchemePattern)) {
        $verification | ForEach-Object { Write-Host $_ }
        throw "APK nao passou na verificacao para API $MinimumSdk"
    }

    $certificateMatches = @($verification | ForEach-Object {
        [regex]::Match(
            $_,
            '(?i)^Signer #1 certificate SHA-256 digest:\s*([0-9a-f]{64})$'
        )
    } | Where-Object { $_.Success })
    if ($certificateMatches.Count -ne 1) {
        throw "Certificado do APK nao foi identificado para API $MinimumSdk"
    }
    $actualCertificate = $certificateMatches[0].Groups[1].Value.
        ToLowerInvariant()
    if ($actualCertificate -cne $ExpectedCertificateSha256) {
        throw "Certificado do APK inesperado para API $MinimumSdk`: $actualCertificate"
    }

    $verification | ForEach-Object { Write-Host $_ }
}

$inputPath = Resolve-ExistingFile $InputApk 'APK de entrada'
$f25Path = Resolve-ExistingFile $LegacyF25Keystore 'Keystore legado F25'
$legacy415aPath = Resolve-ExistingFile $Legacy415aKeystore `
    'Keystore legado 415A'
$currentPath = Resolve-ExistingFile $CurrentKeystore 'Keystore atual'
$apkSignerPath = Resolve-ApkSigner $ApkSigner
$keytoolPath = Resolve-Keytool

$outputApkPath = [IO.Path]::GetFullPath($OutputApk)
$outputLineagePath = [IO.Path]::GetFullPath($OutputLineage)
if ($inputPath -ceq $outputApkPath -or
    $outputApkPath -ceq $outputLineagePath) {
    throw 'Os caminhos de entrada, APK final e lineage precisam ser distintos'
}
foreach ($output in @($outputApkPath, $outputLineagePath)) {
    if (Test-Path -LiteralPath $output) {
        throw "Saida ja existe; remova-a explicitamente antes da build: $output"
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $output) `
        -Force | Out-Null
}

foreach ($secretName in @(
    $LegacyF25StorePasswordEnvironmentVariable,
    $LegacyF25KeyPasswordEnvironmentVariable,
    $Legacy415aStorePasswordEnvironmentVariable,
    $Legacy415aKeyPasswordEnvironmentVariable,
    $CurrentStorePasswordEnvironmentVariable,
    $CurrentKeyPasswordEnvironmentVariable
)) {
    Assert-SafeEnvironmentSecret $secretName
}

foreach ($entry in @(
    @($LegacyF25Alias, 'Alias F25'),
    @($Legacy415aAlias, 'Alias 415A'),
    @($CurrentAlias, 'Alias atual')
)) {
    if ([string]::IsNullOrWhiteSpace([string] $entry[0]) -or
        ([string] $entry[0]).IndexOfAny([char[]] "`0`r`n") -ge 0) {
        throw "$($entry[1]) ausente ou invalido"
    }
}

$f25Certificate = Get-NormalizedCertificateSha256 `
    $ExpectedLegacyF25CertificateSha256 'Certificado legado F25'
$legacy415aCertificate = Get-NormalizedCertificateSha256 `
    $ExpectedLegacy415aCertificateSha256 'Certificado legado 415A'
$currentCertificate = Get-NormalizedCertificateSha256 `
    $ExpectedCurrentCertificateSha256 'Certificado atual'
$certificateSet = @(
    $f25Certificate,
    $legacy415aCertificate,
    $currentCertificate
)
if (@($certificateSet | Sort-Object -Unique).Count -ne 3) {
    throw 'Os tres certificados Android da lineage precisam ser distintos'
}

$temporaryBase = if (-not [string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    $env:RUNNER_TEMP
} else {
    [IO.Path]::GetTempPath()
}
$temporaryRoot = Join-Path $temporaryBase (
    'churchdaw-android-lineage-' + [Guid]::NewGuid().ToString('N'))
$createdOutputs = [Collections.Generic.List[string]]::new()

try {
    New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
    $f25Der = Join-Path $temporaryRoot 'legacy-f25.der'
    $legacy415aDer = Join-Path $temporaryRoot 'legacy-415a.der'
    $currentDer = Join-Path $temporaryRoot 'current.der'
    Assert-KeystoreCertificate $keytoolPath $f25Path $LegacyF25Alias `
        $LegacyF25StorePasswordEnvironmentVariable $f25Certificate $f25Der
    Assert-KeystoreCertificate $keytoolPath $legacy415aPath $Legacy415aAlias `
        $Legacy415aStorePasswordEnvironmentVariable $legacy415aCertificate `
        $legacy415aDer
    Assert-KeystoreCertificate $keytoolPath $currentPath $CurrentAlias `
        $CurrentStorePasswordEnvironmentVariable $currentCertificate $currentDer

    $lineageStep = Join-Path $temporaryRoot 'f25-to-415a.bin'
    $lineageFinal = Join-Path $temporaryRoot 'f25-to-415a-to-current.bin'
    $signedApk = Join-Path $temporaryRoot 'ChurchDAW-unified-signed.apk'

    Invoke-ApkSigner $apkSignerPath @(
        'rotate', '--out', $lineageStep,
        '--old-signer',
        '--ks', $f25Path,
        '--ks-key-alias', $LegacyF25Alias,
        '--ks-pass', "env:$LegacyF25StorePasswordEnvironmentVariable",
        '--key-pass', "env:$LegacyF25KeyPasswordEnvironmentVariable",
        '--set-installed-data', 'true',
        '--set-shared-uid', 'true',
        '--set-permission', 'true',
        '--set-rollback', 'false',
        '--set-auth', 'true',
        '--new-signer',
        '--ks', $legacy415aPath,
        '--ks-key-alias', $Legacy415aAlias,
        '--ks-pass', "env:$Legacy415aStorePasswordEnvironmentVariable",
        '--key-pass', "env:$Legacy415aKeyPasswordEnvironmentVariable"
    ) 'Falha ao criar a etapa F25 -> 415A da lineage'

    Invoke-ApkSigner $apkSignerPath @(
        'rotate', '--in', $lineageStep, '--out', $lineageFinal,
        '--old-signer',
        '--ks', $legacy415aPath,
        '--ks-key-alias', $Legacy415aAlias,
        '--ks-pass', "env:$Legacy415aStorePasswordEnvironmentVariable",
        '--key-pass', "env:$Legacy415aKeyPasswordEnvironmentVariable",
        '--set-installed-data', 'true',
        '--set-shared-uid', 'true',
        '--set-permission', 'true',
        '--set-rollback', 'false',
        '--set-auth', 'true',
        '--new-signer',
        '--ks', $currentPath,
        '--ks-key-alias', $CurrentAlias,
        '--ks-pass', "env:$CurrentStorePasswordEnvironmentVariable",
        '--key-pass', "env:$CurrentKeyPasswordEnvironmentVariable"
    ) 'Falha ao criar a etapa 415A -> atual da lineage'

    Invoke-ApkSigner $apkSignerPath @(
        'sign', '--in', $inputPath, '--out', $signedApk,
        '--min-sdk-version', '24',
        '--rotation-min-sdk-version', '28',
        '--lineage', $lineageFinal,
        '--ks', $f25Path,
        '--ks-key-alias', $LegacyF25Alias,
        '--ks-pass', "env:$LegacyF25StorePasswordEnvironmentVariable",
        '--key-pass', "env:$LegacyF25KeyPasswordEnvironmentVariable",
        '--next-signer',
        '--ks', $legacy415aPath,
        '--ks-key-alias', $Legacy415aAlias,
        '--ks-pass', "env:$Legacy415aStorePasswordEnvironmentVariable",
        '--key-pass', "env:$Legacy415aKeyPasswordEnvironmentVariable",
        '--next-signer',
        '--ks', $currentPath,
        '--ks-key-alias', $CurrentAlias,
        '--ks-pass', "env:$CurrentStorePasswordEnvironmentVariable",
        '--key-pass', "env:$CurrentKeyPasswordEnvironmentVariable"
    ) 'Falha ao assinar o APK com a lineage unificada'

    Assert-ApkForSdkRange $apkSignerPath $signedApk 24 27 `
        'Verified using v2 scheme \(APK Signature Scheme v2\): true' `
        $f25Certificate
    Assert-ApkForSdkRange $apkSignerPath $signedApk 28 $null `
        'Verified using v3 scheme \(APK Signature Scheme v3\): true' `
        $currentCertificate

    [IO.File]::Copy($lineageFinal, $outputLineagePath, $false)
    $createdOutputs.Add($outputLineagePath)
    [IO.File]::Copy($signedApk, $outputApkPath, $false)
    $createdOutputs.Add($outputApkPath)

    $apkHash = (Get-FileHash -LiteralPath $outputApkPath -Algorithm SHA256).
        Hash.ToLowerInvariant()
    $lineageHash = (Get-FileHash -LiteralPath $outputLineagePath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host "APK com lineage: $outputApkPath"
    Write-Host "APK SHA-256: $apkHash"
    Write-Host "Lineage: $outputLineagePath"
    Write-Host "Lineage SHA-256: $lineageHash"
}
catch {
    foreach ($createdOutput in $createdOutputs) {
        Remove-Item -LiteralPath $createdOutput -Force `
            -ErrorAction SilentlyContinue
    }
    throw
}
finally {
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
