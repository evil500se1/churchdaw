param(
    [string] $Device = 'emulator-5554',
    [string] $Package = 'com.churchdaw.app',
    [string] $OutputDirectory =
        "$PSScriptRoot\AndroidMainUiTests\results",
    [switch] $FullMatrix
)

$ErrorActionPreference = 'Stop'
$packageRegex = [regex]::Escape($Package)

function Invoke-Adb {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]] $Arguments
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & adb -s $Device @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($exitCode -ne 0) {
        throw "ADB falhou: adb -s $Device $($Arguments -join ' ')`n$output"
    }
    return $output
}

function Save-Screenshot {
    param([string] $Name)
    $remote = "/sdcard/churchdaw-main-$Name.png"
    $local = Join-Path $OutputDirectory "$Name.png"
    Invoke-Adb -Arguments @(
        'shell', 'screencap', '-p', $remote
    ) | Out-Null
    Invoke-Adb -Arguments @(
        'pull', $remote, $local
    ) | Out-Null
    $file = Get-Item -LiteralPath $local
    if ($file.Length -lt 4096) {
        throw "Screenshot vazia ou incompleta: $local ($($file.Length) bytes)"
    }
    return $local
}

function Get-UiDump {
    Invoke-Adb shell rm -f /sdcard/churchdaw-main.xml | Out-Null
    Invoke-Adb shell uiautomator dump /sdcard/churchdaw-main.xml |
        Out-Null
    return (
        Invoke-Adb shell cat /sdcard/churchdaw-main.xml |
        Out-String
    )
}

function Wait-MainUi {
    param(
        [int] $TimeoutSeconds = 30,
        [int] $StabilizeSeconds = 18
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $processSeen = $false
    do {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $pidText = (
            & adb -s $Device shell pidof $Package 2>$null |
            Out-String
        ).Trim()
        $ErrorActionPreference = $previous
        if ($pidText) {
            $processSeen = $true
            break
        }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $deadline)

    if (-not $processSeen) {
        throw "O processo $Package não abriu em ${TimeoutSeconds}s"
    }

    # A janela JUCE pesada precisa estabilizar antes do UiAutomator. Fazer
    # dumps em loop curto no Android 15 deixa o serviço de automação preso
    # como "already registered" e produz falsos FATAL EXCEPTION.
    Start-Sleep -Seconds $StabilizeSeconds
    $lastDump = ''
    $lastError = ''
    for ($attempt = 1; $attempt -le 2; ++$attempt) {
        try {
            $xml = Get-UiDump
            $lastDump = $xml
            if (
                $xml -match ('package="' + $packageRegex + '"') -and
                $xml -match 'Timbres|Instrumentos' -and
                $xml -match 'Mapear' -and
                $xml -match 'Pad' -and
                $xml -match 'Packs|Bibliotecas' -and
                $xml -match 'content-desc="[^"]*udio'
            ) {
                return $xml
            }
        }
        catch {
            $lastError = $_.Exception.Message
            # A árvore de acessibilidade pode ficar indisponível por alguns
            # quadros enquanto o JUCE troca splash/janela ou redimensiona.
        }
        Start-Sleep -Seconds 5
    }

    throw (
        "A DAW autenticada não estabilizou em ${TimeoutSeconds}s. " +
        "O teste não aceita capturar login/splash como falso positivo. " +
        "ErroUi='$lastError' DumpInicio='" +
        ($lastDump.Substring(0, [Math]::Min(800, $lastDump.Length))) +
        "'"
    )
}

function Assert-VisibleBounds {
    param(
        [string] $Xml,
        [int] $Width,
        [int] $Height,
        [string] $Scenario
    )
    $matches = [regex]::Matches(
        $Xml,
        'bounds="\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]"'
    )
    if ($matches.Count -eq 0) {
        throw "Sem bounds acessíveis em $Scenario"
    }
    foreach ($match in $matches) {
        $left = [int] $match.Groups[1].Value
        $top = [int] $match.Groups[2].Value
        $right = [int] $match.Groups[3].Value
        $bottom = [int] $match.Groups[4].Value
        if (
            $left -lt 0 -or $top -lt 0 -or
            $right -gt $Width -or $bottom -gt $Height -or
            $right -lt $left -or $bottom -lt $top
        ) {
            throw (
                "Elemento fora da janela em ${Scenario}: " +
                "[$left,$top][$right,$bottom], viewport=${Width}x${Height}"
            )
        }
    }
}

if ((Invoke-Adb get-state | Out-String).Trim() -ne 'device') {
    throw "Dispositivo indisponivel: $Device"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$originalSize = (Invoke-Adb shell wm size | Out-String).Trim()
$originalDensity = (Invoke-Adb shell wm density | Out-String).Trim()
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
    [pscustomobject]@{ Name='compact-320x568'; W=320; H=568 },
    [pscustomobject]@{ Name='compact-360x800'; W=360; H=800 },
    [pscustomobject]@{ Name='large-412x915'; W=412; H=915 },
    [pscustomobject]@{ Name='medium-600x960'; W=600; H=960 },
    [pscustomobject]@{ Name='tablet-800x1280'; W=800; H=1280 },
    [pscustomobject]@{ Name='expanded-1024x1366'; W=1024; H=1366 }
)
if (-not $FullMatrix) {
    $profiles = $profiles | Where-Object {
        $_.Name -in @(
            'compact-320x568',
            'compact-360x800',
            'medium-600x960',
            'expanded-1024x1366'
        )
    }
}

$results = [System.Collections.Generic.List[object]]::new()
try {
    Invoke-Adb shell settings put system accelerometer_rotation 0 | Out-Null
    Invoke-Adb shell settings put system user_rotation 0 | Out-Null
    Invoke-Adb shell settings put system font_scale 1.0 | Out-Null
    Invoke-Adb shell wm density 160 | Out-Null

    Invoke-Adb -Arguments @(
        'shell', 'monkey', '-p', $Package, '-c',
        'android.intent.category.LAUNCHER', '1'
    ) | Out-Null
    Wait-MainUi -TimeoutSeconds 35 | Out-Null

    foreach ($profile in $profiles) {
        foreach ($orientation in @('portrait', 'landscape')) {
            $width = if ($orientation -eq 'portrait') {
                $profile.W
            } else {
                $profile.H
            }
            $height = if ($orientation -eq 'portrait') {
                $profile.H
            } else {
                $profile.W
            }
            $name = "$($profile.Name)-$orientation"
            Invoke-Adb shell wm size "$($width)x$($height)" | Out-Null
            Invoke-Adb -Arguments @(
                'shell', 'monkey', '-p', $Package, '-c',
                'android.intent.category.LAUNCHER', '1'
            ) | Out-Null
            $xml = Wait-MainUi -TimeoutSeconds 20 `
                -StabilizeSeconds 6
            Assert-VisibleBounds -Xml $xml -Width $width `
                -Height $height -Scenario $name
            Invoke-Adb logcat -c | Out-Null

            # Forca uma pintura sem acionar nenhum controle.
            Invoke-Adb shell input tap 2 2 | Out-Null
            Start-Sleep -Milliseconds 800
            $screenshot = Save-Screenshot -Name $name
            $log = (
                Invoke-Adb logcat -d -t 350 | Out-String
            )
            if ($log -match
                ('FATAL EXCEPTION|SIGSEGV|ANR in ' + $packageRegex)) {
                throw "Falha nativa/Java em $name`n$log"
            }
            $process = (
                Invoke-Adb shell pidof $Package | Out-String
            ).Trim()
            if (-not $process) {
                throw "Processo ChurchDAW encerrou em $name"
            }

            $results.Add([pscustomobject]@{
                Scenario = $name
                WidthDp = $width
                HeightDp = $height
                Screenshot = $screenshot
                Sha256 = (
                    Get-FileHash -LiteralPath $screenshot `
                        -Algorithm SHA256
                ).Hash
                ProcessAlive = $true
                CrashLog = 'PASS'
                MainUiMarkers = 'PASS'
                VisibleBounds = 'PASS'
            })
        }
    }
}
finally {
    Invoke-Adb shell wm size reset | Out-Null
    Invoke-Adb shell wm density reset | Out-Null
    if ($originalFontScale) {
        Invoke-Adb shell settings put system font_scale `
            $originalFontScale | Out-Null
    }
    Invoke-Adb shell settings put system accelerometer_rotation `
        $originalAutoRotation | Out-Null
    Invoke-Adb shell settings put system user_rotation `
        $originalUserRotation | Out-Null
}

$report = Join-Path $OutputDirectory 'report.json'
$results | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $report -Encoding UTF8
Write-Output (
    "ANDROID_MAIN_UI_MATRIX_OK scenarios=$($results.Count) " +
    "report=$report"
)
