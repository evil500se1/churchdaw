param(
    [string] $Device = '192.168.100.17:5555',
    [string] $Apk = "$PSScriptRoot\..\Releases\Android\ChurchDAW-1.4.0-signed.apk",
    [string] $OutputDirectory = "$PSScriptRoot\AndroidLoginTests\results",
    [switch] $FullMatrix
)

$ErrorActionPreference = 'Stop'
$package = 'com.churchdaw.app'

function Invoke-Adb {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $Arguments)
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & adb -s $Device @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorAction
    if ($exitCode -ne 0) {
        throw "ADB falhou: adb -s $Device $($Arguments -join ' ')`n$output"
    }
    return $output
}

function Save-Screenshot {
    param([string] $Name)
    $remote = "/sdcard/churchdaw-$Name.png"
    $local = Join-Path $OutputDirectory "$Name.png"
    Invoke-Adb -Arguments @(
        'shell', 'screencap', '-p', $remote
    ) | Out-Null
    Invoke-Adb -Arguments @(
        'pull', $remote, $local
    ) | Out-Null
    if (-not (Test-Path -LiteralPath $local)) {
        throw "Screenshot nao foi criado: $local"
    }
    return $local
}

function Save-UiDump {
    param([string] $Name)
    $local = Join-Path $OutputDirectory "$Name.xml"
    $tree = (
        Invoke-Adb -Arguments @(
            'exec-out', 'uiautomator', 'dump', '/dev/tty'
        ) | Out-String
    )
    Set-Content -LiteralPath $local -Value $tree -Encoding UTF8
    return [pscustomobject]@{
        Path = $local
        Xml = $tree
    }
}

function Open-ImeOnEmail {
    param([string] $Scenario)
    $dump = Save-UiDump -Name "$Scenario-before-ime"
    if ($dump.Xml -notmatch
        'content-desc="E-mail,E-mail"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"') {
        throw "Campo de e-mail ausente antes do IME em $Scenario"
    }

    $x1 = [int] $matches[1]
    $y1 = [int] $matches[2]
    $x2 = [int] $matches[3]
    $y2 = [int] $matches[4]
    $tapX = [int] (($x1 + $x2) / 2)
    $tapY = [int] (($y1 + $y2) / 2)
    Invoke-Adb -Arguments @(
        'shell', 'input', 'touchscreen', 'tap',
        [string] $tapX, [string] $tapY
    ) | Out-Null

    $deadline = (Get-Date).AddSeconds(8)
    do {
        Start-Sleep -Milliseconds 350
        $imeDump = Save-UiDump -Name "$Scenario-ime"
        if ($imeDump.Xml -match
            'content-desc="E-mail,E-mail"[^>]*focused="true"') {
            return $imeDump
        }
    } while ((Get-Date) -lt $deadline)

    throw "IME nao focou o campo de e-mail em $Scenario"
}

function Start-ChurchDaw {
    Invoke-Adb -Arguments @(
        'shell', 'am', 'force-stop', $package
    ) | Out-Null
    Invoke-Adb -Arguments @(
        'shell', 'am', 'start', '-W', '-n',
        "$package/com.churchdaw.app.ChurchDAWActivity"
    ) | Out-Null

    # Abra a Activity diretamente. Passar pelo launcher com `monkey` torna a
    # matriz dependente do Pixel Launcher, que pode entrar em ANR durante
    # redimensionamentos consecutivos mesmo quando o ChurchDAW esta saudavel.
    # Nao use um atraso fixo: a verificacao remota de politica/licenca varia
    # conforme a rede e pode manter a splash por mais de sete segundos. Espere
    # a arvore de acessibilidade confirmar os dois campos do login real.
    Start-Sleep -Milliseconds 1500
    $deadline = (Get-Date).AddSeconds(35)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 750
        $tree = (
            Invoke-Adb -Arguments @(
                'exec-out', 'uiautomator', 'dump', '/dev/tty'
            ) | Out-String
        )
        if ($tree -match 'content-desc="E-mail,E-mail"' -and
            $tree -match 'content-desc="Senha,Senha"') {
            # Uma segunda leitura impede aceitar a arvore antiga que o
            # uiautomator ainda pode devolver durante o primeiro frame depois
            # de force-stop/relaunch.
            Start-Sleep -Milliseconds 900
            $stableTree = (
                Invoke-Adb -Arguments @(
                    'exec-out', 'uiautomator', 'dump', '/dev/tty'
                ) | Out-String
            )
            if ($stableTree -match 'content-desc="E-mail,E-mail"' -and
                $stableTree -match 'content-desc="Senha,Senha"') {
                return
            }
        }
    }
    throw 'A tela de login nao ficou pronta em 35 segundos.'
}

$source = Get-Content -LiteralPath (
    Join-Path $PSScriptRoot '..\Source\AndroidLoginPanel.h'
) -Raw
if ($source -match 'juce::Viewport|ScrollView|LazyColumn') {
    throw 'A tela de login voltou a conter uma estrutura de rolagem.'
}

$connected = Invoke-Adb get-state
if (($connected | Out-String).Trim() -ne 'device') {
    throw "Dispositivo indisponivel: $Device"
}
if (-not (Test-Path -LiteralPath $Apk)) {
    throw "APK nao encontrado: $Apk"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$originalFontScale = (
    Invoke-Adb shell settings get system font_scale | Out-String
).Trim()
$originalAutoRotation = (
    Invoke-Adb shell settings get system accelerometer_rotation | Out-String
).Trim()
$originalUserRotation = (
    Invoke-Adb shell settings get system user_rotation | Out-String
).Trim()

$profiles = @(
    [pscustomobject]@{ Name='compact-320x568'; W=320; H=568; Font=1.0 },
    [pscustomobject]@{ Name='compact-360x800'; W=360; H=800; Font=1.0 },
    [pscustomobject]@{ Name='large-412x915'; W=412; H=915; Font=1.0 },
    [pscustomobject]@{ Name='medium-600x960'; W=600; H=960; Font=1.0 },
    [pscustomobject]@{ Name='tablet-800x1280'; W=800; H=1280; Font=1.0 },
    [pscustomobject]@{ Name='expanded-1024x1366'; W=1024; H=1366; Font=1.0 },
    [pscustomobject]@{ Name='font-150'; W=360; H=800; Font=1.5 },
    [pscustomobject]@{ Name='font-200'; W=360; H=800; Font=2.0 }
)
if (-not $FullMatrix) {
    $profiles = $profiles | Where-Object {
        $_.Name -in @(
            'compact-320x568',
            'compact-360x800',
            'medium-600x960',
            'expanded-1024x1366',
            'font-200'
        )
    }
}

$results = [System.Collections.Generic.List[object]]::new()
try {
    Invoke-Adb -Arguments @(
        'install', '-r', $Apk
    ) | Out-Null
    foreach ($profile in $profiles) {
        foreach ($orientation in @('portrait', 'landscape')) {
            $width = if ($orientation -eq 'portrait') { $profile.W } else { $profile.H }
            $height = if ($orientation -eq 'portrait') { $profile.H } else { $profile.W }
            Invoke-Adb shell wm density 160 | Out-Null
            Invoke-Adb shell wm size "$($width)x$($height)" | Out-Null
            Invoke-Adb shell settings put system font_scale (
                [string]::Format(
                    [Globalization.CultureInfo]::InvariantCulture,
                    '{0:0.0}', $profile.Font)
            ) | Out-Null
            Invoke-Adb logcat -c | Out-Null
            Start-ChurchDaw
            Start-Sleep -Milliseconds 800
            $name = "$($profile.Name)-$orientation"
            $screenshot = Save-Screenshot -Name $name
            $geometryLog = (
                Invoke-Adb -Arguments @(
                    'logcat', '-d', '-t', '250'
                ) | Out-String
            )
            if ($geometryLog -match 'ANDROID_LOGIN_GEOMETRY_FAIL') {
                throw "Elemento fora da janela em $name`n$geometryLog"
            }
            $results.Add([pscustomobject]@{
                Scenario = $name
                WidthDp = $width
                HeightDp = $height
                FontScale = $profile.Font
                Screenshot = $screenshot
                Sha256 = (Get-FileHash -LiteralPath $screenshot -Algorithm SHA256).Hash
                Geometry = 'PASS'
            })
        }
    }

    $imeProfiles = @(
        [pscustomobject]@{
            Name='ime-compact-320x568'; W=320; H=568; Font=1.0
        },
        [pscustomobject]@{
            Name='ime-large-412x915'; W=412; H=915; Font=1.0
        }
    )
    if ($FullMatrix) {
        $imeProfiles += [pscustomobject]@{
            Name='ime-font-200-360x800'; W=360; H=800; Font=2.0
        }
    }

    foreach ($profile in $imeProfiles) {
        Invoke-Adb shell input keyevent KEYCODE_BACK | Out-Null
        Invoke-Adb shell wm density 160 | Out-Null
        Invoke-Adb shell wm size "$($profile.W)x$($profile.H)" | Out-Null
        Invoke-Adb shell settings put system font_scale (
            [string]::Format(
                [Globalization.CultureInfo]::InvariantCulture,
                '{0:0.0}', $profile.Font)
        ) | Out-Null
        Invoke-Adb logcat -c | Out-Null
        Start-ChurchDaw
        $imeDump = Open-ImeOnEmail -Scenario $profile.Name

        foreach ($marker in @(
            'content-desc="E-mail,E-mail"',
            'content-desc="Senha,Senha"',
            'content-desc="Entrar"'
        )) {
            if ($imeDump.Xml -notmatch [regex]::Escape($marker)) {
                throw "Controle essencial ausente com IME em $($profile.Name): $marker"
            }
        }
        foreach ($hiddenMarker in @(
            'content-desc="Preparar biblioteca"',
            'content-desc="Comprar licença',
            'content-desc="Suporte"'
        )) {
            if ($imeDump.Xml -match [regex]::Escape($hiddenMarker)) {
                throw "Conteudo secundario ainda visivel com IME em $($profile.Name): $hiddenMarker"
            }
        }
        if ($imeDump.Xml -match 'scrollable="true"') {
            throw "Rolagem apareceu com IME em $($profile.Name)"
        }

        $screenshot = Save-Screenshot -Name $profile.Name
        $geometryLog = (
            Invoke-Adb -Arguments @(
                'logcat', '-d', '-t', '250'
            ) | Out-String
        )
        if ($geometryLog -match 'ANDROID_LOGIN_GEOMETRY_FAIL') {
            throw "Elemento fora da janela com IME em $($profile.Name)`n$geometryLog"
        }
        $results.Add([pscustomobject]@{
            Scenario = $profile.Name
            WidthDp = $profile.W
            HeightDp = $profile.H
            FontScale = $profile.Font
            Screenshot = $screenshot
            UiDump = $imeDump.Path
            Sha256 = (
                Get-FileHash -LiteralPath $screenshot -Algorithm SHA256
            ).Hash
            Geometry = 'PASS'
            Ime = 'PASS'
            SecondaryContentHidden = 'PASS'
        })
    }
}
finally {
    Invoke-Adb shell input keyevent KEYCODE_BACK | Out-Null
    Invoke-Adb shell wm size reset | Out-Null
    Invoke-Adb shell wm density reset | Out-Null
    if ($originalFontScale) {
        Invoke-Adb shell settings put system font_scale $originalFontScale | Out-Null
    }
    Invoke-Adb shell settings put system accelerometer_rotation $originalAutoRotation | Out-Null
    Invoke-Adb shell settings put system user_rotation $originalUserRotation | Out-Null
}

$report = Join-Path $OutputDirectory 'report.json'
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $report -Encoding UTF8
Write-Output "ANDROID_LOGIN_MATRIX_OK scenarios=$($results.Count) report=$report"
