param(
    [string] $ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $Bundle = '',
    [string] $CatalogManifest = "$PSScriptRoot\AndroidHosting\app\android\manifest.json",
    [string] $UpdatePolicyManifest = "$PSScriptRoot\AndroidHosting\app\android\update-policy-v2.json",
    [string] $RequiredPolicyFixture = "$PSScriptRoot\AndroidHosting\diagnostics\update-policy-required-test.json",
    [string] $InvalidCatalogFixture = "$PSScriptRoot\AndroidHosting\diagnostics\manifest-policy-valid-catalog-invalid-test.json"
)

$ErrorActionPreference = 'Stop'

function Assert-AndroidCondition {
    param([bool] $Condition, [string] $Message)

    if (-not $Condition) {
        throw $Message
    }
}

function Get-RequiredRegexValue {
    param([string] $Text, [string] $Pattern, [string] $Description)

    $match = [regex]::Match($Text, $Pattern)
    if (-not $match.Success) {
        throw "Nao foi possivel localizar $Description"
    }

    return $match.Groups[1].Value
}

function Assert-LauncherBitmap {
    param(
        [System.Drawing.Bitmap] $Bitmap,
        [string] $Description,
        [bool] $RequireOpaque
    )

    for ($y = 0; $y -lt $Bitmap.Height; ++$y) {
        for ($x = 0; $x -lt $Bitmap.Width; ++$x) {
            $pixel = $Bitmap.GetPixel($x, $y)

            if ($RequireOpaque -and $pixel.A -ne 255) {
                throw (
                    "$Description possui transparencia em ${x}x${y}; " +
                    "o launcher nao pode revelar uma cor externa"
                )
            }

            # Detecta o verde-croma que originou o fundo incorreto sem rejeitar
            # tons neutros claros, nos quais R/G/B naturalmente se aproximam.
            if (
                $pixel.A -gt 0 -and
                $pixel.G -ge 120 -and
                $pixel.G -gt ($pixel.R + 35) -and
                $pixel.G -gt ($pixel.B + 35)
            ) {
                throw (
                    "$Description contem verde-croma em ${x}x${y}: " +
                    "rgba($($pixel.R),$($pixel.G),$($pixel.B),$($pixel.A))"
                )
            }
        }
    }
}

function Assert-LauncherFile {
    param(
        [string] $Path,
        [string] $Description,
        [bool] $RequireOpaque
    )

    $bitmap = [System.Drawing.Bitmap]::FromFile($Path)
    try {
        Assert-LauncherBitmap $bitmap $Description $RequireOpaque
    } finally {
        $bitmap.Dispose()
    }
}

$projectFile = Join-Path $ProjectRoot 'ChurchDAW.jucer'
$servicesFile = Join-Path $ProjectRoot 'Source\ChurchDAWServices.h'
$startupFile = Join-Path $ProjectRoot 'Source\StartupPanel.h'
$aboutFile = Join-Path $ProjectRoot 'Source\AboutPanel.h'
$mainFile = Join-Path $ProjectRoot 'Source\Main.cpp'
$libraryFile = Join-Path $ProjectRoot 'Source\LibraryPanel.h'
$bridgeFile = Join-Path $ProjectRoot 'Source\AndroidJava\com\churchdaw\app\ChurchDAWUpdateBridge.java'
$signedPolicyVerifierFile = Join-Path $ProjectRoot 'Source\AndroidJava\com\churchdaw\app\ChurchDAWSignedPolicyVerifier.java'
$activityFile = Join-Path $ProjectRoot 'Source\AndroidJava\com\churchdaw\app\ChurchDAWActivity.java'
$updatePolicyPublicKeyFile = Join-Path $ProjectRoot 'Packaging\AndroidUpdatePolicyPublicKey.pem'
$generatedManifestFile = Join-Path $ProjectRoot 'Builds\Android\app\src\main\AndroidManifest.xml'
$generatedGradleFile = Join-Path $ProjectRoot 'Builds\Android\app\build.gradle'
$generatedCmakeFile = Join-Path $ProjectRoot 'Builds\Android\app\CMakeLists.txt'
$androidResourceSource = Join-Path $ProjectRoot 'Packaging\AndroidResources'
$generatedResourceRoot = Join-Path $ProjectRoot 'Builds\Android\app\src\main\res'

Add-Type -AssemblyName System.Drawing

foreach ($requiredFile in @(
    $projectFile,
    $servicesFile,
    $startupFile,
    $aboutFile,
    $mainFile,
    $libraryFile,
    $bridgeFile,
    $signedPolicyVerifierFile,
    $activityFile,
    $updatePolicyPublicKeyFile,
    $CatalogManifest,
    $generatedManifestFile,
    $generatedGradleFile,
    $generatedCmakeFile
)) {
    Assert-AndroidCondition (Test-Path -LiteralPath $requiredFile) "Arquivo Android ausente: $requiredFile"
}

$requiredLauncherResources = @(
    'values\colors.xml',
    'drawable\ic_launcher_foreground.xml',
    'drawable\ic_launcher_monochrome.xml',
    'mipmap-anydpi-v26\ic_launcher.xml',
    'mipmap-anydpi-v33\ic_launcher.xml'
)

foreach ($density in @('ldpi', 'mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi')) {
    $requiredLauncherResources += "mipmap-$density\ic_launcher.png"
    $requiredLauncherResources += "drawable-$density\icon.png"
}

foreach ($relativeResource in $requiredLauncherResources) {
    $sourceResource = Join-Path $androidResourceSource $relativeResource
    $generatedResource = Join-Path $generatedResourceRoot $relativeResource

    Assert-AndroidCondition (Test-Path -LiteralPath $sourceResource) (
        "Recurso oficial do launcher ausente: $sourceResource"
    )
    Assert-AndroidCondition (Test-Path -LiteralPath $generatedResource) (
        "Recurso do launcher nao foi sincronizado apos o Projucer: $generatedResource"
    )
    Assert-AndroidCondition (
        (Get-FileHash -LiteralPath $sourceResource -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $generatedResource -Algorithm SHA256).Hash
    ) "Recurso gerado diverge da fonte oficial: $relativeResource"
}

foreach ($density in @('ldpi', 'mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi')) {
    foreach ($resourceName in @(
        "mipmap-$density\ic_launcher.png",
        "drawable-$density\icon.png"
    )) {
        Assert-LauncherFile `
            (Join-Path $androidResourceSource $resourceName) `
            "Icone legado $resourceName" `
            $true
    }
}

[xml] $adaptiveIconV26 = Get-Content -LiteralPath (
    Join-Path $androidResourceSource 'mipmap-anydpi-v26\ic_launcher.xml'
) -Raw
[xml] $adaptiveIconV33 = Get-Content -LiteralPath (
    Join-Path $androidResourceSource 'mipmap-anydpi-v33\ic_launcher.xml'
) -Raw
[xml] $launcherColors = Get-Content -LiteralPath (
    Join-Path $androidResourceSource 'values\colors.xml'
) -Raw
$resourceAndroidNamespace = 'http://schemas.android.com/apk/res/android'

Assert-AndroidCondition (
    $adaptiveIconV26.DocumentElement.LocalName -eq 'adaptive-icon' -and
    $adaptiveIconV26.DocumentElement.background.GetAttribute(
        'drawable', $resourceAndroidNamespace
    ) -eq '@color/ic_launcher_background' -and
    $adaptiveIconV26.DocumentElement.foreground.GetAttribute(
        'drawable', $resourceAndroidNamespace
    ) -eq '@drawable/ic_launcher_foreground'
) "Icone adaptativo v26 nao referencia background/foreground oficiais"
Assert-AndroidCondition (
    $adaptiveIconV33.DocumentElement.monochrome.GetAttribute(
        'drawable', $resourceAndroidNamespace
    ) -eq '@drawable/ic_launcher_monochrome'
) "Icone tematico monocromatico v33 ausente ou incorreto"
$launcherBackground = $launcherColors.resources.color |
    Where-Object { [string] $_.name -eq 'ic_launcher_background' } |
    Select-Object -First 1
Assert-AndroidCondition (
    $null -ne $launcherBackground -and
    [string] $launcherBackground.'#text' -match '^#(?:1[0-9A-Fa-f]{5}|[0-9A-Fa-f]{8})$'
) "Fundo opaco escuro/vermelho do launcher nao esta definido"

[xml] $projectXml = Get-Content -LiteralPath $projectFile -Raw
$generatedCmake = Get-Content -LiteralPath $generatedCmakeFile -Raw
$project = $projectXml.JUCERPROJECT
$android = $project.EXPORTFORMATS.ANDROIDSTUDIO
$androidProjectFilePaths = @(
    $projectXml.SelectNodes('/JUCERPROJECT/MAINGROUP//FILE') |
        ForEach-Object { ([string] $_.file).Replace('/', '\') }
)
$versionName = [string] $project.version
$packageId = [string] $project.bundleIdentifier
$versionCodeText = [string] $android.androidVersionCode
$versionCode = 0

foreach ($requiredAndroidSource in @(
    'Source\SampleStream.h',
    'Source\WorshipReverb.h',
    'Source\StereoLookaheadLimiter.h',
    'Source\ChannelToneShaper.h',
    'Source\ChannelRotarySpeaker.h',
    'Source\MidiMappingSafety.h',
    'Source\PerformanceMidiSafety.h'
)) {
    Assert-AndroidCondition (
        $androidProjectFilePaths -contains $requiredAndroidSource
    ) "Fonte Android nao esta persistida no ChurchDAW.jucer: $requiredAndroidSource"

    $generatedAndroidSource =
        '../../../' + $requiredAndroidSource.Replace('\', '/')
    Assert-AndroidCondition (
        $generatedCmake.Contains(('"' + $generatedAndroidSource + '"'))
    ) "Fonte Android nao chegou ao CMake gerado: $requiredAndroidSource"
}

Assert-AndroidCondition (
    [int]::TryParse($versionCodeText, [ref] $versionCode) -and $versionCode -gt 0
) "androidVersionCode invalido no ChurchDAW.jucer"
Assert-AndroidCondition ($packageId -eq 'com.churchdaw.app') "Bundle ID inesperado no ChurchDAW.jucer"
Assert-AndroidCondition ($versionName -match '^\d+\.\d+\.\d+$') "Versao semantica invalida no ChurchDAW.jucer"
Assert-AndroidCondition (
    [string] $android.androidActivitySubClassName -eq 'com.churchdaw.app.ChurchDAWActivity'
) "Activity Android persistente nao esta configurada no ChurchDAW.jucer"
Assert-AndroidCondition (
    [string] $android.androidAdditionalJavaFolders -match '(?m)^Source/AndroidJava$'
) "Source/AndroidJava nao esta persistido no ChurchDAW.jucer"
Assert-AndroidCondition (
    [string] $android.androidAdditionalJavaFolders -match
        '(?m)^JUCE/modules/juce_gui_basics/native/javaopt/app$'
) "Pasta oficial que fornece com.rmsl.juce.Receiver nao esta configurada"
Assert-AndroidCondition (
    [string] $android.androidPushNotifications -eq '0'
) "Push notifications foram ativadas sem necessidade no ChurchDAW.jucer"
Assert-AndroidCondition (
    [string] $android.androidManifestCustomXmlElements -match
        'android\.hardware\.wifi.+android:required="false"'
) "Wi-Fi nao esta marcado como feature opcional no ChurchDAW.jucer"
Assert-AndroidCondition (
    [string] $android.androidManifestCustomXmlElements -match
        'android:allowBackup="false"'
) "Auto Backup privado nao esta desativado no ChurchDAW.jucer"
Assert-AndroidCondition (
    [string] $android.androidManifestCustomXmlElements -match
        'android:icon="@mipmap/ic_launcher"' -and
    [string] $android.androidManifestCustomXmlElements -match
        'android:roundIcon="@mipmap/ic_launcher"'
) "Manifest persistente nao usa o launcher adaptativo em icon/roundIcon"
Assert-AndroidCondition (
    [string] $android.androidDependencies -notmatch 'com\.google\.android\.play:app-update'
) "ChurchDAW.jucer ainda depende de Play Core"

$services = Get-Content -LiteralPath $servicesFile -Raw
$cppVersionCode = [int] (Get-RequiredRegexValue $services `
    'currentVersionCode\s*=\s*(\d+)\s*;' 'currentVersionCode no C++')
$cppVersionName = Get-RequiredRegexValue $services `
    'currentVersionName\s*=\s*"([^"]+)"\s*;' 'currentVersionName no C++'
$cppPackageId = Get-RequiredRegexValue $services `
    'packageId\s*=\s*"([^"]+)"\s*;' 'packageId no C++'

Assert-AndroidCondition ($cppVersionCode -eq $versionCode) (
    "versionCode divergente: ChurchDAW.jucer=$versionCode, C++=$cppVersionCode"
)
Assert-AndroidCondition ($cppVersionName -eq $versionName) (
    "versionName divergente: ChurchDAW.jucer=$versionName, C++=$cppVersionName"
)
Assert-AndroidCondition ($cppPackageId -eq $packageId) (
    "packageId divergente: ChurchDAW.jucer=$packageId, C++=$cppPackageId"
)

[xml] $generatedManifest = Get-Content -LiteralPath $generatedManifestFile -Raw
$androidNamespace = 'http://schemas.android.com/apk/res/android'
$manifestRoot = $generatedManifest.DocumentElement
$generatedVersionCode = [int] $manifestRoot.GetAttribute('versionCode', $androidNamespace)
$generatedVersionName = $manifestRoot.GetAttribute('versionName', $androidNamespace)
$generatedActivity = $generatedManifest.SelectSingleNode('/manifest/application/activity')
$generatedApplication = $generatedManifest.SelectSingleNode('/manifest/application')
$generatedActivityName = if ($null -ne $generatedActivity) {
    $generatedActivity.GetAttribute('name', $androidNamespace)
} else {
    ''
}

Assert-AndroidCondition ($generatedVersionCode -eq $versionCode) (
    "AndroidManifest gerado usa versionCode $generatedVersionCode; esperado $versionCode"
)
Assert-AndroidCondition ($generatedVersionName -eq $versionName) (
    "AndroidManifest gerado usa versionName $generatedVersionName; esperado $versionName"
)
Assert-AndroidCondition (
    $generatedActivityName -eq 'com.churchdaw.app.ChurchDAWActivity'
) "AndroidManifest gerado nao usa ChurchDAWActivity"
Assert-AndroidCondition (
    $null -ne $generatedApplication -and
    $generatedApplication.GetAttribute('allowBackup', $androidNamespace) -eq 'false'
) "AndroidManifest gerado permite Auto Backup do acervo privado"
Assert-AndroidCondition (
    $null -ne $generatedApplication -and
    $generatedApplication.GetAttribute('icon', $androidNamespace) -eq
        '@mipmap/ic_launcher' -and
    $generatedApplication.GetAttribute('roundIcon', $androidNamespace) -eq
        '@mipmap/ic_launcher'
) "AndroidManifest gerado nao usa @mipmap/ic_launcher em icon/roundIcon"
$generatedWifiFeature = $generatedManifest.SelectNodes('/manifest/uses-feature') |
    Where-Object {
        $_.GetAttribute('name', $androidNamespace) -eq 'android.hardware.wifi'
    } |
    Select-Object -First 1
Assert-AndroidCondition (
    $null -ne $generatedWifiFeature -and
    $generatedWifiFeature.GetAttribute('required', $androidNamespace) -eq 'false'
) "AndroidManifest gerado filtra aparelhos sem feature Wi-Fi"

$generatedGradle = Get-Content -LiteralPath $generatedGradleFile -Raw
Assert-AndroidCondition (
    $generatedGradle.Contains('"../../../Source/AndroidJava"')
) "Gradle gerado nao compila Source/AndroidJava"
Assert-AndroidCondition (
    $generatedGradle.Contains('juce_gui_basics/native/javaopt/app')
) "Gradle gerado nao compila o Receiver registrado no Manifest"
Assert-AndroidCondition (
    -not $generatedGradle.Contains('com.google.android.play:app-update')
) "Gradle gerado ainda inclui Play Core apesar da distribuicao web"

$mainSource = Get-Content -LiteralPath $mainFile -Raw
$startupSource = Get-Content -LiteralPath $startupFile -Raw
$aboutSource = Get-Content -LiteralPath $aboutFile -Raw
$librarySource = Get-Content -LiteralPath $libraryFile -Raw
$bridgeSource = Get-Content -LiteralPath $bridgeFile -Raw
$signedPolicyVerifierSource = Get-Content -LiteralPath $signedPolicyVerifierFile -Raw
$activitySource = Get-Content -LiteralPath $activityFile -Raw

Assert-AndroidCondition (
    $mainSource.Contains('getInstalledAndroidVersionCode()') -and
    $mainSource.Contains('installedVersionCode < effectiveMinimum') -and
    $mainSource.Contains('check.updatePolicy')
) "Gate obrigatorio nao consulta o versionCode instalado"
Assert-AndroidCondition (
    $startupSource.Contains('startWebsiteUpdate') -and
    $startupSource.Contains('churchdaw.com')
) "Gate obrigatorio nao encaminha o APK para o site oficial"
Assert-AndroidCondition (
    $mainSource.Contains('confirmed-android-version.floor') -and
    $mainSource.Contains('setConfirmedAndroidVersionFloor') -and
    $mainSource.Contains('playEligibility == eligibilityConfirmed') -and
    $mainSource.Contains('policyWasFetchedLive') -and
    $mainSource.Contains('policy.cryptographicallyVerified') -and
    $mainSource.Contains('policy.mode == "rollback"') -and
    $mainSource.Contains(
        'confirmedMinimum > policy.minimumVersionCode'
    )
) "Gate nao usa piso confirmado com rollback autoritativo"
Assert-AndroidCondition (
    $mainSource.Contains('queryImmediateUpdateEligibilityBlocking') -and
    $bridgeSource.Contains(
        'queryImmediateUpdateEligibilityBlocking('
    ) -and
    $bridgeSource.Contains('openWebsiteUpdate') -and
    $bridgeSource.Contains('isOfficialApkUrl') -and
    -not $bridgeSource.Contains('com.google.android.play.core')
) "Candidato de update nao usa o fluxo seguro do site oficial"
Assert-AndroidCondition (
    $services.Contains('update-policy-v2.json') -and
    $services.Contains('struct UpdatePolicy') -and
    $services.Contains('checkRemoteUpdatePolicy') -and
    $services.Contains('acceptSignedUpdatePolicy') -and
    $services.Contains('policySerial') -and
    $services.Contains('payloadSha256') -and
    $services.Contains('catalogSha256') -and
    $services.Contains('Manifesto nao corresponde ao SHA-256 assinado') -and
    $services.Contains('isOfficialLibraryDownloadUrl') -and
    $services.Contains('extractArchiveEntryWithLimits') -and
    $services.Contains('ZIP expandiu alem do tamanho declarado') -and
    $services.Contains('Staging contem marker final invalido') -and
    $services.Contains('Relogio HTTPS regrediu') -and
    $services.Contains('Replay de uma politica antiga rejeitado') -and
    $services.Contains('Rollback nao revoga exatamente o estado atual') -and
    $services.Contains('findAndroidAppClass') -and
    $services.Contains('withNumRedirectsToFollow (0)') -and
    $services.Contains('maximumUpdateMessageCharacters') -and
    $services.Contains('isSafeUpdateVersionName') -and
    $services.Contains('isSafeUpdateMessage') -and
    $services.Contains('invisibleFormatting') -and
    $services.Contains('policyDeadline') -and
    $services.Contains('catalogDeadline')
) "Politica update-only separada, limitada e sem redirects nao esta completa"
Assert-AndroidCondition (
    $signedPolicyVerifierSource.Contains('SHA256withRSA') -and
    $signedPolicyVerifierSource.Contains('RSA_3072_SIGNATURE_BYTES = 384') -and
    $signedPolicyVerifierSource.Contains('churchdaw-update-2026-01') -and
    $services.Contains('ChurchDAWSignedPolicyVerifier') -and
    $services.Contains('verifyAndroidSignedPolicyEnvelope')
) "Verificador criptografico da politica assinada esta incompleto"
Assert-AndroidCondition (
    $mainSource.Contains('void suspended() override') -and
    $mainSource.Contains('void resumed() override') -and
    $mainSource.Contains('beginForegroundUpdateCheck') -and
    $mainSource.Contains('handleForegroundUpdateCheck') -and
    $mainSource.Contains('mainWindow = nullptr; // fecha callbacks de audio/MIDI') -and
    $mainSource.Contains('foregroundRevalidationPending') -and
    $mainSource.Contains('beginStartupRevalidationBeforeMain') -and
    $mainSource.Contains('Cobre o intervalo entre beginMainWindow()')
) "Gate nao revalida e bloqueia a DAW ao voltar ao foreground"
Assert-AndroidCondition (
    $mainSource.Contains('requiredUpdateGateActive') -and
    $mainSource.Contains('installedVersionCode < requiredUpdateFloor') -and
    $mainSource.Contains('handleAndroidForegroundReturn') -and
    $mainSource.Contains('refaca o cold')
) "Gate nao confirma e libera com seguranca uma atualizacao concluida pelo Android"
Assert-AndroidCondition (
    $services.Contains('starter-choice-completed') -and
    $services.Contains('starterChoiceWasCompleted') -and
    $startupSource.Contains('rememberStarterChoiceCompleted') -and
    $mainSource.Contains('ChurchDAWServices::starterChoiceWasCompleted()')
) "Escolha de abrir sem samples nao e lembrada apos a primeira configuracao"
Assert-AndroidCondition (
    $services.Contains('legacy-library-prompt-handled') -and
    $services.Contains('legacyLibraryPromptWasHandled') -and
    $services.Contains('rememberLegacyLibraryPromptHandled') -and
    $services.Contains('hasLegacyInstalledLibrary') -and
    $mainSource.Contains('! ChurchDAWServices::legacyLibraryPromptWasHandled()') -and
    $mainSource.Contains('rememberLegacyLibraryPromptHandled') -and
    -not $mainSource.Contains('const auto oldLibrary = AppPaths::legacySharedDataFolder()')
) "Oferta opcional do acervo antigo nao esta persistida ou ainda sonda arquivos antes da permissao"
Assert-AndroidCondition (
    $startupSource.Contains('Voce ja usava o ChurchDAW neste aparelho?') -and
    $startupSource.Contains('Essa etapa e opcional') -and
    $startupSource.Contains('Nada sera movido, copiado ou apagado') -and
    -not $startupSource.Contains('Encontramos samples de uma versao anterior')
) "Tela inicial afirma ter encontrado samples antes da permissao ou nao explica a escolha"
Assert-AndroidCondition (
    $librarySource.Contains('Liberar acervo antigo') -and
    $librarySource.Contains('requestLegacyLibraryAccess') -and
    $librarySource.Contains('RuntimePermissions::request') -and
    $librarySource.Contains('hasLegacyInstalledLibrary') -and
    $librarySource.Contains('Tentar acesso novamente')
) "Bibliotecas nao oferece recuperacao do acesso ao acervo antigo"
Assert-AndroidCondition (
    ([regex]::Matches(
        $startupSource,
        'rememberStarterChoiceCompleted\(\)'
    )).Count -ge 3
) "Caminho de sucesso/falha do instalador nao persiste a escolha inicial"
Assert-AndroidCondition (
    $services.Contains('paidLibraryPurchasesEnabled = false') -and
    $services.Contains('! paidLibraryPurchasesEnabled') -and
    $aboutSource.Contains('politica-de-privacidade.html')
) "Protecoes de loja ou politica de privacidade nao estao presentes"
Assert-AndroidCondition (
    -not $aboutSource.Contains('Vers\xc3\xa3o 1.1.0') -and
    $aboutSource.Contains('getApplicationVersion()')
) "Tela Sobre nao acompanha a versao real do aplicativo"
Assert-AndroidCondition (
    $aboutSource.Contains('politica-de-privacidade.html')
) "Tela Sobre nao oferece acesso a politica de privacidade"
Assert-AndroidCondition (
    $mainSource.Contains('#if JUCE_ANDROID')
) "Gate Android perdeu a protecao de plataforma"
Assert-AndroidCondition (
    $startupSource.Contains(
        'self->activeUpdatePolicy.playStoreUrl'
    ) -and
    -not $startupSource.Contains('rememberPlayUpdateFloor') -and
    $services.Contains('setConfirmedWebsiteUpdateFloor') -and
    $services.Contains(
        '"(Landroid/app/Activity;Ljava/lang/String;)Z"'
    ) -and
    $services.Contains(
        '"(Landroid/app/Activity;I)I"'
    ) -and
    $bridgeSource.Contains(
        'openWebsiteUpdate(Activity activity, String updateUrl)'
    ) -and
    $bridgeSource.Contains(
        'setConfirmedRequiredVersion('
    )
) "Piso obrigatorio nao atravessa C++/JNI/site"
Assert-AndroidCondition (
    $bridgeSource.Contains('APP_PACKAGE = "com.churchdaw.app"')
) "Bridge nao possui package ID estrito"
Assert-AndroidCondition (
    $bridgeSource.Contains('Intent.ACTION_VIEW') -and
    $bridgeSource.Contains('android.net.Uri') -and
    $bridgeSource.Contains('isOfficialApkUrl') -and
    -not $bridgeSource.Contains('com.google.android.play.core') -and
    $startupSource.Contains('Atualizar no ChurchDAW') -and
    $startupSource.Contains('churchdaw.com')
) "Gate nao abre o APK oficial nem explica a instalacao"
Assert-AndroidCondition (
    $bridgeSource.Contains('REQUIRED_VERSION_KEY') -and
    $bridgeSource.Contains('putInt(REQUIRED_VERSION_KEY, requested)') -and
    $bridgeSource.Contains('remove(REQUIRED_VERSION_KEY)') -and
    -not $bridgeSource.Contains(
        'Math.max(' + [Environment]::NewLine +
        '                clampVersionCode(prefs.getInt(REQUIRED_VERSION_KEY'
    )
) "Bridge web nao preserva/reaplica o piso obrigatorio"
Assert-AndroidCondition (
    $bridgeSource.Contains('Intent.ACTION_VIEW') -and
    $bridgeSource.Contains('Uri.parse(updateUrl)')
) "Bridge nao abre o APK assinado pelo Android"
Assert-AndroidCondition (
    $activitySource.Contains('ChurchDAWUpdateBridge.resumeImmediateUpdate(this)')
) "ChurchDAWActivity nao retoma atualizacao no onResume"
Assert-AndroidCondition (
    $activitySource.Contains('ChurchDAWUpdateBridge.handleActivityResult')
) "ChurchDAWActivity nao trata o resultado da atualizacao"
Assert-AndroidCondition (
    $activitySource.Contains('extends Activity') -and
    $activitySource.Contains('import android.app.Activity;') -and
    -not $activitySource.Contains('extends JuceActivity') -and
    -not $activitySource.Contains('import com.rmsl.juce.JuceActivity;')
) "ChurchDAWActivity nao segue o ciclo de vida correto do JUCE sem push"
Assert-AndroidCondition (
    $activitySource.Contains('initEdgeToEdge()') -and
    $activitySource.Contains('setDecorFitsSystemWindows(false)')
) "ChurchDAWActivity perdeu a configuracao edge-to-edge"

if (Test-Path -LiteralPath $CatalogManifest) {
    $catalog = Get-Content -LiteralPath $CatalogManifest -Raw | ConvertFrom-Json
    Assert-AndroidCondition ($catalog.package_id -eq $packageId) (
        "Manifesto remoto aponta para outro pacote: $($catalog.package_id)"
    )
    Assert-AndroidCondition (
        [int] $catalog.minimum_version_code -le $versionCode
    ) (
        "O AAB versionCode $versionCode seria bloqueado pelo minimum_version_code " +
        "$($catalog.minimum_version_code)"
    )
    Assert-AndroidCondition (
        [int] $catalog.minimum_version_code -le [int] $catalog.latest_version_code
    ) "Faixa de versoes invalida no manifesto remoto"
} else {
    $catalog = $null
}

Assert-AndroidCondition (
    Test-Path -LiteralPath $UpdatePolicyManifest
) "Politica update-only ausente: $UpdatePolicyManifest"

$updateEnvelope = Get-Content -LiteralPath $UpdatePolicyManifest -Raw |
    ConvertFrom-Json
Assert-AndroidCondition (
    @($updateEnvelope.PSObject.Properties).Count -eq 5 -and
    [int] $updateEnvelope.schema_version -eq 2 -and
    [string] $updateEnvelope.algorithm -eq 'SHA256withRSA' -and
    [string] $updateEnvelope.key_id -eq 'churchdaw-update-2026-01'
) "Envelope da politica assinada possui schema/algoritmo/chave inesperados"

try {
    $signedPayloadBytes = [Convert]::FromBase64String(
        [string] $updateEnvelope.payload
    )
    $signedPolicySignature = [Convert]::FromBase64String(
        [string] $updateEnvelope.signature
    )
} catch {
    throw "Envelope da politica possui Base64 invalido: $($_.Exception.Message)"
}

Assert-AndroidCondition (
    $signedPayloadBytes.Length -gt 0 -and
    $signedPayloadBytes.Length -le (24 * 1024) -and
    $signedPolicySignature.Length -eq 384
) "Payload ou assinatura da politica possui tamanho inesperado"

$strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
$signedPayloadText = $strictUtf8.GetString($signedPayloadBytes)
$updatePolicy = $signedPayloadText | ConvertFrom-Json
Assert-AndroidCondition (
    @($updatePolicy.PSObject.Properties).Count -eq 13 -and
    [int] $updatePolicy.schema_version -eq 2 -and
    [int] $updatePolicy.policy_serial -ge 1 -and
    [string] $updatePolicy.mode -eq 'normal' -and
    [int] $updatePolicy.revokes_policy_serial -eq 0
) "Payload assinado nao e uma politica normal valida"

$catalogSha256 = (
    Get-FileHash -LiteralPath $CatalogManifest -Algorithm SHA256
).Hash.ToLowerInvariant()
Assert-AndroidCondition (
    [string] $updatePolicy.catalog_sha256 -cmatch '^[0-9a-f]{64}$' -and
    [string] $updatePolicy.catalog_sha256 -ceq $catalogSha256
) "SHA-256 assinado do catalogo diverge do manifesto publicado"

$embeddedKeyExpression = [regex]::Match(
    $signedPolicyVerifierSource,
    '(?s)PUBLIC_KEY_DER_BASE64\s*=\s*(?<value>.*?);'
)
Assert-AndroidCondition $embeddedKeyExpression.Success (
    'Nao foi possivel extrair a chave publica do verificador Java'
)
$embeddedKeyChunks = [regex]::Matches(
    $embeddedKeyExpression.Groups['value'].Value,
    '"([^"]*)"'
)
$embeddedKeyBase64 = (
    $embeddedKeyChunks | ForEach-Object { $_.Groups[1].Value }
) -join ''
$publicKeyBase64 = (
    (Get-Content -LiteralPath $updatePolicyPublicKeyFile -Raw) `
        -replace '-----BEGIN PUBLIC KEY-----', '' `
        -replace '-----END PUBLIC KEY-----', '' `
        -replace '\s', ''
)
Assert-AndroidCondition (
    $embeddedKeyBase64 -eq $publicKeyBase64
) "Chave publica Java diverge da chave usada para validar a hospedagem"

$openssl = 'C:\Program Files\Git\usr\bin\openssl.exe'
Assert-AndroidCondition (Test-Path -LiteralPath $openssl -PathType Leaf) (
    "OpenSSL ausente para validar a assinatura: $openssl"
)
$policyVerifyFolder = Join-Path ([IO.Path]::GetTempPath()) (
    'churchdaw-policy-assert-' + [Guid]::NewGuid().ToString('N')
)
$policyPayloadFile = Join-Path $policyVerifyFolder 'payload.json'
$policySignatureFile = Join-Path $policyVerifyFolder 'payload.sig'
New-Item -ItemType Directory -Path $policyVerifyFolder | Out-Null
try {
    [IO.File]::WriteAllBytes($policyPayloadFile, $signedPayloadBytes)
    [IO.File]::WriteAllBytes($policySignatureFile, $signedPolicySignature)
    & $openssl dgst -sha256 -verify $updatePolicyPublicKeyFile `
        -signature $policySignatureFile $policyPayloadFile | Out-Null
    Assert-AndroidCondition ($LASTEXITCODE -eq 0) (
        'Assinatura RSA da politica update-only e invalida'
    )
} finally {
    Remove-Item -LiteralPath $policyVerifyFolder -Recurse -Force
}

Assert-AndroidCondition ($updatePolicy.package_id -eq $packageId) (
    "Politica assinada aponta para outro pacote: $($updatePolicy.package_id)"
)
Assert-AndroidCondition (
    [int] $updatePolicy.minimum_version_code -le $versionCode
) (
    "O AAB versionCode $versionCode seria bloqueado pela politica assinada " +
    "$($updatePolicy.minimum_version_code)"
)
Assert-AndroidCondition (
    [int] $updatePolicy.minimum_version_code -le
        [int] $updatePolicy.latest_version_code
) "Faixa de versoes invalida na politica update-only"
Assert-AndroidCondition (
    [string] $updatePolicy.play_store_url -match
        '^https://churchdaw\.com/app/android/ChurchDAW-[0-9A-Za-z._+-]{1,48}(?:-signed)?\.apk$'
) "URL de APK inesperada na politica update-only"
Assert-AndroidCondition (
    ([string] $updatePolicy.latest_version_name).Length -le 48 -and
    ([string] $updatePolicy.update_message).Length -le 640
) "Texto remoto excede o limite aceito pelo binario"
$nowEpoch = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
Assert-AndroidCondition (
    [int64] $updatePolicy.issued_at -le ($nowEpoch + 86400) -and
    [int64] $updatePolicy.expires_at -gt $nowEpoch -and
    [int64] $updatePolicy.expires_at -gt [int64] $updatePolicy.issued_at -and
    ([int64] $updatePolicy.expires_at - [int64] $updatePolicy.issued_at) `
        -le (180L * 24 * 60 * 60)
) "Janela temporal da politica assinada e invalida"
if ($null -ne $catalog) {
    Assert-AndroidCondition (
        [int] $updatePolicy.latest_version_code -eq
            [int] $catalog.latest_version_code -and
        [int] $updatePolicy.minimum_version_code -eq
            [int] $catalog.minimum_version_code -and
        [string] $updatePolicy.latest_version_name -eq
            [string] $catalog.latest_version_name -and
        [string] $updatePolicy.play_store_url -eq
            [string] $catalog.play_store_url
    ) "Politica assinada e envelope legado do catalogo estao divergentes"
}

Assert-AndroidCondition (
    Test-Path -LiteralPath $RequiredPolicyFixture
) "Fixture da politica obrigatoria ausente: $RequiredPolicyFixture"
Assert-AndroidCondition (
    Test-Path -LiteralPath $InvalidCatalogFixture
) "Fixture de catalogo invalido ausente: $InvalidCatalogFixture"

$requiredFixture = Get-Content -LiteralPath $RequiredPolicyFixture -Raw |
    ConvertFrom-Json
$invalidCatalogFixtureObject =
    Get-Content -LiteralPath $InvalidCatalogFixture -Raw | ConvertFrom-Json

Assert-AndroidCondition (
    [int] $requiredFixture.minimum_version_code -gt $versionCode -and
    [int] $requiredFixture.latest_version_code -ge
        [int] $requiredFixture.minimum_version_code
) "Fixture obrigatoria nao bloqueia o versionCode atual"
Assert-AndroidCondition (
    [int] $invalidCatalogFixtureObject.minimum_version_code -eq
        [int] $requiredFixture.minimum_version_code -and
    [string] $invalidCatalogFixtureObject.package_id -eq $packageId -and
    [string] $invalidCatalogFixtureObject.libraries[0].id -match '\.\.'
) "Fixture nao combina uma politica valida com um catalogo deliberadamente invalido"
Assert-AndroidCondition (
    -not $services.Contains('auto legacy = parseUpdatePolicy (raw') -and
    -not $services.Contains('serialiseUpdatePolicy')
) "Manifesto/catalogo legado ainda consegue alimentar o gate assinado"

if (-not [string]::IsNullOrWhiteSpace($Bundle)) {
    Assert-AndroidCondition (Test-Path -LiteralPath $Bundle) "AAB ausente: $Bundle"

    $bundleItem = Get-Item -LiteralPath $Bundle
    # ChurchDAW.jucer e a fonte do target Android. O CMake desktop tambem usa
    # Source/, mas inclui arquivos exclusivos de Windows/macOS que nunca entram
    # no AAB. Considerar toda a pasta produziria falso "AAB obsoleto" sempre
    # que apenas o cliente desktop fosse recompilado.
    $androidProjectInputs = @(
        $androidProjectFilePaths | ForEach-Object {
            Get-Item -LiteralPath (Join-Path $ProjectRoot $_)
        }
    )
    $androidJavaInputs = @(
        Get-ChildItem -LiteralPath (
            Join-Path $ProjectRoot 'Source\AndroidJava'
        ) -Recurse -File |
            Where-Object { $_.Extension -eq '.java' }
    )
    $authoritativeInputs = @(
        $projectFile,
        $generatedManifestFile,
        $generatedGradleFile,
        $generatedCmakeFile
    ) | ForEach-Object { Get-Item -LiteralPath $_ }
    $authoritativeInputs += $androidProjectInputs
    $authoritativeInputs += $androidJavaInputs
    $authoritativeInputs += Get-ChildItem -LiteralPath $androidResourceSource `
        -Recurse -File
    $newestInput = $authoritativeInputs |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1

    Assert-AndroidCondition (
        $bundleItem.LastWriteTimeUtc -ge $newestInput.LastWriteTimeUtc
    ) (
        "AAB obsoleto: $($newestInput.FullName) foi alterado depois do bundle"
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($bundleItem.FullName)

    try {
        foreach ($entryName in @(
            'BundleConfig.pb',
            'base/dex/classes.dex',
            'base/manifest/AndroidManifest.xml',
            'base/lib/arm64-v8a/libjuce_jni.so'
        )) {
            Assert-AndroidCondition ($null -ne $zip.GetEntry($entryName)) (
                "Entrada obrigatoria ausente do AAB: $entryName"
            )
        }

        $bundleAdaptiveV26 = $zip.Entries |
            Where-Object {
                $_.FullName -match
                    '^base/res/mipmap-anydpi-v26/ic_launcher\.xml$'
            } |
            Select-Object -First 1
        $bundleAdaptiveV33 = $zip.Entries |
            Where-Object {
                $_.FullName -match
                    '^base/res/mipmap-anydpi-v33/ic_launcher\.xml$'
            } |
            Select-Object -First 1
        Assert-AndroidCondition ($null -ne $bundleAdaptiveV26) (
            'AAB nao empacotou o icone adaptativo Android 8+'
        )
        Assert-AndroidCondition ($null -ne $bundleAdaptiveV33) (
            'AAB nao empacotou o icone monocromatico Android 13+'
        )

        foreach ($density in @(
            'ldpi', 'mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi'
        )) {
            $legacyLauncherEntry = $zip.Entries |
                Where-Object {
                    $_.FullName -match (
                        '^base/res/mipmap-' + [regex]::Escape($density) +
                        '(?:-v4)?/ic_launcher\.png$'
                    )
                } |
                Select-Object -First 1
            Assert-AndroidCondition ($null -ne $legacyLauncherEntry) (
                "AAB nao empacotou ic_launcher legado para $density"
            )

            $launcherStream = $legacyLauncherEntry.Open()
            $launcherBitmap = [System.Drawing.Bitmap]::FromStream(
                $launcherStream
            )
            try {
                Assert-LauncherBitmap `
                    $launcherBitmap `
                    "Icone $density empacotado no AAB" `
                    $true
            } finally {
                $launcherBitmap.Dispose()
                $launcherStream.Dispose()
            }
        }

        $signature = $zip.Entries |
            Where-Object { $_.FullName -match '^META-INF/.+\.RSA$' } |
            Select-Object -First 1
        Assert-AndroidCondition ($null -ne $signature) "AAB nao possui assinatura JAR"

        $dexEntries = @(
            $zip.Entries |
                Where-Object {
                    $_.FullName -match '^base/dex/classes(?:[0-9]+)?\.dex$'
                } |
                Sort-Object FullName
        )
        Assert-AndroidCondition ($dexEntries.Count -gt 0) (
            'AAB nao contem nenhum DEX base/dex/classes*.dex'
        )

        $dexMemory = [System.IO.MemoryStream]::new()
        try {
            foreach ($dexEntry in $dexEntries) {
                $dexStream = $dexEntry.Open()
                try {
                    $dexStream.CopyTo($dexMemory)
                } finally {
                    $dexStream.Dispose()
                }
            }
            $dexText = [System.Text.Encoding]::UTF8.GetString($dexMemory.ToArray())
        } finally {
            $dexMemory.Dispose()
        }

        foreach ($dexNeedle in @(
            'ChurchDAWActivity',
            'ChurchDAWUpdateBridge',
            'ChurchDAWSignedPolicyVerifier',
            'churchdaw-update-2026-01',
            'getInstalledVersionCode',
            'resumeImmediateUpdate',
            'openWebsiteUpdate'
        )) {
            Assert-AndroidCondition ($dexText.Contains($dexNeedle)) (
                "AAB nao contem simbolo Java obrigatorio: $dexNeedle"
            )
        }

        $manifestEntry = $zip.GetEntry('base/manifest/AndroidManifest.xml')
        $manifestStream = $manifestEntry.Open()
        $manifestMemory = [System.IO.MemoryStream]::new()
        try {
            $manifestStream.CopyTo($manifestMemory)
            $manifestText = [System.Text.Encoding]::UTF8.GetString(
                $manifestMemory.ToArray()
            )
        } finally {
            $manifestMemory.Dispose()
            $manifestStream.Dispose()
        }

        foreach ($manifestNeedle in @(
            $packageId,
            $versionName,
            'com.churchdaw.app.ChurchDAWActivity',
            'ic_launcher'
        )) {
            Assert-AndroidCondition ($manifestText.Contains($manifestNeedle)) (
                "Manifesto dentro do AAB nao contem: $manifestNeedle"
            )
        }
    } finally {
        $zip.Dispose()
    }
}

Write-Host (
    "CONFIGURACAO_ANDROID_OK package=$packageId versionName=$versionName " +
    "versionCode=$versionCode"
)
