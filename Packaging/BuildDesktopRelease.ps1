# Pipeline Windows do ChurchDAW.
#
# Commercial (padrao) e fail-closed:
#   - worktree Git limpo e commit imutavel durante a execucao;
#   - build Visual Studio 2022 x64 em diretorio novo/efemero;
#   - licenciamento, update gate e testes habilitados;
#   - app assinado antes de entrar no RCDATA do instalador;
#   - app, payload extraido e instalador com versao/arquitetura verificadas;
#   - mesmo publisher/thumbprint no app e instalador;
#   - timestamp e `signtool verify /pa /all /tw /v` obrigatorios;
#   - policy assinada, manifest JSON, checksums e logs preservados.
#
# Uma saida unsigned existe somente para desenvolvimento explicito:
#   .\BuildDesktopRelease.ps1 -ReleaseMode Test -AllowUnsignedTestBuild `
#       -SkipExtendedTestsForTest

[CmdletBinding()]
param(
    [ValidateSet('Commercial', 'Test')]
    [string] $ReleaseMode = 'Commercial',
    [switch] $AllowUnsignedTestBuild,
    [switch] $SkipTimestampForTest,
    [switch] $SkipExtendedTestsForTest,

    [int] $PolicySerial = 0,
    [int] $MinimumVersionCode = 0,
    [string] $UpdateMessage = '',
    [string] $UpdatePolicyPrivateKey = '',
    [string] $UpdatePolicyPublicKey = '',
    # O key_id do envelope PRECISA parear com a chave privada usada: o
    # default do assinador (churchdaw-update-2026-08) nao casa com a
    # private 2026-01 do ChurchDAW-Secrets e faz o gate de update
    # reprovar (assinatura invalida) e abandona clientes antigos.
    [string] $UpdatePolicyKeyId = 'churchdaw-update-2026-01',
    [string] $PreviousPolicy = '',

    [string] $PfxPath = '',
    [string] $PfxPassword = '',
    [string] $Thumbprint = '',
    [string] $SubjectName = '',
    [switch] $MachineStore,
    [string] $ExpectedPublisher = '',
    [Alias('ExpectedThumbprint')]
    [string[]] $ExpectedSignerThumbprints = @(),
    [string] $TimestampUrl = 'http://timestamp.digicert.com',

    [string] $BuildDir = '',
    [string] $OutputDir = '',
    [switch] $KeepBuildDirectory,
    [ValidateRange(5, 1024)]
    [int] $MinimumFreeSpaceGiB = 20,
    [ValidateRange(1, 100)]
    [int] $MinimumOutputFreeSpaceGiB = 2,

    [string] $CMakeGenerator = 'Visual Studio 17 2022',
    [string] $CMakeToolset = 'v143',
    [ValidateRange(1, 128)]
    [int] $Parallel = 1
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'WindowsRelease.Common.ps1')

function Invoke-ReleaseCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,
        [Parameter(Mandatory = $true)]
        [string[]] $Arguments,
        [Parameter(Mandatory = $true)]
        [string] $LogPath,
        [Parameter(Mandatory = $true)]
        [string] $Label
    )

    $result = Invoke-ChurchDAWNativeCapture -FilePath $FilePath `
        -Arguments $Arguments -LogPath $LogPath -Label $Label
    if ($result.ExitCode -ne 0) {
        throw "$Label falhou (codigo $($result.ExitCode))"
    }
    return $result
}

function Get-CMakeCacheMap {
    param([Parameter(Mandatory = $true)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "CMakeCache.txt nao encontrado: $Path"
    }

    $map = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([^#/][^:=]*):[^=]+=(.*)$') {
            $map[$matches[1]] = $matches[2]
        }
    }
    return $map
}

function Assert-CacheValue {
    param(
        [Parameter(Mandatory = $true)] $Cache,
        [Parameter(Mandatory = $true)] [string] $Name,
        [Parameter(Mandatory = $true)] [string] $Expected
    )

    if (-not $Cache.ContainsKey($Name)) {
        throw "Entrada ausente no CMakeCache: $Name"
    }
    if (-not ([string] $Cache[$Name]).Equals(
        $Expected,
        [StringComparison]::Ordinal)) {
        throw ("CMakeCache inseguro: $Name='$($Cache[$Name])'; " +
               "esperado='$Expected'")
    }
}

function Resolve-UniqueBuildFile {
    param(
        [Parameter(Mandatory = $true)] [string] $Root,
        [Parameter(Mandatory = $true)] [string] $Name
    )

    $found = @(Get-ChildItem -LiteralPath $Root -Recurse -File `
        -Filter $Name -ErrorAction Stop)
    if ($found.Count -ne 1) {
        throw ("Esperado exatamente um '$Name' em '$Root'; encontrados: " +
               $found.Count)
    }
    return $found[0].FullName
}

function Copy-FileAndVerify {
    param(
        [Parameter(Mandatory = $true)] [string] $Source,
        [Parameter(Mandatory = $true)] [string] $Destination
    )

    $sourceFull = [IO.Path]::GetFullPath($Source)
    $destinationFull = [IO.Path]::GetFullPath($Destination)
    Copy-Item -LiteralPath $sourceFull -Destination $destinationFull
    $sourceHash = (Get-FileHash -LiteralPath $sourceFull `
        -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $destinationFull `
        -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw "Copia divergente: $destinationFull"
    }
}

function Write-Utf8Json {
    param(
        [Parameter(Mandatory = $true)] $Value,
        [Parameter(Mandatory = $true)] [string] $Path
    )

    $json = $Value | ConvertTo-Json -Depth 20
    [IO.File]::WriteAllText(
        [IO.Path]::GetFullPath($Path),
        $json + [Environment]::NewLine,
        [Text.UTF8Encoding]::new($false))
}

function Assert-FreeSpace {
    param(
        [Parameter(Mandatory = $true)] [string] $Path,
        [Parameter(Mandatory = $true)] [int] $RequiredGiB
    )

    $root = [IO.Path]::GetPathRoot([IO.Path]::GetFullPath($Path))
    $drive = [IO.DriveInfo]::new($root)
    $requiredBytes = [long] $RequiredGiB * 1GB
    if (-not $drive.IsReady -or
        $drive.AvailableFreeSpace -lt $requiredBytes) {
        $available = if ($drive.IsReady) {
            [Math]::Round($drive.AvailableFreeSpace / 1GB, 2)
        } else { 0 }
        throw ("Espaco insuficiente em ${root}: $available GiB livres; " +
               "necessarios ao menos $RequiredGiB GiB")
    }
}

function Test-PathIsInside {
    param(
        [Parameter(Mandatory = $true)] [string] $Child,
        [Parameter(Mandatory = $true)] [string] $Parent
    )

    $childFull = [IO.Path]::GetFullPath($Child).TrimEnd('\') + '\'
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    return $childFull.StartsWith(
        $parentFull,
        [StringComparison]::OrdinalIgnoreCase)
}

if (-not (Test-ChurchDAWIsWindows)) {
    throw 'O release Windows precisa rodar no Windows'
}

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$cmakeLists = Join-Path $repo 'CMakeLists.txt'
$versionMatches = [regex]::Matches(
    [IO.File]::ReadAllText($cmakeLists),
    '(?m)^\s*project\(MinhaDAW\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s*\)')
if ($versionMatches.Count -ne 1) {
    throw 'Nao foi possivel resolver uma unica versao em CMakeLists.txt'
}
$version = $versionMatches[0].Groups[1].Value
$versionParts = $version.Split('.')
$versionCode =
    [int] $versionParts[0] * 10000 +
    [int] $versionParts[1] * 100 +
    [int] $versionParts[2]

# PISO HISTORICO: o CMakeLists pode elevar o version code acima da formula
# (ex.: 1.6.3 publicada como 10610). O updater compara CODIGOS, entao a
# politica precisa usar o MESMO code que o binario compilado. Le o piso do
# CMakeLists para nunca divergir.
$cmakeText = [IO.File]::ReadAllText($cmakeLists)
$floorMatches = [regex]::Matches(
    $cmakeText,
    '(?m)^\s*set\(CHURCHDAW_VERSION_CODE_FLOOR\s+([0-9]+)\s*\)')
if ($floorMatches.Count -ge 1) {
    $floor = [int] $floorMatches[0].Groups[1].Value
    if ($floor -gt $versionCode) {
        $versionCode = $floor
    }
}

$hasSigningIdentity =
    -not [string]::IsNullOrWhiteSpace($PfxPath) -or
    -not [string]::IsNullOrWhiteSpace($Thumbprint) -or
    -not [string]::IsNullOrWhiteSpace($SubjectName)
$signingPolicy = Assert-ChurchDAWWindowsSigningPolicy `
    -ReleaseMode $ReleaseMode `
    -HasSigningIdentity $hasSigningIdentity `
    -AllowUnsignedTestBuild ([bool] $AllowUnsignedTestBuild) `
    -SkipTimestamp ([bool] $SkipTimestampForTest) `
    -ExpectedPublisher $ExpectedPublisher `
    -ExpectedSignerThumbprints $ExpectedSignerThumbprints

if ($ReleaseMode -eq 'Commercial' -and $SkipExtendedTestsForTest) {
    throw '-SkipExtendedTestsForTest existe somente para -ReleaseMode Test'
}
if ($ReleaseMode -eq 'Commercial' -and $PolicySerial -lt 1) {
    throw 'Release comercial exige -PolicySerial positivo'
}
if ($PolicySerial -gt 0 -and $MinimumVersionCode -lt 1) {
    throw '-PolicySerial exige -MinimumVersionCode positivo'
}
if ($MinimumVersionCode -gt $versionCode) {
    throw ("MinimumVersionCode $MinimumVersionCode e maior que a versao " +
           "compilada $versionCode")
}
if ($PolicySerial -gt 1 -and
    $ReleaseMode -eq 'Commercial' -and
    [string]::IsNullOrWhiteSpace($PreviousPolicy)) {
    throw ('PolicySerial maior que 1 exige -PreviousPolicy para provar ' +
           'monotonicidade')
}

$needsUpdatePrivateKey =
    $PolicySerial -gt 0 -or -not $SkipExtendedTestsForTest
if ([string]::IsNullOrWhiteSpace($UpdatePolicyPrivateKey)) {
    $UpdatePolicyPrivateKey = Join-Path $env:USERPROFILE `
        'ChurchDAW-Secrets\churchdaw-update-policy-private.pem'
}
if ($needsUpdatePrivateKey -and
    -not (Test-Path -LiteralPath $UpdatePolicyPrivateKey -PathType Leaf)) {
    throw ("Os testes/policy de update exigem a chave privada oficial: " +
           $UpdatePolicyPrivateKey)
}
if (-not [string]::IsNullOrWhiteSpace($PreviousPolicy) -and
    -not (Test-Path -LiteralPath $PreviousPolicy -PathType Leaf)) {
    throw "PreviousPolicy nao encontrada: $PreviousPolicy"
}

$gitStart = Get-ChurchDAWGitState $repo
if ($ReleaseMode -eq 'Commercial' -and -not $gitStart.IsClean) {
    throw ('Release comercial exige worktree Git limpo. Pendencias: ' +
           (@($gitStart.Status) -join '; '))
}

$generatedBuildDir = [string]::IsNullOrWhiteSpace($BuildDir)
$buildId = [Guid]::NewGuid().ToString('N')
if ($generatedBuildDir) {
    $BuildDir = Join-Path ([IO.Path]::GetTempPath()) `
        "churchdaw-windows-release-$buildId"
}
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if ($ReleaseMode -eq 'Commercial' -and
    (Test-PathIsInside -Child $BuildDir -Parent $repo)) {
    throw 'BuildDir comercial precisa ficar fora do worktree'
}
$utcStamp = [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssZ')
$shortCommit = $gitStart.Commit.Substring(0, 12)
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repo `
        "Releases\Windows\ChurchDAW-$version-$utcStamp-$shortCommit"
}
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if (Test-Path -LiteralPath $OutputDir) {
    throw "OutputDir ja existe; nenhum overwrite e permitido: $OutputDir"
}
if (Test-PathIsInside -Child $OutputDir -Parent $BuildDir) {
    throw 'OutputDir nao pode ficar dentro do BuildDir efemero'
}
Assert-FreeSpace -Path $OutputDir `
    -RequiredGiB $MinimumOutputFreeSpaceGiB

Assert-FreeSpace -Path $BuildDir -RequiredGiB $MinimumFreeSpaceGiB
if (Test-Path -LiteralPath $BuildDir) {
    if (@(Get-ChildItem -LiteralPath $BuildDir -Force).Count -gt 0) {
        throw "BuildDir precisa ser novo/vazio: $BuildDir"
    }
}
else {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

$ephemeralMarker = Join-Path $BuildDir '.churchdaw-ephemeral-build'
if ($generatedBuildDir) {
    [IO.File]::WriteAllText(
        $ephemeralMarker,
        $buildId,
        [Text.UTF8Encoding]::new($false))
}

$stagingDir = Join-Path $BuildDir 'release-staging'
$logsDir = Join-Path $BuildDir 'release-logs'
New-Item -ItemType Directory -Path $stagingDir | Out-Null
New-Item -ItemType Directory -Path $logsDir | Out-Null

$cmake = (Get-Command cmake.exe -CommandType Application `
    -ErrorAction Stop).Source
$ctest = (Get-Command ctest.exe -CommandType Application `
    -ErrorAction Stop).Source
$cmakeVersion = (@(& $cmake --version) | Select-Object -First 1).Trim()
$ctestVersion = (@(& $ctest --version) | Select-Object -First 1).Trim()

$audioTestsEnabled = -not $SkipExtendedTestsForTest
$configureArguments = @(
    '-S', $repo,
    '-B', $BuildDir,
    '-G', $CMakeGenerator,
    '-A', 'x64',
    '-T', $CMakeToolset,
    '-DCHURCHDAW_ENABLE_DESKTOP_LICENSING=ON',
    '-DCHURCHDAW_BUILD_LICENSE_TESTS=ON',
    "-DCHURCHDAW_BUILD_AUDIO_TESTS=$(
        if ($audioTestsEnabled) { 'ON' } else { 'OFF' })",
    "-DCHURCHDAW_BUILD_UPDATE_TESTS=$(
        if ($audioTestsEnabled) { 'ON' } else { 'OFF' })",
    '-DCHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST=OFF',
    '-DCHURCHDAW_LICENSE_API_BASE=https://churchdaw.com/api/v1',
    '-DCHURCHDAW_BUILD_INSTALLER=ON',
    '-DCHURCHDAW_UPDATE_ALLOW_LOCALHOST=OFF',
    "-DCHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE=$(
        if ($ReleaseMode -eq 'Commercial') { 'ON' } else { 'OFF' })",
    '-DCHURCHDAW_UPDATE_POLICY_URL=https://churchdaw.com/app/desktop/update-policy-v1.json'
)

Write-Host '== configure limpo Visual Studio 2022 x64 ==' `
    -ForegroundColor Cyan
$null = Invoke-ReleaseCommand -FilePath $cmake `
    -Arguments $configureArguments `
    -LogPath (Join-Path $logsDir '01-configure.log') `
    -Label 'cmake configure'

$cache = Get-CMakeCacheMap (Join-Path $BuildDir 'CMakeCache.txt')
$requiredCache = [ordered] @{
    CHURCHDAW_ENABLE_DESKTOP_LICENSING = 'ON'
    CHURCHDAW_BUILD_LICENSE_TESTS = 'ON'
    CHURCHDAW_BUILD_AUDIO_TESTS =
        $(if ($audioTestsEnabled) { 'ON' } else { 'OFF' })
    CHURCHDAW_BUILD_UPDATE_TESTS =
        $(if ($audioTestsEnabled) { 'ON' } else { 'OFF' })
    CHURCHDAW_LICENSE_ALLOW_HTTP_LOCALHOST = 'OFF'
    CHURCHDAW_LICENSE_API_BASE = 'https://churchdaw.com/api/v1'
    CHURCHDAW_BUILD_INSTALLER = 'ON'
    CHURCHDAW_UPDATE_ALLOW_LOCALHOST = 'OFF'
    CHURCHDAW_UPDATE_REQUIRE_OS_SIGNATURE =
        $(if ($ReleaseMode -eq 'Commercial') { 'ON' } else { 'OFF' })
    CHURCHDAW_UPDATE_POLICY_URL =
        'https://churchdaw.com/app/desktop/update-policy-v1.json'
    CMAKE_GENERATOR = $CMakeGenerator
    CMAKE_GENERATOR_PLATFORM = 'x64'
    CMAKE_GENERATOR_TOOLSET = $CMakeToolset
}
foreach ($entry in $requiredCache.GetEnumerator()) {
    Assert-CacheValue -Cache $cache -Name $entry.Key -Expected $entry.Value
}

Write-Host '== teste do verificador de licenca ==' -ForegroundColor Cyan
$null = Invoke-ReleaseCommand -FilePath $cmake `
    -Arguments @(
        '--build', $BuildDir, '--config', 'Release',
        '--target', 'ChurchDAWLicenseVerifierTests',
        '--parallel', "$Parallel") `
    -LogPath (Join-Path $logsDir '02-build-license-tests.log') `
    -Label 'build license verifier tests'
$licenseVerifier = Resolve-UniqueBuildFile -Root $BuildDir `
    -Name 'ChurchDAWLicenseVerifierTests.exe'
$fixture = Join-Path $repo `
    'Tests\DesktopLicensing\lease-fixture-ci.token'
$null = Invoke-ReleaseCommand -FilePath $licenseVerifier `
    -Arguments @($fixture, 'fixture-installation-id', '1800000000') `
    -LogPath (Join-Path $logsDir '03-license-tests.log') `
    -Label 'license verifier tests'

if ($audioTestsEnabled) {
    Write-Host '== testes de audio/update/ChurchCam (sem app comercial) ==' `
        -ForegroundColor Cyan
    $null = Invoke-ReleaseCommand -FilePath $cmake `
        -Arguments @(
            '--build', $BuildDir, '--config', 'Release',
            '--target',
            'ChurchDAWReverbPitchTests',
            'ChurchDAWMasterLimiterTests',
            'ChurchDAWChannelToneTests',
            'ChurchDAWNordSampleToneTests',
            'ChurchDAWMidiMappingSafetyTests',
            'ChurchDAWChannelRotaryTests',
            'ChurchDAWPadProcessorTests',
            'ChurchDAWMobileBufferPolicyTests',
            'ChurchDAWPadSynthProcessorTests',
            'ChurchDAWPadSynthRenderProbe',
            'ChurchDAWLibraryPackPathTests',
            'ChurchDAWTimbreDownloadTests',
            'ChurchDAWLiveScenesTests',
            'ChurchDAWSceneFileWriteProbe',
            'ChurchDAWPluginScanPolicyTests',
            'ChurchDAWAndroidStageLayoutTests',
            'ChurchDAWTimbreLoadStressTests',
            'ChurchDAWHostedInstrumentMidiRoutingTests',
            'ChurchDAWMasterRouteAdversarialProbe',
            'ChurchDAWZipDeleteProbe',
            'ChurchDAWReverbBoundsProbe',
            'ChurchDAWUpdatePolicyTests',
            # O ctest abaixo roda TODOS os testes registrados. Portanto cada
            # executavel/fixture precisa constar nesta lista; caso contrario o
            # teste aparece como "Not Run" apesar de o codigo compilar em outro
            # build local.
            'ChurchDAWChildProcessReapTests',
            'ChurchDAWPluginScanIntegrationTests',
            'ChurchDAWVST3MultiClassCrashFixture',
            'ChurchDAWSabrinaTests',
            'ChurchDAWMixerStripCollapseTests',
            'ChurchDAWEngineRealtimeTests',
            'ChurchDAWStudioPreview',
            '--parallel', "$Parallel") `
        -LogPath (Join-Path $logsDir '04-build-extended-tests.log') `
        -Label 'build release extended tests'
    $null = Invoke-ReleaseCommand -FilePath $ctest `
        -Arguments @(
            '--test-dir', $BuildDir,
            '-C', 'Release',
            '--output-on-failure',
            '--no-tests=error') `
        -LogPath (Join-Path $logsDir '05-audio-tests-release.log') `
        -Label 'complete release tests'

    # O StudioPreview nao esta registrado no ctest. Seus modos abaixo usam
    # quadros e audio sinteticos, sem abrir camera, microfone ou sessao do app.
    # NativeCapture mantem o executavel GUI em um pipeline, aguardando sua
    # saida e preservando stdout/stderr e codigo de retorno no log.
    $studioPreview = Resolve-UniqueBuildFile -Root $BuildDir `
        -Name 'ChurchDAW Studio Preview.exe'
    $null = Invoke-ReleaseCommand -FilePath $studioPreview `
        -Arguments @('--self-test') `
        -LogPath (Join-Path $logsDir '05b-churchcam-self-tests.log') `
        -Label 'ChurchCam self tests'
    $null = Invoke-ReleaseCommand -FilePath $studioPreview `
        -Arguments @(
            '--movie-writer-contract',
            (Join-Path $BuildDir 'churchcam-contract')) `
        -LogPath (Join-Path $logsDir '05c-churchcam-movie-writer-contract.log') `
        -Label 'ChurchCam native movie writer contract'
    $null = Invoke-ReleaseCommand -FilePath $studioPreview `
        -Arguments @(
            '--record-test',
            (Join-Path $BuildDir 'churchcam-record-test-720p.mp4')) `
        -LogPath (Join-Path $logsDir '05d-churchcam-recorder-720p.log') `
        -Label 'ChurchCam Recorder 720p'
    $null = Invoke-ReleaseCommand -FilePath $studioPreview `
        -Arguments @(
            '--record-test-fullhd',
            (Join-Path $BuildDir 'churchcam-record-test-1080p.mp4')) `
        -LogPath (Join-Path $logsDir '05e-churchcam-recorder-1080p.log') `
        -Label 'ChurchCam Recorder 1080p'

    $null = Invoke-ReleaseCommand -FilePath $cmake `
        -Arguments @(
            '--build', $BuildDir, '--config', 'RelWithDebInfo',
            '--target', 'ChurchDAWReverbBoundsProbe',
            '--parallel', "$Parallel") `
        -LogPath (Join-Path $logsDir '06-build-reverb-asan.log') `
        -Label 'build reverb bounds ASan'

    # RUNTIME DO ASan NO PATH.
    #
    # O probe linka com /fsanitize=address dinamico, entao precisa achar
    # clang_rt.asan_dynamic-x86_64.dll em tempo de execucao. Ela mora no bin do
    # toolset MSVC e NAO esta no PATH de um shell comum: sem isto o ctest morre
    # com 0xc0000135 (DLL nao encontrada) e o release inteiro aborta -- sem
    # nenhum defeito no reverb, que passa normalmente na suite Release.
    $asanRuntimeDir = $null
    $asanDllName = 'clang_rt.asan_dynamic-x86_64.dll'

    # 1) o toolset ativo, quando o script roda dentro de um Developer Prompt
    if (-not [string]::IsNullOrWhiteSpace($env:VCToolsInstallDir)) {
        $candidate = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64'
        if (Test-Path (Join-Path $candidate $asanDllName)) {
            $asanRuntimeDir = (Resolve-Path $candidate).Path
        }
    }
    # 2) varredura das instalacoes do Visual Studio (inclui Build Tools, que
    #    ficam em "Program Files (x86)\...\2022\BuildTools\VC\Tools\MSVC\<ver>")
    if (-not $asanRuntimeDir) {
        foreach ($root in @(
                "${env:ProgramFiles(x86)}\Microsoft Visual Studio",
                "$env:ProgramFiles\Microsoft Visual Studio")) {
            if (-not (Test-Path $root)) { continue }
            $found = Get-ChildItem $root -Recurse -Filter $asanDllName -File `
                -ErrorAction SilentlyContinue |
                Where-Object { $_.DirectoryName -like '*Hostx64\x64' } |
                Sort-Object FullName -Descending | Select-Object -First 1
            if ($found) { $asanRuntimeDir = $found.DirectoryName; break }
        }
    }
    if ($asanRuntimeDir) {
        Write-Host "== runtime ASan: $asanRuntimeDir ==" -ForegroundColor Cyan
        $env:PATH = "$asanRuntimeDir;$env:PATH"
    } else {
        throw ('Runtime do AddressSanitizer nao encontrada ' +
               '(clang_rt.asan_dynamic-x86_64.dll). Instale o componente ' +
               '"C++ AddressSanitizer" no Visual Studio Installer.')
    }

    $null = Invoke-ReleaseCommand -FilePath $ctest `
        -Arguments @(
            '--test-dir', $BuildDir,
            '-C', 'RelWithDebInfo',
            '--output-on-failure',
            '--no-tests=error',
            '-R', '^ChurchDAWReverbBounds$') `
        -LogPath (Join-Path $logsDir '07-reverb-asan.log') `
        -Label 'reverb bounds ASan'
}

Write-Host '== build do aplicativo comercial ==' -ForegroundColor Cyan
$null = Invoke-ReleaseCommand -FilePath $cmake `
    -Arguments @(
        '--build', $BuildDir, '--config', 'Release',
        '--target', 'MinhaDAW',
        '--parallel', "$Parallel") `
    -LogPath (Join-Path $logsDir '08-build-app.log') `
    -Label 'build ChurchDAW app'

$appBinary = Join-Path $BuildDir `
    'MinhaDAW_artefacts\Release\ChurchDAW.exe'
if (-not (Test-Path -LiteralPath $appBinary -PathType Leaf)) {
    throw "ChurchDAW.exe nao encontrado: $appBinary"
}
$appPe = Assert-ChurchDAWPeReleaseIdentity -Path $appBinary `
    -ExpectedVersion $version -ExpectedProductName 'ChurchDAW'

$appAscii = [Text.Encoding]::ASCII.GetString(
    [IO.File]::ReadAllBytes($appBinary))
foreach ($marker in @(
    'CHURCHDAW_RELEASE_LICENSE_GATE=DESKTOP_ENABLED',
    'https://churchdaw.com/api/v1',
    '/auth/login',
    'churchdaw-lease+jwt',
    'Entrar no ChurchDAW',
    'https://churchdaw.com/app/desktop/update-policy-v1.json'
)) {
    if (-not $appAscii.Contains($marker)) {
        throw "Gate comercial ausente do executavel (marcador: $marker)"
    }
}

$signScript = Join-Path $PSScriptRoot 'SignDesktopInstaller.ps1'
$signArguments = @{
    ReleaseMode = $ReleaseMode
    Installer = $appBinary
    ExpectedPublisher = $signingPolicy.ExpectedPublisher
    ExpectedSignerThumbprints =
        $signingPolicy.ExpectedSignerThumbprints
    TimestampUrl = $TimestampUrl
    VerificationLogPath =
        (Join-Path $logsDir '09-app-authenticode.log')
}
if (-not [string]::IsNullOrWhiteSpace($PfxPath)) {
    $signArguments.PfxPath = $PfxPath
    $signArguments.PfxPassword = $PfxPassword
}
elseif (-not [string]::IsNullOrWhiteSpace($Thumbprint)) {
    $signArguments.Thumbprint = $Thumbprint
}
elseif (-not [string]::IsNullOrWhiteSpace($SubjectName)) {
    $signArguments.SubjectName = $SubjectName
}
if ($MachineStore) { $signArguments.MachineStore = $true }
if ($SkipTimestampForTest) { $signArguments.SkipTimestamp = $true }

$appSignature = $null
if ($hasSigningIdentity) {
    Write-Host '== assinatura Authenticode do app ==' -ForegroundColor Cyan
    $appSignature = & $signScript @signArguments
}
else {
    $unsignedApp = Get-AuthenticodeSignature -LiteralPath $appBinary
    if ($unsignedApp.Status -ne 'NotSigned') {
        throw 'Build de teste unsigned produziu app inesperadamente assinado'
    }
    Write-Warning 'App unsigned: artefato marcado como TEST ONLY.'
}
$appSha = (Get-FileHash -LiteralPath $appBinary `
    -Algorithm SHA256).Hash.ToLowerInvariant()

if ($audioTestsEnabled) {
    $fixtureDirectory = Join-Path $BuildDir 'update-policy-fixtures'
    New-Item -ItemType Directory -Path $fixtureDirectory | Out-Null
    $fixture1 = Join-Path $fixtureDirectory 'policy-serial-1.json'
    $fixture2 = Join-Path $fixtureDirectory 'policy-serial-2.json'
    $fixtureUrl =
        "https://churchdaw.com/app/desktop/$([IO.Path]::GetFileName($appBinary))"
    $fixtureCommon = @{
        LatestVersionCode = 10400
        MinimumVersionCode = 10300
        LatestVersionName = '1.4.0'
        UpdateMessage = 'Fixture automatizado do gate desktop.'
        WindowsInstaller = $appBinary
        WindowsInstallerUrl = $fixtureUrl
        PrivateKey = $UpdatePolicyPrivateKey
        KeyId = $UpdatePolicyKeyId
        ReleaseMode = $ReleaseMode
        ExpectedWindowsPublisher = $signingPolicy.ExpectedPublisher
        ExpectedWindowsSignerThumbprints =
            $signingPolicy.ExpectedSignerThumbprints
        WindowsVerificationLogPath =
            (Join-Path $logsDir '10-update-fixture-authenticode.log')
    }
    if (-not [string]::IsNullOrWhiteSpace($UpdatePolicyPublicKey)) {
        # A chave publica de conferencia PRECISA parear com a privada usada;
        # sem isto o assinador cai no default do ChurchDAW-Secrets (chave
        # antiga) e recusa assinaturas feitas com a rotacao 2026-08.
        $fixtureCommon.PublicKey = $UpdatePolicyPublicKey
    }
    if (-not $hasSigningIdentity) {
        $fixtureCommon.AllowUnsignedWindowsInstallerForTest = $true
    }

    $fixture1Arguments = @{} + $fixtureCommon
    $fixture1Arguments.PolicySerial = 1
    $fixture1Arguments.Output = $fixture1
    $null = & (Join-Path $PSScriptRoot `
        'SignDesktopUpdatePolicy.ps1') @fixture1Arguments

    $fixture2Arguments = @{} + $fixtureCommon
    $fixture2Arguments.PolicySerial = 2
    $fixture2Arguments.LatestVersionCode = 10500
    $fixture2Arguments.LatestVersionName = '1.5.0'
    $fixture2Arguments.PreviousPolicy = $fixture1
    $fixture2Arguments.Output = $fixture2
    $null = & (Join-Path $PSScriptRoot `
        'SignDesktopUpdatePolicy.ps1') @fixture2Arguments

    $updateTests = Resolve-UniqueBuildFile -Root $BuildDir `
        -Name 'ChurchDAWUpdatePolicyTests.exe'
    $null = Invoke-ReleaseCommand -FilePath $updateTests `
        -Arguments @($repo, $fixture1, $fixture2) `
        -LogPath (Join-Path $logsDir '11-update-policy-tests.log') `
        -Label 'desktop update policy tests'
}

Write-Host '== build do instalador com app ja assinado ==' `
    -ForegroundColor Cyan
$null = Invoke-ReleaseCommand -FilePath $cmake `
    -Arguments @(
        '--build', $BuildDir, '--config', 'Release',
        '--target', 'ChurchDAWSetup',
        '--parallel', "$Parallel") `
    -LogPath (Join-Path $logsDir '12-build-installer.log') `
    -Label 'build ChurchDAW installer'

$intermediatePayload = Join-Path $BuildDir `
    'installer-payload\ChurchDAW.exe'
if (-not (Test-Path -LiteralPath $intermediatePayload -PathType Leaf)) {
    throw "Payload intermediario ausente: $intermediatePayload"
}
$intermediateSha = (Get-FileHash -LiteralPath $intermediatePayload `
    -Algorithm SHA256).Hash.ToLowerInvariant()
if ($intermediateSha -ne $appSha) {
    throw 'Payload intermediario diverge do app auditado/assinado'
}
$null = Assert-ChurchDAWPeReleaseIdentity -Path $intermediatePayload `
    -ExpectedVersion $version -ExpectedProductName 'ChurchDAW'

$installer = Join-Path $BuildDir `
    "Release\ChurchDAW-$version-Setup.exe"
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
    throw "Instalador nao encontrado: $installer"
}
$installerPe = Assert-ChurchDAWPeReleaseIdentity -Path $installer `
    -ExpectedVersion $version -ExpectedProductName 'ChurchDAW'
if (-not $installerPe.OriginalFilename.Equals(
    "ChurchDAW-$version-Setup.exe",
    [StringComparison]::Ordinal)) {
    throw ("OriginalFilename do instalador diverge: " +
           $installerPe.OriginalFilename)
}

$installerSignature = $null
if ($hasSigningIdentity) {
    Write-Host '== assinatura Authenticode do instalador ==' `
        -ForegroundColor Cyan
    $installerSignArguments = @{} + $signArguments
    $installerSignArguments.Installer = $installer
    $installerSignArguments.VerificationLogPath =
        (Join-Path $logsDir '13-installer-authenticode.log')
    $installerSignature = & $signScript @installerSignArguments
    Assert-ChurchDAWSameSigner -First $appSignature `
        -Second $installerSignature `
        -FirstLabel 'ChurchDAW.exe' `
        -SecondLabel "ChurchDAW-$version-Setup.exe"
}
else {
    $unsignedInstaller = Get-AuthenticodeSignature -LiteralPath $installer
    if ($unsignedInstaller.Status -ne 'NotSigned') {
        throw ('Build de teste unsigned produziu instalador ' +
               'inesperadamente assinado')
    }
    Write-Warning 'Instalador unsigned: artefato marcado como TEST ONLY.'
}

$extractedPayload = Join-Path $BuildDir `
    'installer-payload-extracted\ChurchDAW.exe'
$extraction = Export-ChurchDAWInstallerPayload `
    -Installer $installer -Output $extractedPayload -ResourceId 1
if ($extraction.Sha256 -ne $appSha) {
    throw ('RCDATA extraido diverge do app auditado: ' +
           "$($extraction.Sha256) != $appSha")
}
$null = Assert-ChurchDAWPeReleaseIdentity -Path $extractedPayload `
    -ExpectedVersion $version -ExpectedProductName 'ChurchDAW'
if ($hasSigningIdentity) {
    $extractedSignature = Assert-ChurchDAWAuthenticodeSignature `
        -Path $extractedPayload `
        -ExpectedPublisher $signingPolicy.ExpectedPublisher `
        -ExpectedSignerThumbprints $signingPolicy.ExpectedSignerThumbprints `
        -RequireTimestamp $signingPolicy.RequireTimestamp `
        -RequirePublicChain $signingPolicy.RequirePublicChain `
        -VerificationLogPath (Join-Path $logsDir `
            '14-extracted-payload-authenticode.log')
    Assert-ChurchDAWSameSigner -First $appSignature `
        -Second $extractedSignature `
        -FirstLabel 'ChurchDAW.exe' `
        -SecondLabel 'RCDATA extraido'
}

$stagedApp = Join-Path $stagingDir 'ChurchDAW.exe'
$stagedInstaller = Join-Path $stagingDir `
    "ChurchDAW-$version-Setup.exe"
$stagedPayload = Join-Path $stagingDir `
    'ChurchDAW-installer-payload.extracted.exe'
Copy-FileAndVerify $appBinary $stagedApp
Copy-FileAndVerify $installer $stagedInstaller
Copy-FileAndVerify $extractedPayload $stagedPayload

$stagedLogs = Join-Path $stagingDir 'logs'
Copy-Item -LiteralPath $logsDir -Destination $stagedLogs -Recurse

$policyPath = ''
$policyResult = $null
if ($PolicySerial -gt 0) {
    if ([string]::IsNullOrWhiteSpace($UpdateMessage)) {
        $UpdateMessage = "Nova versao $version do ChurchDAW."
    }

    $policyPath = Join-Path $stagingDir 'update-policy-v1.json'
    $policyArguments = @{
        PolicySerial = $PolicySerial
        LatestVersionCode = $versionCode
        MinimumVersionCode = $MinimumVersionCode
        LatestVersionName = $version
        UpdateMessage = $UpdateMessage
        WindowsInstaller = $stagedInstaller
        WindowsInstallerUrl =
            "https://churchdaw.com/app/desktop/$(
                [IO.Path]::GetFileName($stagedInstaller))"
        PrivateKey = $UpdatePolicyPrivateKey
        KeyId = $UpdatePolicyKeyId
        PreviousPolicy = $PreviousPolicy
        Output = $policyPath
        ReleaseMode = $ReleaseMode
        ExpectedWindowsPublisher = $signingPolicy.ExpectedPublisher
        ExpectedWindowsSignerThumbprints =
            $signingPolicy.ExpectedSignerThumbprints
        WindowsVerificationLogPath =
            (Join-Path $stagedLogs '15-policy-installer-authenticode.log')
    }
    if (-not $hasSigningIdentity) {
        $policyArguments.AllowUnsignedWindowsInstallerForTest = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($UpdatePolicyPublicKey)) {
        $policyArguments.PublicKey = $UpdatePolicyPublicKey
    }
    $policyResult = & (Join-Path $PSScriptRoot `
        'SignDesktopUpdatePolicy.ps1') @policyArguments
}

$gitEnd = Get-ChurchDAWGitState $repo
if ($ReleaseMode -eq 'Commercial') {
    if (-not $gitEnd.IsClean -or $gitEnd.Commit -ne $gitStart.Commit) {
        throw ('Fontes mudaram durante o release; artefatos recusados. ' +
               "inicio=$($gitStart.Commit), fim=$($gitEnd.Commit)")
    }
}

$artifactRecords = @()
$artifactRecords += Get-ChurchDAWFileRecord $stagedApp 'windows_app'
$artifactRecords += Get-ChurchDAWFileRecord `
    $stagedInstaller 'windows_installer'
$artifactRecords += Get-ChurchDAWFileRecord `
    $stagedPayload 'installer_payload_extracted'
if (-not [string]::IsNullOrWhiteSpace($policyPath)) {
    $artifactRecords += Get-ChurchDAWFileRecord `
        $policyPath 'desktop_update_policy'
}

$manifestPath = Join-Path $stagingDir 'release-manifest.json'
$manifest = [ordered] @{
    schema_version = 1
    product_id = 'com.churchdaw.app'
    product_name = 'ChurchDAW'
    platform = 'windows'
    architecture = 'x64'
    release_mode = $ReleaseMode.ToLowerInvariant()
    distribution_approved = $ReleaseMode -eq 'Commercial'
    version_name = $version
    version_code = $versionCode
    generated_at_utc = [DateTimeOffset]::UtcNow.ToString('o')
    source = [ordered] @{
        git_commit = $gitStart.Commit
        git_branch = $gitStart.Branch
        clean_at_start = $gitStart.IsClean
        clean_at_end = $gitEnd.IsClean
        dirty_entries_at_start = @($gitStart.Status)
    }
    toolchain = [ordered] @{
        cmake = $cmakeVersion
        ctest = $ctestVersion
        generator = $CMakeGenerator
        toolset = $CMakeToolset
        configuration = 'Release'
        architecture = 'x64'
        parallel = $Parallel
    }
    gates = [ordered] @{
        licensing_enabled = $true
        http_localhost_disabled = $true
        update_localhost_disabled = $true
        license_tests = 'passed'
        audio_tests = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        churchcam_self_tests = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        churchcam_movie_writer_contract = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        churchcam_recorder_720p = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        churchcam_recorder_1080p = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        reverb_bounds_asan = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        update_policy_tests = if ($audioTestsEnabled) { 'passed' } else { 'skipped-test-mode' }
        pe_x64 = $true
        version_match = $true
        intermediate_payload_sha256_match = $true
        extracted_rcdata_sha256_match = $true
        signatures_required = $ReleaseMode -eq 'Commercial'
        timestamp_required = $signingPolicy.RequireTimestamp
        signtool_verify = if (-not $hasSigningIdentity) {
            'skipped-test-mode-unsigned'
        } elseif ($signingPolicy.RequireTimestamp) {
            '/pa /all /tw /v passed'
        } else {
            '/pa /all /v passed (test mode without timestamp)'
        }
    }
    signing = [ordered] @{
        signed = $hasSigningIdentity
        publisher = if ($appSignature) {
            $appSignature.Publisher
        } else { '' }
        signer_thumbprint = if ($appSignature) {
            $appSignature.SignerThumbprint
        } else { '' }
        same_signer_app_installer = if ($hasSigningIdentity) {
            $true
        } else { $false }
        timestamped = if ($appSignature) {
            [bool] $appSignature.TimeStamped
        } else { $false }
    }
    embedded_payload = [ordered] @{
        resource_type = 'RCDATA'
        resource_id = 1
        app_sha256 = $appSha
        intermediate_sha256 = $intermediateSha
        extracted_sha256 = $extraction.Sha256
        bytes = $extraction.Bytes
    }
    update_policy = [ordered] @{
        generated = -not [string]::IsNullOrWhiteSpace($policyPath)
        policy_serial = if ($policyResult) {
            $policyResult.PolicySerial
        } else { 0 }
        minimum_version_code = if ($policyResult) {
            $policyResult.MinimumVersionCode
        } else { 0 }
    }
    artifacts = $artifactRecords
    logs_directory = 'logs'
}
Write-Utf8Json -Value $manifest -Path $manifestPath

$checksumsPath = Join-Path $stagingDir 'SHA256SUMS.txt'
$checksumLines = @(Get-ChildItem -LiteralPath $stagingDir -Recurse -File |
    Where-Object { $_.FullName -ne $checksumsPath } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stagingDir.Length + 1).
            Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $_.FullName `
            -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $relative"
    })
[IO.File]::WriteAllText(
    $checksumsPath,
    ($checksumLines -join [Environment]::NewLine) +
        [Environment]::NewLine,
    [Text.UTF8Encoding]::new($false))

$outputParent = Split-Path -Parent $OutputDir
New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
Move-Item -LiteralPath $stagingDir -Destination $OutputDir

foreach ($record in $artifactRecords) {
    $published = Join-Path $OutputDir $record.file
    $publishedHash = (Get-FileHash -LiteralPath $published `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($publishedHash -ne $record.sha256) {
        throw "Hash mudou na publicacao local: $published"
    }
}

if ($generatedBuildDir -and -not $KeepBuildDirectory) {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    $resolvedBuild = [IO.Path]::GetFullPath($BuildDir).TrimEnd('\') + '\'
    $markerValue = if (Test-Path -LiteralPath $ephemeralMarker -PathType Leaf) {
        [IO.File]::ReadAllText($ephemeralMarker)
    } else { '' }
    if (-not $resolvedBuild.StartsWith(
            $tempRoot,
            [StringComparison]::OrdinalIgnoreCase) -or
        $markerValue -ne $buildId -or
        [IO.Path]::GetFileName($BuildDir) -ne
            "churchdaw-windows-release-$buildId") {
        throw "Recusando limpar BuildDir sem prova de posse: $BuildDir"
    }
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

Write-Host ''
Write-Host '== release Windows validado localmente ==' -ForegroundColor Green
Write-Host "  $OutputDir"
if ($ReleaseMode -eq 'Test') {
    Write-Warning 'TEST ONLY: esta saida nao esta aprovada para distribuicao.'
}

[pscustomobject] @{
    Version = $version
    VersionCode = $versionCode
    ReleaseMode = $ReleaseMode
    DistributionApproved = $ReleaseMode -eq 'Commercial'
    OutputDirectory = $OutputDir
    Installer = Join-Path $OutputDir `
        "ChurchDAW-$version-Setup.exe"
    InstallerSha256 = (
        Get-FileHash -LiteralPath (
            Join-Path $OutputDir "ChurchDAW-$version-Setup.exe") `
            -Algorithm SHA256).Hash.ToLowerInvariant()
    Manifest = Join-Path $OutputDir 'release-manifest.json'
    Checksums = Join-Path $OutputDir 'SHA256SUMS.txt'
    Policy = if (-not [string]::IsNullOrWhiteSpace($policyPath)) {
        Join-Path $OutputDir 'update-policy-v1.json'
    } else { '' }
    BuildDirectory = if ($generatedBuildDir -and -not $KeepBuildDirectory) {
        ''
    } else { $BuildDir }
}
