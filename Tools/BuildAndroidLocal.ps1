# Build local do APK Android (arm64-v8a Release) no Windows sem depender do GitHub Actions.
# Usa o kit portatil em D:\tools\jdk-21 e D:\tools\android-sdk.
#
# Uso:
#   powershell -NoProfile -ExecutionPolicy Bypass -File D:\ChurchDAW\Tools\BuildAndroidLocal.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File D:\ChurchDAW\Tools\BuildAndroidLocal.ps1 -InstallAdb
param(
    [string] $JavaHome = 'D:\tools\jdk-21',
    [string] $AndroidHome = 'D:\tools\android-sdk',
    [string] $GradleUserHome = 'D:\tools\gradle-home',
    [switch] $Clean,
    [switch] $InstallAdb
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$jucerPath = Join-Path $projectRoot 'ChurchDAW.jucer'
$projucerExe = Join-Path $projectRoot 'JUCE\Projucer.exe'
$androidDir = Join-Path $projectRoot 'Builds\Android'

foreach ($pair in @(
    @((Join-Path $JavaHome 'bin\java.exe'), 'JDK 21 portatil'),
    @((Join-Path $AndroidHome 'build-tools\36.0.0\apksigner.bat'), 'Android Build-Tools 36.0.0'),
    @((Join-Path $AndroidHome 'ndk\28.1.13356709'), 'Android NDK 28.1.13356709'),
    @($projucerExe, 'Projucer.exe')
)) {
    if (-not (Test-Path -LiteralPath $pair[0])) {
        throw "$($pair[1]) nao encontrado em: $($pair[0])"
    }
}

$env:JAVA_HOME = $JavaHome
$env:ANDROID_HOME = $AndroidHome
$env:ANDROID_SDK_ROOT = $AndroidHome
$env:GRADLE_USER_HOME = $GradleUserHome
$env:GRADLE_OPTS = '-Xmx4096m -Dfile.encoding=UTF-8'
$env:PATH = "$JavaHome\bin;$AndroidHome\platform-tools;$env:PATH"

$originalJucerBytes = [IO.File]::ReadAllBytes($jucerPath)
$sw = [Diagnostics.Stopwatch]::StartNew()

try {
    & (Join-Path $projectRoot 'Packaging\ApplyAndroidOpenGLCompatibility.ps1') -ProjectRoot $projectRoot

    if ($Clean -and (Test-Path -LiteralPath $androidDir)) {
        Remove-Item -LiteralPath $androidDir -Recurse -Force
    }

    Write-Host '==> Regenerando projeto Android via Projucer...'
    $projucer = Start-Process -FilePath $projucerExe `
        -ArgumentList @('--resave', $jucerPath) `
        -WindowStyle Hidden -Wait -PassThru
    if ($projucer.ExitCode -ne 0) {
        throw "Projucer falhou com codigo $($projucer.ExitCode)"
    }

    $resSource = Join-Path $projectRoot 'Packaging\AndroidResources'
    $resTarget = Join-Path $androidDir 'app\src\main\res'
    Get-ChildItem -LiteralPath $resSource -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($resSource.Length).TrimStart('\', '/')
        $dest = Join-Path $resTarget $rel
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $dest -Force
    }

    $escapedSdk = $AndroidHome.Replace('\', '\\').Replace(':', '\:')
    Set-Content -LiteralPath (Join-Path $androidDir 'local.properties') `
        -Value "sdk.dir=$escapedSdk" -Encoding Ascii

    Push-Location $androidDir
    try {
        $gradleArgs = @()
        if ($Clean) { $gradleArgs += 'clean' }
        $gradleArgs += @(
            'assembleRelease_Release',
            '--no-daemon',
            '--console=plain',
            '--stacktrace'
        )
        Write-Host '==> Compilando C++ NDK arm64-v8a + Java + APK Release...'
        & .\gradlew.bat @gradleArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Gradle falhou com codigo $LASTEXITCODE"
        }
    } finally {
        Pop-Location
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $unsignedApk = Get-ChildItem -Path (Join-Path $androidDir 'app\build\outputs\apk\release_\release') `
        -Filter '*.apk' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($null -eq $unsignedApk) {
        throw 'APK Release arm64 nao foi encontrado na saida do Gradle'
    }

    $zip = [System.IO.Compression.ZipFile]::OpenRead($unsignedApk.FullName)
    try {
        foreach ($entry in @('AndroidManifest.xml', 'classes.dex', 'lib/arm64-v8a/libjuce_jni.so')) {
            if ($null -eq $zip.GetEntry($entry)) {
                throw "Entrada obrigatoria ausente no APK: $entry"
            }
        }
    } finally {
        $zip.Dispose()
    }

    [xml] $projectXml = [Text.Encoding]::UTF8.GetString($originalJucerBytes)
    $versionName = [string] $projectXml.JUCERPROJECT.version

    $localKeystore = Join-Path $AndroidHome 'churchdaw-local-dev.jks'
    if (-not (Test-Path -LiteralPath $localKeystore)) {
        Write-Host '==> Criando chave persistente de teste local (churchdaw-local-dev.jks)...'
        & (Join-Path $JavaHome 'bin\keytool.exe') -genkeypair -noprompt `
            -keystore $localKeystore `
            -storepass churchdawlocal -keypass churchdawlocal `
            -alias churchdawdev -keyalg RSA -keysize 2048 -validity 10000 `
            -dname 'CN=ChurchDAW Local Dev,O=ChurchDAW,C=BR'
        if ($LASTEXITCODE -ne 0) {
            throw 'Falha ao gerar keystore local de desenvolvimento'
        }
    }

    $distDir = Join-Path $projectRoot 'dist'
    New-Item -ItemType Directory -Force -Path $distDir | Out-Null
    $alignedApk = Join-Path $env:TEMP 'churchdaw-local-aligned.apk'
    $finalApk = Join-Path $distDir "ChurchDAW-$versionName-Android-arm64-local.apk"

    $buildTools = Join-Path $AndroidHome 'build-tools\36.0.0'
    & (Join-Path $buildTools 'zipalign.exe') -f -p 4 $unsignedApk.FullName $alignedApk
    if ($LASTEXITCODE -ne 0) { throw "zipalign falhou ($LASTEXITCODE)" }

    & (Join-Path $buildTools 'apksigner.bat') sign `
        --ks $localKeystore `
        --ks-pass pass:churchdawlocal `
        --ks-key-alias churchdawdev `
        --key-pass pass:churchdawlocal `
        --out $finalApk `
        $alignedApk
    if ($LASTEXITCODE -ne 0) { throw "apksigner sign falhou ($LASTEXITCODE)" }

    Remove-Item -LiteralPath $alignedApk -Force -ErrorAction SilentlyContinue

    $verifyOut = & (Join-Path $buildTools 'apksigner.bat') verify --verbose $finalApk 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host ($verifyOut | Out-String)
        throw "apksigner verify falhou ($LASTEXITCODE)"
    }
    $verifyOut | Select-Object -First 8 | ForEach-Object { Write-Host $_ }

    $apkSizeMb = [Math]::Round((Get-Item -LiteralPath $finalApk).Length / 1MB, 2)
    Write-Host ("`nAPK_ANDROID_PRONTO ({0:n0}s, {1} MB): {2}" -f $sw.Elapsed.TotalSeconds, $apkSizeMb, $finalApk)

    if ($InstallAdb) {
        $adb = Join-Path $AndroidHome 'platform-tools\adb.exe'
        & $adb install -r $finalApk
    }
} finally {
    [IO.File]::WriteAllBytes($jucerPath, $originalJucerBytes)
}
