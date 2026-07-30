param(
    [string] $Device = 'emulator-5554',
    [string] $Package = 'com.churchdaw.app.qarelease',
    [string] $ExpectedPreset = '(?i)cl.ssico',
    [string] $OutputDirectory =
        "$PSScriptRoot\AndroidMainUiTests\tone-controls"
)

$ErrorActionPreference = 'Stop'

$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) {
    $adb = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'
}
if (-not (Test-Path -LiteralPath $adb)) {
    throw "ADB não encontrado: $adb"
}

function Invoke-Adb {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]] $Arguments
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $output = & $adb -s $Device @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($exitCode -ne 0) {
        throw "ADB falhou: $($Arguments -join ' ')`n$output"
    }
    return $output
}

function Get-UiDump {
    Invoke-Adb shell uiautomator dump /sdcard/churchdaw-tone.xml |
        Out-Null
    return (
        Invoke-Adb shell cat /sdcard/churchdaw-tone.xml |
        Out-String
    )
}

function Get-Nodes {
    param([string] $Xml)
    $nodes = [System.Collections.Generic.List[object]]::new()
    $pattern =
        '<node[^>]*content-desc="([^"]*)"[^>]*' +
        'clickable="(true|false)"[^>]*enabled="(true|false)"[^>]*' +
        'focusable="(true|false)"[^>]*focused="(true|false)"[^>]*' +
        'scrollable="(true|false)"[^>]*[^>]*' +
        'selected="(true|false)"[^>]*' +
        'bounds="\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]"[^>]*>'
    foreach ($match in [regex]::Matches($Xml, $pattern)) {
        $nodes.Add([pscustomobject]@{
            Description = [System.Net.WebUtility]::HtmlDecode(
                $match.Groups[1].Value
            )
            Clickable = $match.Groups[2].Value -eq 'true'
            Enabled = $match.Groups[3].Value -eq 'true'
            Focusable = $match.Groups[4].Value -eq 'true'
            Focused = $match.Groups[5].Value -eq 'true'
            Scrollable = $match.Groups[6].Value -eq 'true'
            Selected = $match.Groups[7].Value -eq 'true'
            Left = [int] $match.Groups[8].Value
            Top = [int] $match.Groups[9].Value
            Right = [int] $match.Groups[10].Value
            Bottom = [int] $match.Groups[11].Value
        })
    }
    return $nodes
}

function Find-Node {
    param(
        [object[]] $Nodes,
        [string] $DescriptionPattern,
        [switch] $Clickable
    )
    $found = $Nodes | Where-Object {
        $_.Description -match $DescriptionPattern -and
        (-not $Clickable -or $_.Clickable)
    } | Select-Object -First 1
    if (-not $found) {
        throw "Controle não encontrado: /$DescriptionPattern/"
    }
    return $found
}

function Tap-Node {
    param([object] $Node)
    $x = [int] (($Node.Left + $Node.Right) / 2)
    $y = [int] (($Node.Top + $Node.Bottom) / 2)
    Invoke-Adb shell input tap $x $y | Out-Null
    Start-Sleep -Milliseconds 500
}

function Assert-MinTarget {
    param(
        [object] $Node,
        [int] $Minimum = 48
    )
    $width = $Node.Right - $Node.Left
    $height = $Node.Bottom - $Node.Top
    if ($width -lt $Minimum -or $height -lt $Minimum) {
        throw (
            "Alvo menor que ${Minimum}dp: '$($Node.Description)' " +
            "${width}x${height}"
        )
    }
}

function Assert-VisibleBounds {
    param(
        [object[]] $Nodes,
        [int] $Width,
        [int] $Height
    )
    foreach ($node in $Nodes) {
        if (
            $node.Left -lt 0 -or $node.Top -lt 0 -or
            $node.Right -gt $Width -or $node.Bottom -gt $Height
        ) {
            throw (
                "Elemento fora da janela: '$($node.Description)' " +
                "[$($node.Left),$($node.Top)]" +
                "[$($node.Right),$($node.Bottom)] " +
                "viewport=${Width}x${Height}"
            )
        }
    }
}

function Save-Screenshot {
    param([string] $Name)
    $remote = "/sdcard/churchdaw-tone-$Name.png"
    $local = Join-Path $OutputDirectory "$Name.png"
    Invoke-Adb -Arguments @('shell', 'screencap', '-p', $remote) |
        Out-Null
    Invoke-Adb -Arguments @('pull', $remote, $local) | Out-Null
    if ((Get-Item -LiteralPath $local).Length -lt 4096) {
        throw "Screenshot inválida: $local"
    }
    return $local
}

function Wait-MainUi {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        Start-Sleep -Seconds 1
        try {
            $xml = Get-UiDump
            if (
                $xml -match [regex]::Escape($Package) -and
                $xml -match 'Canal 1' -and
                $xml -match 'Master'
            ) {
                return $xml
            }
        }
        catch {
            # A árvore JUCE pode ficar indisponível durante o relayout.
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Mixer não estabilizou em 30 s para $Package"
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$originalSize = (Invoke-Adb shell wm size | Out-String).Trim()
$originalDensity = (Invoke-Adb shell wm density | Out-String).Trim()
$originalRotation = (
    Invoke-Adb shell settings get system user_rotation | Out-String
).Trim()
$originalAutoRotation = (
    Invoke-Adb shell settings get system accelerometer_rotation | Out-String
).Trim()
$originalFontScale = (
    Invoke-Adb shell settings get system font_scale | Out-String
).Trim()

$profiles = @(
    [pscustomobject]@{ Name='compact-portrait'; W=360; H=800 },
    [pscustomobject]@{ Name='compact-landscape'; W=640; H=360 },
    [pscustomobject]@{ Name='tablet-portrait'; W=800; H=1280 },
    [pscustomobject]@{ Name='expanded-landscape'; W=1366; H=1024 }
)

$results = [System.Collections.Generic.List[object]]::new()
$pickerValidated = $false
$launchActivity = (
    Invoke-Adb shell cmd package resolve-activity --brief `
        -c android.intent.category.LAUNCHER $Package |
    Where-Object { $_ -match '/' } |
    Select-Object -Last 1
).Trim()
if (-not $launchActivity) {
    throw "Activity de inicialização não encontrada para $Package"
}

try {
    Invoke-Adb shell settings put system accelerometer_rotation 0 |
        Out-Null
    Invoke-Adb shell settings put system font_scale 1.0 | Out-Null
    Invoke-Adb shell wm density 160 | Out-Null

    foreach ($profile in $profiles) {
        Invoke-Adb shell wm size "$($profile.W)x$($profile.H)" |
            Out-Null
        Invoke-Adb shell am force-stop $Package | Out-Null
        Invoke-Adb -Arguments @(
            'shell', 'am', 'start', '-W', '-n', $launchActivity
        ) | Out-Null
        Start-Sleep -Seconds 8

        $xml = Wait-MainUi
        $nodes = @(Get-Nodes -Xml $xml)
        Assert-VisibleBounds -Nodes $nodes `
            -Width $profile.W -Height $profile.H

        foreach ($name in 1..5) {
            $tab = Find-Node -Nodes $nodes `
                -DescriptionPattern "^Canal $name(?:,|$)" -Clickable
            Assert-MinTarget -Node $tab
        }
        $masterTab = Find-Node -Nodes $nodes `
            -DescriptionPattern '^Master(?:,|$)' -Clickable
        Assert-MinTarget -Node $masterTab

        $channelOne = Find-Node -Nodes $nodes `
            -DescriptionPattern '^Canal 1(?:,|$)' -Clickable
        Tap-Node -Node $channelOne
        $xml = Wait-MainUi
        $nodes = @(Get-Nodes -Xml $xml)

        $preset = $nodes | Where-Object {
            $_.Clickable -and $_.Description -match $ExpectedPreset
        } | Select-Object -First 1

        # O timbre pode nao persistir entre reinicios do pacote QA. Nesse caso,
        # carregue o orgao disponivel pelo mesmo fluxo D-pad que certificamos.
        if (-not $preset) {
            $add = Find-Node -Nodes $nodes `
                -DescriptionPattern 'Adicionar instrumento' -Clickable
            Tap-Node -Node $add
            Start-Sleep -Milliseconds 700
            $pickerNodes = @(Get-Nodes -Xml (Get-UiDump))
            $close = Find-Node -Nodes $pickerNodes `
                -DescriptionPattern 'Fechar seletor' -Clickable
            Assert-MinTarget -Node $close
            foreach ($row in $pickerNodes | Where-Object {
                $_.Description -match '^Row \d+$'
            }) {
                Assert-MinTarget -Node $row
            }

            $before = (
                $pickerNodes | Where-Object Selected |
                Select-Object -First 1
            ).Description
            Invoke-Adb shell input keyevent KEYCODE_DPAD_DOWN |
                Out-Null
            Start-Sleep -Milliseconds 300
            $afterNodes = @(Get-Nodes -Xml (Get-UiDump))
            $after = (
                $afterNodes | Where-Object Selected |
                Select-Object -First 1
            ).Description
            if (-not $after -or $after -eq $before) {
                throw "D-pad nao moveu a selecao no picker"
            }
            Invoke-Adb shell input keyevent KEYCODE_ENTER | Out-Null
            Start-Sleep -Seconds 8
            $nodes = @(Get-Nodes -Xml (Wait-MainUi))
            $preset = Find-Node -Nodes $nodes `
                -DescriptionPattern $ExpectedPreset -Clickable
            $pickerValidated = $true
        }
        $on = Find-Node -Nodes $nodes `
            -DescriptionPattern '^ON(?:,|$)' -Clickable
        $eq = Find-Node -Nodes $nodes `
            -DescriptionPattern '^EQ(?:,|$)' -Clickable
        $attack = Find-Node -Nodes $nodes `
            -DescriptionPattern 'ATTACK'
        $release = Find-Node -Nodes $nodes `
            -DescriptionPattern 'RELEASE'
        $rotary = Find-Node -Nodes $nodes `
            -DescriptionPattern 'ROTARY'
        $rotaryMode = Find-Node -Nodes $nodes `
            -DescriptionPattern 'Velocidade do Leslie' -Clickable

        foreach ($target in @(
            $preset, $on, $eq, $attack, $release, $rotary, $rotaryMode
        )) {
            Assert-MinTarget -Node $target
        }

        $envShot = Save-Screenshot -Name "$($profile.Name)-env"

        Tap-Node -Node $eq
        $eqXml = Wait-MainUi
        $eqNodes = @(Get-Nodes -Xml $eqXml)
        foreach ($label in @('LOW', 'MID', 'HIGH')) {
            $control = Find-Node -Nodes $eqNodes `
                -DescriptionPattern $label
            Assert-MinTarget -Node $control
        }
        $eqShot = Save-Screenshot -Name "$($profile.Name)-eq"

        $masterTab = Find-Node -Nodes $eqNodes `
            -DescriptionPattern '^Master(?:,|$)' -Clickable
        Tap-Node -Node $masterTab
        $masterXml = Wait-MainUi
        $masterNodes = @(Get-Nodes -Xml $masterXml)
        foreach ($label in @(
            '^PANIC',
            '^(?:SUSTAIN|SUS)(?:,|$)',
            '^(?:INV\. PEDAL|INV)(?:,|$)',
            '^(?:AUTO EQ|EQ)(?:,|$)',
            '^(?:PUNCH|PESO)'
        )) {
            $control = Find-Node -Nodes $masterNodes `
                -DescriptionPattern $label -Clickable
            Assert-MinTarget -Node $control
        }
        $masterShot = Save-Screenshot -Name "$($profile.Name)-master"

        $results.Add([pscustomobject]@{
            Scenario = $profile.Name
            WidthDp = $profile.W
            HeightDp = $profile.H
            EnvScreenshot = $envShot
            EqScreenshot = $eqShot
            MasterScreenshot = $masterShot
            HitTargets48Dp = 'PASS'
            RotaryAttackRelease = 'PASS'
            LowMidHigh = 'PASS'
            DPadPicker = if ($pickerValidated) { 'PASS' } else { 'PENDING' }
        })
    }

    $log = Invoke-Adb logcat -d -t 900 | Out-String
    $packageRegex = [regex]::Escape($Package)
    if ($log -match (
        'FATAL EXCEPTION|SIGSEGV|ANR in ' + $packageRegex
    )) {
        throw "Crash/ANR detectado`n$log"
    }
}
finally {
    Invoke-Adb shell wm size reset | Out-Null
    Invoke-Adb shell wm density reset | Out-Null
    Invoke-Adb shell settings put system user_rotation `
        $originalRotation | Out-Null
    Invoke-Adb shell settings put system accelerometer_rotation `
        $originalAutoRotation | Out-Null
    if ($originalFontScale) {
        Invoke-Adb shell settings put system font_scale `
            $originalFontScale | Out-Null
    }
}

$report = Join-Path $OutputDirectory 'report.json'
$results | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $report -Encoding UTF8
Write-Output (
    "ANDROID_TONE_CONTROLS_OK scenarios=$($results.Count) " +
    "pickerDpad=$pickerValidated report=$report"
)
