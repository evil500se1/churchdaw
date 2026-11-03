param(
    [string] $SecretsDirectory = "$env:USERPROFILE\ChurchDAW-Secrets",
    [string] $UploadCredentialFile = '',
    [switch] $SkipClean,
    [switch] $BuildApk
)

$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$projectFile = Join-Path $projectRoot 'ChurchDAW.jucer'
& (Join-Path $PSScriptRoot 'ApplyAndroidOpenGLCompatibility.ps1') -ProjectRoot $projectRoot
$projucer = Join-Path $projectRoot 'JUCE\Projucer.exe'
$configurationVerifier = Join-Path $PSScriptRoot 'AssertAndroidBuildConfiguration.ps1'
$credentialFile = if ([string]::IsNullOrWhiteSpace($UploadCredentialFile)) {
    Join-Path $SecretsDirectory 'churchdaw-upload-2026.credential.xml'
} else {
    [IO.Path]::GetFullPath($UploadCredentialFile)
}
$keystoreFile = Join-Path $SecretsDirectory 'churchdaw-upload-2026.jks'
$legacyCredentialFile = Join-Path $SecretsDirectory `
    'churchdaw-legacy-android-debug.credential.xml'
$legacyF25KeystoreFile = Join-Path $SecretsDirectory `
    'churchdaw-PRODUCAO-ATUAL-debugkey.jks'
$legacy415aKeystoreFile = Join-Path $SecretsDirectory `
    'churchdaw-legacy-415a.jks'
$signingLineageBuilder = Join-Path $PSScriptRoot `
    'NewAndroidDirectApkWithSigningLineage.ps1'
$expectedLegacyF25CertificateSha256 =
    'f25a31498464456a7303269a7ac0d269fa199d81a62cb95b296d59d87d6d5b04'
$expectedLegacy415aCertificateSha256 =
    '415a1dc8e1da7f457eb3cdc028ebf09c75bb82e0e8c9be26390a524fa42befea'
$expectedSigningCertificateSha256 =
    'aad0bea9e4b788d0e7d4224e3c06bd7b4f212f31bd6a5505a29f375214bc3e91'
$androidDirectory = Join-Path $projectRoot 'Builds\Android'
$androidResources = Join-Path $PSScriptRoot 'AndroidResources'
$generatedResourceDirectory = Join-Path $androidDirectory 'app\src\main\res'
$generatedMainDirectory = Join-Path $androidDirectory 'app\src\main'

function Sync-ChurchDawAndroidResources {
    if (-not (Test-Path -LiteralPath $androidResources)) {
        throw "Recursos Android oficiais ausentes: $androidResources"
    }

    # Arquivos AppleDouble (._*) podem nascer ao transportar o workspace de
    # macOS para Windows. O Android Lint tenta interpreta-los como XML e cai
    # dentro de PositionXmlParser. Eles nao sao recursos do aplicativo.
    if (Test-Path -LiteralPath $generatedMainDirectory) {
        Get-ChildItem -LiteralPath $generatedMainDirectory -Recurse -File |
            Where-Object {
                $_.Name.StartsWith('._', [StringComparison]::Ordinal) -or
                $_.Name -ceq '.DS_Store'
            } |
            Remove-Item -Force
    }

    $resourceFiles = @(Get-ChildItem -LiteralPath $androidResources `
        -Recurse -File | Where-Object {
            -not $_.Name.StartsWith('._', [StringComparison]::Ordinal) -and
            $_.Name -cne '.DS_Store'
        })
    if ($resourceFiles.Count -eq 0) {
        throw "A pasta de recursos Android esta vazia: $androidResources"
    }

    foreach ($resourceFile in $resourceFiles) {
        $relativePath = $resourceFile.FullName.Substring(
            $androidResources.Length
        ).TrimStart('\', '/')
        $targetFile = Join-Path $generatedResourceDirectory $relativePath
        $targetDirectory = Split-Path -Parent $targetFile

        New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
        Copy-Item -LiteralPath $resourceFile.FullName `
            -Destination $targetFile -Force
    }
}

if (-not (Test-Path -LiteralPath $projucer)) {
    throw "Projucer nao encontrado: $projucer"
}

if (-not (Test-Path -LiteralPath $configurationVerifier)) {
    throw "Verificador Android nao encontrado: $configurationVerifier"
}

# O .jucer e a fonte persistente do projeto Android. Regenerar antes da build
# impede publicar um Gradle/Manifest antigo depois de mudar versionCode, Activity
# ou dependencias do Play Core.
$projucerProcess = Start-Process -FilePath $projucer `
    -ArgumentList @('--resave', $projectFile) `
    -WindowStyle Hidden -Wait -PassThru
if ($projucerProcess.ExitCode -ne 0) {
    throw "Projucer terminou com codigo $($projucerProcess.ExitCode)"
}

# O Projucer recria Builds/Android. Sincronize toda a arvore (mipmap,
# drawable e values) depois dele para o icone adaptativo nunca depender de
# um ajuste manual no projeto gerado.
Sync-ChurchDawAndroidResources

& $configurationVerifier -ProjectRoot $projectRoot

if (-not (Test-Path -LiteralPath $credentialFile)) {
    throw "Credencial protegida nao encontrada: $credentialFile"
}

if (-not (Test-Path -LiteralPath $keystoreFile)) {
    throw "Chave de upload nao encontrada: $keystoreFile"
}

foreach ($requiredFile in @(
    @($legacyCredentialFile, 'Credencial protegida das chaves legadas'),
    @($legacyF25KeystoreFile, 'Chave Android legada F25'),
    @($legacy415aKeystoreFile, 'Chave Android legada 415A'),
    @($signingLineageBuilder, 'Gerador da lineage Android')
)) {
    if (-not (Test-Path -LiteralPath $requiredFile[0] -PathType Leaf)) {
        throw "$($requiredFile[1]) nao encontrada: $($requiredFile[0])"
    }
}

$credential = Import-Clixml -LiteralPath $credentialFile
$plainPassword = $credential.GetNetworkCredential().Password
$legacyCredential = Import-Clixml -LiteralPath $legacyCredentialFile
$legacyPlainPassword = $legacyCredential.GetNetworkCredential().Password
$previousJavaHome = $env:JAVA_HOME
$previousGradleOpts = $env:GRADLE_OPTS
$bundledJavaHome = 'C:\Program Files\Android\Android Studio\jbr'

try {
    if (-not $env:JAVA_HOME -and (Test-Path -LiteralPath $bundledJavaHome)) {
        $env:JAVA_HOME = $bundledJavaHome
    }
    # O pre-bundle inclui a biblioteca nativa e pode ultrapassar o heap
    # padrao do Gradle mesmo depois de a compilacao C++ ter terminado.
    $env:GRADLE_OPTS = '-Xmx4096m -Dfile.encoding=UTF-8'

    $env:CHURCHDAW_UPLOAD_STORE_FILE = $keystoreFile
    $env:CHURCHDAW_UPLOAD_STORE_PASSWORD = $plainPassword
    $env:CHURCHDAW_UPLOAD_KEY_ALIAS = $credential.UserName
    $env:CHURCHDAW_UPLOAD_KEY_PASSWORD = $plainPassword
    $env:CHURCHDAW_LEGACY_STORE_PASSWORD = $legacyPlainPassword
    $env:CHURCHDAW_LEGACY_KEY_PASSWORD = $legacyPlainPassword

    # Falhe antes da compilacao se o segredo apontar para uma chave diferente
    # da que assinou a base instalada. Uma assinatura v2 valida, mas de outro
    # certificado, produz INSTALL_FAILED_UPDATE_INCOMPATIBLE nos clientes.
    $keytool = Join-Path $env:JAVA_HOME 'bin\keytool.exe'
    if (-not (Test-Path -LiteralPath $keytool -PathType Leaf)) {
        throw 'keytool ausente para validar o certificado Android'
    }
    $certificateProbe = Join-Path $env:TEMP (
        'churchdaw-android-cert-' + [Guid]::NewGuid().ToString('N') + '.der')
    try {
        & $keytool -exportcert -keystore $keystoreFile `
            -storepass:env CHURCHDAW_UPLOAD_STORE_PASSWORD `
            -alias $credential.UserName -file $certificateProbe | Out-Null
        if ($LASTEXITCODE -ne 0 -or
            -not (Test-Path -LiteralPath $certificateProbe -PathType Leaf)) {
            throw 'Nao foi possivel extrair o certificado da chave Android'
        }
        $actualCertificateSha256 = (Get-FileHash `
            -LiteralPath $certificateProbe -Algorithm SHA256).Hash.
            ToLowerInvariant()
        if ($actualCertificateSha256 -cne
            $expectedSigningCertificateSha256) {
            throw ('Chave Android incompativel com a base instalada: ' +
                   $actualCertificateSha256)
        }
    }
    finally {
        Remove-Item -LiteralPath $certificateProbe -Force `
            -ErrorAction SilentlyContinue
    }

    $localProperties = Join-Path $androidDirectory 'local.properties'
    $androidSdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
    $escapedAndroidSdk = $androidSdk.Replace('\', '\\').Replace(':', '\:')

    Set-Content -LiteralPath $localProperties -Value "sdk.dir=$escapedAndroidSdk" -Encoding Ascii

    Push-Location $androidDirectory
    try {
        $gradleTasks = if ($SkipClean) {
            @('lintRelease_Release', 'bundleRelease_Release')
        } else {
            @('clean', 'lintRelease_Release', 'bundleRelease_Release')
        }

        if ($BuildApk) {
            $gradleTasks += 'assembleRelease_Release'
        }

        & .\gradlew.bat @gradleTasks
        if ($LASTEXITCODE -ne 0) {
            throw "A build Android terminou com codigo $LASTEXITCODE"
        }

        $bundle = Join-Path $androidDirectory 'app\build\outputs\bundle\release_Release\app-release_-release.aab'
        if (-not (Test-Path -LiteralPath $bundle)) {
            throw "A build terminou sem produzir o AAB esperado"
        }

        [xml] $projectXml = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\ChurchDAW.jucer') -Raw
        $versionName = [string] $projectXml.JUCERPROJECT.version
        if ([string]::IsNullOrWhiteSpace($versionName)) {
            throw "Nao foi possivel ler a versao do ChurchDAW.jucer"
        }

        $releaseDirectory = Join-Path $PSScriptRoot '..\Releases\Android'
        $releaseBundle = Join-Path $releaseDirectory "ChurchDAW-$versionName-signed.aab"
        New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
        Copy-Item -LiteralPath $bundle -Destination $releaseBundle -Force

        & $configurationVerifier -ProjectRoot $projectRoot -Bundle $releaseBundle

        $jarsigner = Join-Path $env:JAVA_HOME 'bin\jarsigner.exe'
        if (-not (Test-Path -LiteralPath $jarsigner)) {
            throw "jarsigner nao encontrado: $jarsigner"
        }

        & $jarsigner -verify $releaseBundle
        if ($LASTEXITCODE -ne 0) {
            throw "A assinatura JAR do AAB nao passou na verificacao criptografica"
        }

        $releaseItem = Get-Item -LiteralPath $releaseBundle
        $releaseHash = (Get-FileHash -LiteralPath $releaseBundle -Algorithm SHA256).Hash
        Write-Host "AAB: $($releaseItem.FullName)"
        Write-Host "Bytes: $($releaseItem.Length)"
        Write-Host "SHA-256: $releaseHash"

        if ($BuildApk) {
            $apk = Join-Path $androidDirectory `
                'app\build\outputs\apk\release_\release\app-release_-release.apk'
            if (-not (Test-Path -LiteralPath $apk)) {
                throw "A build terminou sem produzir o APK release esperado"
            }

            $buildToolsDirectory = Get-ChildItem -LiteralPath (Join-Path $androidSdk 'build-tools') `
                -Directory |
                Sort-Object { try { [version] $_.Name } catch { [version] '0.0' } } -Descending |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'apksigner.bat') } |
                Select-Object -First 1
            if ($null -eq $buildToolsDirectory) {
                throw "apksigner nao encontrado no Android SDK"
            }

            $apkSigner = Join-Path $buildToolsDirectory.FullName 'apksigner.bat'
            $apkVerification = @(& $apkSigner verify --verbose --print-certs $apk)
            if ($LASTEXITCODE -ne 0 -or
                -not ($apkVerification -match
                    'Verified using v2 scheme \(APK Signature Scheme v2\): true')) {
                throw "O APK release nao possui uma assinatura v2 valida"
            }
            $certificateMatches = @($apkVerification | ForEach-Object {
                [regex]::Match(
                    [string] $_,
                    '(?i)^Signer #1 certificate SHA-256 digest: ([0-9a-f]{64})$')
            } | Where-Object { $_.Success })
            if ($certificateMatches.Count -ne 1 -or
                $certificateMatches[0].Groups[1].Value.ToLowerInvariant() -cne
                    $expectedSigningCertificateSha256) {
                throw 'APK foi assinado por certificado diferente da base instalada'
            }
            $apkVerification | ForEach-Object { Write-Host $_ }

            $releaseApk = Join-Path $releaseDirectory `
                "ChurchDAW-$versionName-signed.apk"
            $releaseLineage = Join-Path $releaseDirectory `
                'churchdaw-signing-lineage.bin'
            Remove-Item -LiteralPath $releaseApk, $releaseLineage -Force `
                -ErrorAction SilentlyContinue

            & $signingLineageBuilder `
                -InputApk $apk `
                -OutputApk $releaseApk `
                -OutputLineage $releaseLineage `
                -ApkSigner $apkSigner `
                -LegacyF25Keystore $legacyF25KeystoreFile `
                -LegacyF25Alias $legacyCredential.UserName `
                -LegacyF25StorePasswordEnvironmentVariable `
                    'CHURCHDAW_LEGACY_STORE_PASSWORD' `
                -LegacyF25KeyPasswordEnvironmentVariable `
                    'CHURCHDAW_LEGACY_KEY_PASSWORD' `
                -Legacy415aKeystore $legacy415aKeystoreFile `
                -Legacy415aAlias $legacyCredential.UserName `
                -Legacy415aStorePasswordEnvironmentVariable `
                    'CHURCHDAW_LEGACY_STORE_PASSWORD' `
                -Legacy415aKeyPasswordEnvironmentVariable `
                    'CHURCHDAW_LEGACY_KEY_PASSWORD' `
                -CurrentKeystore $keystoreFile `
                -CurrentAlias $credential.UserName `
                -CurrentStorePasswordEnvironmentVariable `
                    'CHURCHDAW_UPLOAD_STORE_PASSWORD' `
                -CurrentKeyPasswordEnvironmentVariable `
                    'CHURCHDAW_UPLOAD_KEY_PASSWORD' `
                -ExpectedLegacyF25CertificateSha256 `
                    $expectedLegacyF25CertificateSha256 `
                -ExpectedLegacy415aCertificateSha256 `
                    $expectedLegacy415aCertificateSha256 `
                -ExpectedCurrentCertificateSha256 `
                    $expectedSigningCertificateSha256
            if ($LASTEXITCODE -ne 0) {
                throw "Gerador da lineage terminou com codigo $LASTEXITCODE"
            }

            $releaseApkItem = Get-Item -LiteralPath $releaseApk
            $releaseApkHash = (Get-FileHash -LiteralPath $releaseApk -Algorithm SHA256).Hash
            $releaseLineageHash = (Get-FileHash -LiteralPath $releaseLineage `
                -Algorithm SHA256).Hash
            Write-Host "APK: $($releaseApkItem.FullName)"
            Write-Host "APK bytes: $($releaseApkItem.Length)"
            Write-Host "APK SHA-256: $releaseApkHash"
            Write-Host "Lineage: $releaseLineage"
            Write-Host "Lineage SHA-256: $releaseLineageHash"
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    Remove-Item Env:\CHURCHDAW_UPLOAD_STORE_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:\CHURCHDAW_UPLOAD_STORE_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item Env:\CHURCHDAW_UPLOAD_KEY_ALIAS -ErrorAction SilentlyContinue
    Remove-Item Env:\CHURCHDAW_UPLOAD_KEY_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item Env:\CHURCHDAW_LEGACY_STORE_PASSWORD `
        -ErrorAction SilentlyContinue
    Remove-Item Env:\CHURCHDAW_LEGACY_KEY_PASSWORD `
        -ErrorAction SilentlyContinue
    if ($previousJavaHome) {
        $env:JAVA_HOME = $previousJavaHome
    } else {
        Remove-Item Env:\JAVA_HOME -ErrorAction SilentlyContinue
    }
    if ($previousGradleOpts) {
        $env:GRADLE_OPTS = $previousGradleOpts
    } else {
        Remove-Item Env:\GRADLE_OPTS -ErrorAction SilentlyContinue
    }
    $plainPassword = $null
    $legacyPlainPassword = $null
    $credential = $null
    $legacyCredential = $null
}

Write-Host 'AAB release assinado e lint concluido com sucesso.'
