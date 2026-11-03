# Build de DESENVOLVIMENTO do ChurchDAW no Windows.
#
# Gera um ChurchDAW.exe para testar na propria maquina: sem licenca, sem ASIO
# (se asiosdk/ nao existir) e com o auto-update apontado para um endereco
# invalido -- a policy falha, vira "unavailable" e o gate segue sem nunca
# baixar/instalar a versao publicada por cima deste build.
#
# NUNCA distribua o exe gerado aqui. Release usa Packaging/BuildDesktopRelease.ps1.
#
# Uso:
#   powershell -ExecutionPolicy Bypass -File Tools\BuildWindowsDev.ps1          # configura + compila
#   powershell -ExecutionPolicy Bypass -File Tools\BuildWindowsDev.ps1 -Run     # compila e abre o app
#   powershell -ExecutionPolicy Bypass -File Tools\BuildWindowsDev.ps1 -Clean   # apaga a pasta de build antes
#   powershell -ExecutionPolicy Bypass -File Tools\BuildWindowsDev.ps1 -Install # instala p/ o usuario + atalhos
#
# -Install copia o exe para %LOCALAPPDATA%\Programs\ChurchDAW Teste (sem admin)
# e cria "ChurchDAW (teste)" na Area de Trabalho e no Menu Iniciar. Os dados
# (samples, cifras, ajustes) continuam em %APPDATA%\ChurchDAW, os mesmos de
# sempre. Com -Run, abre a copia instalada.
param(
    [string] $BuildDir = "build-win-dev",
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string] $Config = "Release",
    [string] $Target = "MinhaDAW",
    [switch] $Run,
    [switch] $Clean,
    [switch] $Install
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$buildPath = Join-Path $root $BuildDir

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere nao encontrado: instale o Visual Studio 2022 Build Tools (C++)." }
$vs = & $vswhere -products * -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio/Build Tools com o workload C++ nao encontrado." }

# Importa o ambiente do vcvars64 (cl, link, rc, Windows SDK) para este processo.
# O proprio vcvars chama vswhere.exe e reclama se ele nao estiver no PATH.
$env:PATH = "$(Split-Path -Parent $vswhere);$env:PATH"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

$cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninjaDir = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$env:PATH = "$ninjaDir;$env:PATH"

if ($Clean -and (Test-Path $buildPath)) { Remove-Item -Recurse -Force $buildPath }

if (-not (Test-Path (Join-Path $buildPath "build.ninja"))) {
    & $cmake -S $root -B $buildPath -G Ninja `
        "-DCMAKE_BUILD_TYPE=$Config" `
        -DCHURCHDAW_ENABLE_DESKTOP_LICENSING=OFF `
        -DCHURCHDAW_ALLOW_UNLICENSED_DESKTOP_BUILD=ON `
        -DCHURCHDAW_ALLOW_NO_ASIO=ON `
        "-DCHURCHDAW_UPDATE_POLICY_URL=https://churchdaw.invalid/dev-build-sem-update.json"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure falhou ($LASTEXITCODE)" }
}

$sw = [Diagnostics.Stopwatch]::StartNew()
& $cmake --build $buildPath --target $Target --parallel
if ($LASTEXITCODE -ne 0) { throw "build falhou ($LASTEXITCODE)" }
"Build ok em {0:n0}s" -f $sw.Elapsed.TotalSeconds

$exe = Get-ChildItem -Path (Join-Path $buildPath "${Target}_artefacts") -Filter "*.exe" -Recurse |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($exe) { "Executavel: $($exe.FullName)" }

if ($Install -and $exe) {
    $installDir = Join-Path $env:LOCALAPPDATA "Programs\ChurchDAW Teste"
    $installed = Join-Path $installDir "ChurchDAW.exe"
    # Matar o app liga o MODO SEGURO no proximo boot: se a copia instalada
    # estiver aberta, o musico fecha (ou o script para aqui).
    $running = Get-Process -Name ChurchDAW -ErrorAction SilentlyContinue |
               Where-Object { $_.Path -eq $installed }
    if ($running) { throw "Feche o ChurchDAW (teste) antes de instalar a versao nova." }

    New-Item -ItemType Directory -Force $installDir | Out-Null
    Copy-Item $exe.FullName $installed -Force
    Get-ChildItem $exe.DirectoryName -Filter "*.pdb" | Copy-Item -Destination $installDir -Force

    $shell = New-Object -ComObject WScript.Shell
    foreach ($folder in @([Environment]::GetFolderPath("Desktop"), [Environment]::GetFolderPath("Programs"))) {
        $link = $shell.CreateShortcut((Join-Path $folder "ChurchDAW (teste).lnk"))
        $link.TargetPath = $installed
        $link.WorkingDirectory = $installDir
        $link.IconLocation = "$installed,0"
        $link.Description = "ChurchDAW - build de teste (sem auto-update)"
        $link.Save()
    }
    "Instalado: $installed (atalhos na Area de Trabalho e no Menu Iniciar)"
    $exe = Get-Item $installed
}

if ($Run -and $exe) {
    # Nunca matar: o proximo boot entraria em MODO SEGURO. Aberto, fica aberto.
    if (Get-Process -Name $exe.BaseName -ErrorAction SilentlyContinue) {
        "ChurchDAW ja esta aberto; feche-o para abrir a versao nova."
    } else {
        Start-Process -FilePath $exe.FullName -WorkingDirectory $exe.DirectoryName
    }
}
