param(
    [string] $SecretsDirectory = "$env:USERPROFILE\ChurchDAW-Secrets",
    [switch] $SkipClean,
    [switch] $BuildApk
)

$ErrorActionPreference = 'Stop'

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$projectFile = Join-Path $projectRoot 'ChurchDAW.jucer'
$projucer = Join-Path $projectRoot 'JUCE\Projucer.exe'
$configurationVerifier = Join-Path $PSScriptRoot 'AssertAndroidBuildConfiguration.ps1'
$credentialFile = Join-Path $SecretsDirectory 'churchdaw-upload.credential.xml'
$keystoreFile = Join-Path $SecretsDirectory 'churchdaw-upload.jks'
$androidDirectory = Join-Path $projectRoot 'Builds\Android'
$androidResources = Join-Path $PSScriptRoot 'AndroidResources'
$generatedResourceDirectory = Join-Path $androidDirectory 'app\src\main\res'

function Sync-ChurchDawAndroidResources {
    if (-not (Test-Path -LiteralPath $androidResources)) {
        throw "Recursos Android oficiais ausentes: $androidResources"
    }

    $resourceFiles = Get-ChildItem -LiteralPath $androidResources -Recurse -File
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

$credential = Import-Clixml -LiteralPath $credentialFile
$plainPassword = $credential.GetNetworkCredential().Password
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

            $releaseApk = Join-Path $releaseDirectory "ChurchDAW-$versionName-signed.apk"
            Copy-Item -LiteralPath $apk -Destination $releaseApk -Force

            $buildToolsDirectory = Get-ChildItem -LiteralPath (Join-Path $androidSdk 'build-tools') `
                -Directory |
                Sort-Object { try { [version] $_.Name } catch { [version] '0.0' } } -Descending |
                Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'apksigner.bat') } |
                Select-Object -First 1
            if ($null -eq $buildToolsDirectory) {
                throw "apksigner nao encontrado no Android SDK"
            }

            $apkSigner = Join-Path $buildToolsDirectory.FullName 'apksigner.bat'
            $apkVerification = @(& $apkSigner verify --verbose --print-certs $releaseApk)
            if ($LASTEXITCODE -ne 0 -or
                -not ($apkVerification -match
                    'Verified using v2 scheme \(APK Signature Scheme v2\): true')) {
                throw "O APK release nao possui uma assinatura v2 valida"
            }
            $apkVerification | ForEach-Object { Write-Host $_ }

            $releaseApkItem = Get-Item -LiteralPath $releaseApk
            $releaseApkHash = (Get-FileHash -LiteralPath $releaseApk -Algorithm SHA256).Hash
            Write-Host "APK: $($releaseApkItem.FullName)"
            Write-Host "APK bytes: $($releaseApkItem.Length)"
            Write-Host "APK SHA-256: $releaseApkHash"
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
}

Write-Host 'AAB release assinado e lint concluido com sucesso.'
