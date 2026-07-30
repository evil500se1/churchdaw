[CmdletBinding()]
param(
    [string]$SourcePath = '',
    [switch]$KeepTemporaryFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SourcePath)) {
    $SourcePath = Join-Path $PSScriptRoot '..\Source\ChurchDAWServices.h'
}

$script:Passed = 0
$script:Failed = 0
$script:Failures = [System.Collections.Generic.List[string]]::new()

function Assert-True {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Assert-Equal {
    param(
        $Expected,
        $Actual,
        [string]$Message
    )

    if ($Expected -ne $Actual) {
        throw "$Message (esperado='$Expected', atual='$Actual')"
    }
}

function Invoke-Test {
    param(
        [string]$Name,
        [scriptblock]$Body
    )

    try {
        & $Body
        ++$script:Passed
        Write-Host "[PASS] $Name"
    }
    catch {
        ++$script:Failed
        $failure = "$Name`: $($_.Exception.Message)"
        $script:Failures.Add($failure)
        Write-Host "[FAIL] $failure" -ForegroundColor Red
    }
}

function Get-Sha256Hex {
    param([byte[]]$Bytes)

    $algorithm = [System.Security.Cryptography.SHA256]::Create()

    try {
        return ([System.BitConverter]::ToString(
            $algorithm.ComputeHash($Bytes)
        )).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Test-ShaMatches {
    param(
        [string]$Actual,
        [string]$Expected
    )

    return [System.StringComparer]::OrdinalIgnoreCase.Equals(
        $Actual,
        $Expected
    )
}

function Test-SafeIdentifier {
    param([string]$Value)

    return -not [string]::IsNullOrWhiteSpace($Value) -and
        $Value -match '^[A-Za-z0-9][A-Za-z0-9._-]*$'
}

function Test-HasProperty {
    param(
        $Object,
        [string]$Name
    )

    return $null -ne $Object -and
        $null -ne $Object.PSObject.Properties[$Name]
}

function Test-ResumePartValid {
    param($Part)

    foreach ($name in @(
        'index',
        'download_url',
        'sha256',
        'download_bytes',
        'extracted',
        'entry_count',
        'file_count',
        'expanded_bytes'
    )) {
        if (-not (Test-HasProperty $Part $name)) {
            return $false
        }
    }

    if ([int64]$Part.index -lt 0 -or
        [string]::IsNullOrWhiteSpace([string]$Part.download_url) -or
        -not ([string]$Part.download_url).StartsWith(
            'https://',
            [System.StringComparison]::OrdinalIgnoreCase
        ) -or
        ([string]$Part.sha256) -notmatch '^[0-9a-fA-F]{64}$' -or
        [int64]$Part.download_bytes -le 0 -or
        $Part.extracted -isnot [bool] -or
        [int64]$Part.entry_count -lt 0 -or
        [int64]$Part.file_count -lt 0 -or
        [int64]$Part.file_count -gt [int64]$Part.entry_count -or
        [int64]$Part.expanded_bytes -lt 0) {
        return $false
    }

    if ([bool]$Part.extracted) {
        return [int64]$Part.entry_count -gt 0 -and
            [int64]$Part.file_count -gt 0
    }

    return [int64]$Part.entry_count -eq 0 -and
        [int64]$Part.file_count -eq 0 -and
        [int64]$Part.expanded_bytes -eq 0
}

function Test-ResumeStateValid {
    param($State)

    foreach ($name in @(
        'schema_version',
        'id',
        'install_folder',
        'target_version',
        'installed_bytes',
        'staging_name',
        'parts'
    )) {
        if (-not (Test-HasProperty $State $name)) {
            return $false
        }
    }

    $parts = @($State.parts)

    if ([int64]$State.schema_version -ne 2 -or
        -not (Test-SafeIdentifier ([string]$State.id)) -or
        -not (Test-SafeIdentifier ([string]$State.install_folder)) -or
        [int64]$State.target_version -lt 1 -or
        [int64]$State.target_version -gt 1000000000 -or
        [int64]$State.installed_bytes -le 0 -or
        $parts.Count -le 0 -or
        -not ([string]$State.staging_name).StartsWith(
            ".installing-$($State.id)-",
            [System.StringComparison]::Ordinal
        ) -or
        ([string]$State.staging_name).Contains('/') -or
        ([string]$State.staging_name).Contains('\')) {
        return $false
    }

    [int64]$completedBytes = 0
    [int64]$completedEntries = 0

    for ($index = 0; $index -lt $parts.Count; ++$index) {
        $part = $parts[$index]

        if (-not (Test-ResumePartValid $part) -or
            [int64]$part.index -ne $index) {
            return $false
        }

        $completedBytes += [int64]$part.expanded_bytes
        $completedEntries += [int64]$part.entry_count

        if ($completedBytes -gt [int64]$State.installed_bytes -or
            $completedEntries -gt 1000000) {
            return $false
        }
    }

    return $true
}

function ConvertTo-ResumeJournalPayload {
    param($State)

    $serializedParts = @()

    foreach ($part in @($State.parts)) {
        $serializedParts += [ordered]@{
            index = [int]$part.index
            download_url = [string]$part.download_url
            sha256 = ([string]$part.sha256).ToLowerInvariant()
            download_bytes = [int64]$part.download_bytes
            extracted = [bool]$part.extracted
            entry_count = [int]$part.entry_count
            file_count = [int]$part.file_count
            expanded_bytes = [int64]$part.expanded_bytes
        }
    }

    return [ordered]@{
        schema_version = 2
        id = [string]$State.id
        install_folder = [string]$State.install_folder
        target_version = [int]$State.target_version
        installed_bytes = [int64]$State.installed_bytes
        staging_name = [string]$State.staging_name
        parts = $serializedParts
    }
}

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Text
    )

    $encoding = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Write-ResumeJournal {
    param(
        [string]$Path,
        $State
    )

    Assert-True (Test-ResumeStateValid $State) 'Estado de retomada invalido'
    $payload = ConvertTo-ResumeJournalPayload $State
    Write-Utf8NoBom $Path ($payload | ConvertTo-Json -Depth 12)
}

function Read-ResumeJournal {
    param([string]$Path)

    if (-not [System.IO.File]::Exists($Path)) {
        return $null
    }

    try {
        $state = [System.IO.File]::ReadAllText($Path) | ConvertFrom-Json
    }
    catch {
        return $null
    }

    if (-not (Test-ResumeStateValid $state)) {
        return $null
    }

    $expectedName = ".install-resume-$($state.id).json"

    if ([System.IO.Path]::GetFileName($Path) -cne $expectedName) {
        return $null
    }

    return $state
}

function Get-CompletedMetric {
    param(
        $State,
        [ValidateSet('entry_count', 'file_count', 'expanded_bytes')]
        [string]$Property
    )

    [int64]$result = 0

    foreach ($part in @($State.parts)) {
        $result += [int64]$part.$Property
    }

    return $result
}

function Get-StagingInspection {
    param(
        [string]$StagingPath,
        [string]$ExpectedId = 'fixture-pack',
        [int]$ExpectedVersion = 7
    )

    [int64]$bytes = 0
    [int]$files = 0
    $markerPath = Join-Path $StagingPath '.churchdaw-library.json'

    foreach ($file in @(Get-ChildItem -LiteralPath $StagingPath -File -Recurse)) {
        if (($file.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Staging de teste contem link/reparse point: $($file.FullName)"
        }

        if ([System.StringComparer]::OrdinalIgnoreCase.Equals(
            [System.IO.Path]::GetFullPath($file.FullName),
            [System.IO.Path]::GetFullPath($markerPath)
        )) {
            try {
                $marker = [System.IO.File]::ReadAllText($markerPath) |
                    ConvertFrom-Json
            }
            catch {
                throw 'Staging contem marker final invalido'
            }

            if (-not (Test-HasProperty $marker 'id') -or
                -not (Test-HasProperty $marker 'version') -or
                [string]$marker.id -cne $ExpectedId -or
                [int64]$marker.version -ne $ExpectedVersion -or
                [int64]$marker.version -lt 1 -or
                [int64]$marker.version -gt 1000000000) {
                throw 'Staging contem marker final invalido'
            }

            # O marker raiz valido pertence a fase de ativacao, nao ao ZIP.
            continue
        }

        ++$files
        $bytes += [int64]$file.Length
    }

    return [pscustomobject]@{
        file_count = $files
        expanded_bytes = $bytes
    }
}

function Test-ResumeMatchesStaging {
    param(
        $State,
        [string]$StagingPath
    )

    if (-not (Test-ResumeStateValid $State) -or
        -not [System.IO.Directory]::Exists($StagingPath)) {
        return $false
    }

    try {
        $actual = Get-StagingInspection `
            $StagingPath `
            ([string]$State.id) `
            ([int]$State.target_version)
    }
    catch {
        return $false
    }

    return [int64]$actual.expanded_bytes -eq
            (Get-CompletedMetric $State 'expanded_bytes') -and
        [int64]$actual.file_count -eq
            (Get-CompletedMetric $State 'file_count')
}

function Copy-JsonObject {
    param($Value)

    return ($Value | ConvertTo-Json -Depth 12 | ConvertFrom-Json)
}

function New-TestPart {
    param(
        [string]$Url,
        [byte[]]$ArchiveBytes,
        [System.Collections.IDictionary]$Files,
        [int]$EntryCount
    )

    $records = @()

    foreach ($relativePath in @($Files.Keys)) {
        $records += [pscustomobject]@{
            path = [string]$relativePath
            bytes = [byte[]]$Files[$relativePath]
        }
    }

    return [pscustomobject]@{
        download_url = $Url
        sha256 = (Get-Sha256Hex $ArchiveBytes).ToUpperInvariant()
        download_bytes = [int64]$ArchiveBytes.LongLength
        archive_bytes = $ArchiveBytes
        files = $records
        entry_count = $EntryCount
    }
}

function New-ResumeState {
    param(
        [object[]]$Fixtures,
        [int64]$InstalledBytes
    )

    $parts = @()

    for ($index = 0; $index -lt $Fixtures.Count; ++$index) {
        $fixture = $Fixtures[$index]
        $parts += [pscustomobject]@{
            index = $index
            download_url = [string]$fixture.download_url
            sha256 = [string]$fixture.sha256
            download_bytes = [int64]$fixture.download_bytes
            extracted = $false
            entry_count = 0
            file_count = 0
            expanded_bytes = [int64]0
        }
    }

    return [pscustomobject]@{
        schema_version = 2
        id = 'fixture-pack'
        install_folder = 'FixturePack'
        target_version = 7
        installed_bytes = $InstalledBytes
        staging_name = '.installing-fixture-pack-deadbeef'
        parts = $parts
    }
}

function Install-ModeledPart {
    param(
        $State,
        $Fixture,
        [int]$PartIndex,
        [string]$StagingPath,
        [string]$JournalPath
    )

    Assert-True (Test-ResumeStateValid $State) 'Estado anterior invalido'
    $part = @($State.parts)[$PartIndex]
    Assert-True (-not [bool]$part.extracted) "Parte $PartIndex ja extraida"

    $actualSha = Get-Sha256Hex ([byte[]]$Fixture.archive_bytes)
    Assert-True (
        Test-ShaMatches $actualSha ([string]$part.sha256)
    ) "SHA invalido na parte $PartIndex"

    Assert-Equal (
        [int64]$Fixture.download_bytes
    ) (
        [int64]$part.download_bytes
    ) "Tamanho de download divergente na parte $PartIndex"

    [int64]$expandedBytes = 0
    [int]$fileCount = 0
    $stagingFull = [System.IO.Path]::GetFullPath($StagingPath)
    $stagingPrefix = $stagingFull.TrimEnd(
        [System.IO.Path]::DirectorySeparatorChar,
        [System.IO.Path]::AltDirectorySeparatorChar
    ) + [System.IO.Path]::DirectorySeparatorChar

    foreach ($record in @($Fixture.files)) {
        $relative = ([string]$record.path).Replace(
            '/',
            [System.IO.Path]::DirectorySeparatorChar
        )
        $target = [System.IO.Path]::GetFullPath(
            (Join-Path $stagingFull $relative)
        )

        Assert-True (
            $target.StartsWith(
                $stagingPrefix,
                [System.StringComparison]::OrdinalIgnoreCase
            )
        ) "Entrada tentou escapar do staging: $relative"
        Assert-True (
            -not [System.IO.File]::Exists($target) -and
            -not [System.IO.Directory]::Exists($target)
        ) "Pack contem caminho de arquivo duplicado: $relative"

        [void][System.IO.Directory]::CreateDirectory(
            [System.IO.Path]::GetDirectoryName($target)
        )

        $contents = [byte[]]$record.bytes
        [System.IO.File]::WriteAllBytes($target, $contents)
        ++$fileCount
        $expandedBytes += [int64]$contents.LongLength
    }

    $part.extracted = $true
    $part.entry_count = [int]$Fixture.entry_count
    $part.file_count = $fileCount
    $part.expanded_bytes = $expandedBytes

    Assert-True (Test-ResumeStateValid $State) 'Estado concluido invalido'
    Write-ResumeJournal $JournalPath $State
}

function Assert-SourceRegex {
    param(
        [string]$Source,
        [string]$Pattern,
        [string]$Message
    )

    $matches = [System.Text.RegularExpressions.Regex]::Matches(
        $Source,
        $Pattern,
        [System.Text.RegularExpressions.RegexOptions]::Singleline
    )
    Assert-True ($matches.Count -gt 0) $Message
}

$resolvedSource = [System.IO.Path]::GetFullPath($SourcePath)
Assert-True ([System.IO.File]::Exists($resolvedSource)) (
    "Fonte nao encontrada: $resolvedSource"
)
$source = [System.IO.File]::ReadAllText($resolvedSource)

$temporaryBase = [System.IO.Path]::GetFullPath(
    [System.IO.Path]::GetTempPath()
).TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar
) + [System.IO.Path]::DirectorySeparatorChar
$temporaryRoot = Join-Path $temporaryBase (
    'ChurchDAW-InstallerRecoveryTests-' + [guid]::NewGuid().ToString('N')
)
$temporaryRoot = [System.IO.Path]::GetFullPath($temporaryRoot)

Assert-True (
    $temporaryRoot.StartsWith(
        $temporaryBase,
        [System.StringComparison]::OrdinalIgnoreCase
    )
) 'Diretorio de teste escapou da pasta temporaria'
Assert-True (
    ([System.IO.Path]::GetFileName($temporaryRoot)).StartsWith(
        'ChurchDAW-InstallerRecoveryTests-',
        [System.StringComparison]::Ordinal
    )
) 'Prefixo de seguranca do diretorio temporario invalido'

[void][System.IO.Directory]::CreateDirectory($temporaryRoot)

$staging = Join-Path $temporaryRoot '.installing-fixture-pack-deadbeef'
[void][System.IO.Directory]::CreateDirectory($staging)
$journal = Join-Path $temporaryRoot '.install-resume-fixture-pack.json'

$partAFiles = [ordered]@{
    'Piano/A.wav' = [byte[]](1, 2, 3, 4)
    'Piano/readme.txt' = [byte[]](5)
}
$partBFiles = [ordered]@{
    'Piano/B.flac' = [byte[]](6, 7, 8, 9, 10, 11, 12)
}
$partA = New-TestPart `
    'https://example.invalid/fixture-a.zip' `
    ([System.Text.Encoding]::UTF8.GetBytes('synthetic archive A')) `
    $partAFiles `
    3
$partB = New-TestPart `
    'https://example.invalid/fixture-b.zip' `
    ([System.Text.Encoding]::UTF8.GetBytes('synthetic archive B')) `
    $partBFiles `
    2
$fixtures = @($partA, $partB)
$state = New-ResumeState $fixtures 12

try {
    Invoke-Test 'journal inicial schema 2 inclui file_count e bytes zerados' {
        Write-ResumeJournal $journal $state
        $loaded = Read-ResumeJournal $journal
        Assert-True ($null -ne $loaded) 'Journal schema 2 nao foi aceito'
        Assert-Equal 2 ([int]$loaded.schema_version) 'Schema incorreto'
        Assert-Equal 0 ([int]$loaded.parts[0].file_count) (
            'file_count inicial incorreto'
        )
        Assert-Equal 0 ([int64]$loaded.parts[0].expanded_bytes) (
            'expanded_bytes inicial incorreto'
        )

        $raw = [System.IO.File]::ReadAllText($journal)
        Assert-True ($raw.Contains('"file_count"')) (
            'Journal serializado omitiu file_count'
        )
    }

    Invoke-Test 'schema 1 e rejeitado e schema 2 volta a ser aceito' {
        $schemaOne = ConvertTo-ResumeJournalPayload $state
        $schemaOne.schema_version = 1
        Write-Utf8NoBom $journal ($schemaOne | ConvertTo-Json -Depth 12)
        Assert-True ($null -eq (Read-ResumeJournal $journal)) (
            'Journal legado schema 1 foi aceito indevidamente'
        )

        Write-ResumeJournal $journal $state
        Assert-True ($null -ne (Read-ResumeJournal $journal)) (
            'Journal schema 2 valido foi rejeitado'
        )
    }

    Invoke-Test 'SHA-256 e comparado sem diferenciar caixa' {
        $lower = Get-Sha256Hex ([byte[]]$partA.archive_bytes)
        $upper = $lower.ToUpperInvariant()
        Assert-True (Test-ShaMatches $lower $upper) (
            'SHA identico com caixa diferente foi rejeitado'
        )

        $tampered = ('0' + $upper.Substring(1))

        if ($tampered -ceq $upper) {
            $tampered = ('1' + $upper.Substring(1))
        }

        Assert-True (-not (Test-ShaMatches $lower $tampered)) (
            'SHA realmente diferente foi aceito'
        )
    }

    Invoke-Test 'cancelamento apos parte A preserva estado retomavel' {
        Install-ModeledPart $state $partA 0 $staging $journal
        $resumed = Read-ResumeJournal $journal
        Assert-True ($null -ne $resumed) 'Journal apos parte A nao foi lido'
        Assert-True ([bool]$resumed.parts[0].extracted) (
            'Parte A nao foi confirmada'
        )
        Assert-True (-not [bool]$resumed.parts[1].extracted) (
            'Parte B foi confirmada antes da retomada'
        )
        Assert-Equal 2 (
            Get-CompletedMetric $resumed 'file_count'
        ) 'Contagem de arquivos apos parte A incorreta'
        Assert-Equal 5 (
            Get-CompletedMetric $resumed 'expanded_bytes'
        ) 'Bytes apos parte A incorretos'
        Assert-True (Test-ResumeMatchesStaging $resumed $staging) (
            'Staging da parte A nao corresponde ao journal'
        )
    }

    Invoke-Test 'staging adulterado por arquivo extra e detectado' {
        $resumed = Read-ResumeJournal $journal
        $rogue = Join-Path $staging 'Piano/rogue.bin'
        [System.IO.File]::WriteAllBytes($rogue, [byte[]](90, 91, 92))
        Assert-True (-not (Test-ResumeMatchesStaging $resumed $staging)) (
            'Arquivo extra no staging nao foi detectado'
        )
        [System.IO.File]::Delete($rogue)
        Assert-True (Test-ResumeMatchesStaging $resumed $staging) (
            'Staging restaurado deveria voltar a ser valido'
        )
    }

    Invoke-Test 'staging adulterado mantendo file_count mas mudando bytes e detectado' {
        $resumed = Read-ResumeJournal $journal
        $sample = Join-Path $staging 'Piano/A.wav'
        $original = [System.IO.File]::ReadAllBytes($sample)
        [System.IO.File]::WriteAllBytes($sample, [byte[]](1, 2, 3, 4, 5))
        Assert-Equal 2 (Get-StagingInspection $staging).file_count (
            'Pre-condicao do teste de bytes falhou'
        )
        Assert-True (-not (Test-ResumeMatchesStaging $resumed $staging)) (
            'Alteracao apenas de bytes nao foi detectada'
        )
        [System.IO.File]::WriteAllBytes($sample, $original)
        Assert-True (Test-ResumeMatchesStaging $resumed $staging) (
            'Staging restaurado deveria voltar a ser valido'
        )
    }

    Invoke-Test 'retomada pula A, instala B e fecha contagens exatas' {
        $resumed = Read-ResumeJournal $journal
        Assert-True ([bool]$resumed.parts[0].extracted) (
            'Retomada perdeu conclusao da parte A'
        )
        Assert-True (-not [bool]$resumed.parts[1].extracted) (
            'Parte B deveria estar pendente'
        )

        Install-ModeledPart $resumed $partB 1 $staging $journal
        $complete = Read-ResumeJournal $journal
        Assert-True ([bool]$complete.parts[0].extracted) (
            'Parte A deixou de estar concluida'
        )
        Assert-True ([bool]$complete.parts[1].extracted) (
            'Parte B nao foi concluida na retomada'
        )
        Assert-Equal 5 (
            Get-CompletedMetric $complete 'entry_count'
        ) 'entry_count multipartes incorreto'
        Assert-Equal 3 (
            Get-CompletedMetric $complete 'file_count'
        ) 'file_count multipartes incorreto'
        Assert-Equal 12 (
            Get-CompletedMetric $complete 'expanded_bytes'
        ) 'Bytes multipartes incorretos'
        Assert-Equal ([int64]$complete.installed_bytes) (
            Get-CompletedMetric $complete 'expanded_bytes'
        ) 'Bytes finais nao fecham com installed_bytes'
        Assert-True (Test-ResumeMatchesStaging $complete $staging) (
            'Staging final nao corresponde ao journal'
        )
    }

    Invoke-Test 'journal adulterado em file_count ou bytes e rejeitado pelo staging' {
        $complete = Read-ResumeJournal $journal

        $wrongFiles = Copy-JsonObject $complete
        $wrongFiles.parts[0].file_count =
            [int]$wrongFiles.parts[0].file_count + 1
        Assert-True (Test-ResumeStateValid $wrongFiles) (
            'Pre-condicao de file_count adulterado deveria manter estado estrutural'
        )
        Assert-True (-not (Test-ResumeMatchesStaging $wrongFiles $staging)) (
            'file_count adulterado nao foi detectado'
        )

        $wrongBytes = Copy-JsonObject $complete
        $wrongBytes.parts[0].expanded_bytes =
            [int64]$wrongBytes.parts[0].expanded_bytes - 1
        Assert-True (Test-ResumeStateValid $wrongBytes) (
            'Pre-condicao de bytes adulterados deveria manter estado estrutural'
        )
        Assert-True (-not (Test-ResumeMatchesStaging $wrongBytes $staging)) (
            'expanded_bytes adulterado nao foi detectado'
        )
    }

    Invoke-Test 'marker raiz valido nao entra em file_count nem expanded_bytes' {
        $complete = Read-ResumeJournal $journal
        $markerPath = Join-Path $staging '.churchdaw-library.json'
        $marker = [ordered]@{
            schema_version = 1
            id = 'fixture-pack'
            name = 'Fixture Pack'
            version = 7
            version_name = '7.0'
            installed_bytes = 12
            file_count = 3
            installed_at_ms = 123456789
        }
        Write-Utf8NoBom $markerPath ($marker | ConvertTo-Json -Depth 4)

        $inspection = Get-StagingInspection `
            $staging `
            'fixture-pack' `
            7
        Assert-Equal 3 ([int]$inspection.file_count) (
            'Marker raiz valido entrou em file_count'
        )
        Assert-Equal 12 ([int64]$inspection.expanded_bytes) (
            'Marker raiz valido entrou em expanded_bytes'
        )
        Assert-True (Test-ResumeMatchesStaging $complete $staging) (
            'Marker valido impediu retomada do staging concluido'
        )
        [System.IO.File]::Delete($markerPath)
    }

    Invoke-Test 'marker raiz invalido e rejeitado na retomada' {
        $complete = Read-ResumeJournal $journal
        $markerPath = Join-Path $staging '.churchdaw-library.json'
        $wrongVersion = [ordered]@{
            schema_version = 1
            id = 'fixture-pack'
            version = 6
        }
        Write-Utf8NoBom $markerPath (
            $wrongVersion | ConvertTo-Json -Depth 4
        )
        Assert-True (-not (Test-ResumeMatchesStaging $complete $staging)) (
            'Marker com versao errada foi aceito'
        )

        Write-Utf8NoBom $markerPath '{nao-e-json'
        Assert-True (-not (Test-ResumeMatchesStaging $complete $staging)) (
            'Marker malformado foi aceito'
        )
        [System.IO.File]::Delete($markerPath)
        Assert-True (Test-ResumeMatchesStaging $complete $staging) (
            'Staging deveria voltar a ser valido sem marker invalido'
        )
    }

    Invoke-Test 'caminho de arquivo duplicado entre partes e rejeitado' {
        $duplicateRoot = Join-Path $temporaryRoot 'duplicate-case'
        [void][System.IO.Directory]::CreateDirectory($duplicateRoot)

        $duplicateA = New-TestPart `
            'https://example.invalid/duplicate-a.zip' `
            ([System.Text.Encoding]::UTF8.GetBytes('duplicate archive A')) `
            ([ordered]@{
                'Shared/duplicate.wav' = [byte[]](20, 21)
            }) `
            2
        $duplicateB = New-TestPart `
            'https://example.invalid/duplicate-b.zip' `
            ([System.Text.Encoding]::UTF8.GetBytes('duplicate archive B')) `
            ([ordered]@{
                'Shared/duplicate.wav' = [byte[]](30, 31, 32)
            }) `
            2
        $duplicateState = New-ResumeState @(
            $duplicateA,
            $duplicateB
        ) 5
        $duplicateState.staging_name =
            '.installing-fixture-pack-duplicate'
        $duplicateStaging = Join-Path `
            $duplicateRoot `
            $duplicateState.staging_name
        [void][System.IO.Directory]::CreateDirectory($duplicateStaging)
        $duplicateJournal = Join-Path `
            $duplicateRoot `
            '.install-resume-fixture-pack.json'

        Install-ModeledPart `
            $duplicateState `
            $duplicateA `
            0 `
            $duplicateStaging `
            $duplicateJournal
        $originalBytes = [System.IO.File]::ReadAllBytes(
            (Join-Path $duplicateStaging 'Shared/duplicate.wav')
        )
        $rejected = $false

        try {
            Install-ModeledPart `
                $duplicateState `
                $duplicateB `
                1 `
                $duplicateStaging `
                $duplicateJournal
        }
        catch {
            if ($_.Exception.Message -like '*caminho de arquivo duplicado*') {
                $rejected = $true
            }
            else {
                throw
            }
        }

        Assert-True $rejected (
            'Parte B sobrescreveu caminho ja extraido pela parte A'
        )
        Assert-True (-not [bool]$duplicateState.parts[1].extracted) (
            'Parte B duplicada foi marcada como extraida'
        )
        Assert-True (
            [System.Convert]::ToBase64String($originalBytes) -ceq
            [System.Convert]::ToBase64String(
                [System.IO.File]::ReadAllBytes(
                    (Join-Path $duplicateStaging 'Shared/duplicate.wav')
                )
            )
        ) 'Arquivo original da parte A foi alterado'

        $persisted = Read-ResumeJournal $duplicateJournal
        Assert-True ($null -ne $persisted) (
            'Journal seguro da parte A deixou de ser legivel'
        )
        Assert-True (-not [bool]$persisted.parts[1].extracted) (
            'Journal confirmou parte B duplicada'
        )
        Assert-True (
            Test-ResumeMatchesStaging $persisted $duplicateStaging
        ) 'Staging da parte A deveria continuar retomavel'
    }

    Invoke-Test 'source: journal de retomada grava e exige schema 2' {
        Assert-SourceRegex $source (
            'writeLibraryInstallResumeState\s*\(.*?' +
            'setProperty\s*\(\s*"schema_version"\s*,\s*2\s*\)'
        ) 'Writer nao fixa schema_version 2'
        Assert-SourceRegex $source (
            'readLibraryInstallResumeState\s*\(.*?' +
            'schemaVersion\s*!=\s*2'
        ) 'Reader nao rejeita schema diferente de 2'
    }

    Invoke-Test 'source: journal persiste e le file_count' {
        Assert-SourceRegex $source (
            'writeLibraryInstallResumeState\s*\(.*?' +
            'setProperty\s*\(\s*"file_count"\s*,\s*part\.fileCount\s*\)'
        ) 'Writer nao persiste part.fileCount'
        Assert-SourceRegex $source (
            'readLibraryInstallResumeState\s*\(.*?' +
            'getProperty\s*\(\s*"file_count"\s*\)'
        ) 'Reader nao le file_count'
        Assert-SourceRegex $source (
            'completedFileCount\s*\(\s*\)\s*const'
        ) 'Agregador completedFileCount ausente'
    }

    Invoke-Test 'source: retomada compara staging real por bytes e file_count' {
        Assert-SourceRegex $source (
            'const\s+auto\s+expectedExpanded\s*=\s*' +
            'parsed\.completedExpandedBytes\s*\(\s*\).*?' +
            'const\s+auto\s+expectedFiles\s*=\s*' +
            'parsed\.completedFileCount\s*\(\s*\).*?' +
            'actualStaging\.expandedBytes\s*!=\s*expectedExpanded.*?' +
            'actualStaging\.fileCount\s*!=\s*expectedFiles'
        ) 'Retomada nao confere bytes e arquivos reais do staging'
    }

    Invoke-Test 'source: ativacao compara staging final por bytes e file_count' {
        Assert-SourceRegex $source (
            'const\s+auto\s+totalFiles\s*=\s*' +
            'resumeState\.completedFileCount\s*\(\s*\).*?' +
            'actualStaging\.expandedBytes\s*!=\s*expandedBytes.*?' +
            'actualStaging\.fileCount\s*!=\s*totalFiles'
        ) 'Ativacao nao confere bytes e arquivos reais do staging'
    }

    Invoke-Test 'source: partes concluidas registram file_count da inspecao' {
        Assert-SourceRegex $source (
            'completedPart\.entryCount\s*=\s*inspection\.entryCount\s*;' +
            '.*?completedPart\.fileCount\s*=\s*inspection\.fileCount\s*;' +
            '.*?completedPart\.expandedBytes\s*=\s*inspection\.expandedBytes\s*;'
        ) 'Conclusao de parte nao registra contagens e bytes inspecionados'
    }

    Invoke-Test 'source: os dois caminhos de checksum sao case-insensitive' {
        $checksumMatches = [System.Text.RegularExpressions.Regex]::Matches(
            $source,
            'actualSha256\.equalsIgnoreCase\s*\(\s*downloadPart\.sha256\s*\)',
            [System.Text.RegularExpressions.RegexOptions]::Singleline
        )
        Assert-True ($checksumMatches.Count -ge 2) (
            'Esperados dois checks equalsIgnoreCase para SHA-256'
        )
    }

    Invoke-Test 'source: journal incompativel e descartado com seguranca' {
        Assert-SourceRegex $source (
            'const\s+bool\s+compatible\s*=\s*parsedOk.*?' +
            'libraryInstallResumeMatchesPack\s*\(\s*parsed\s*,\s*pack\s*\)' +
            '.*?if\s*\(\s*!\s*compatible\s*\).*?' +
            'discardResumeArtifacts'
        ) 'Retomada incompativel nao aciona descarte seguro'
    }

    Invoke-Test 'source: marcador final persiste quantidade real de arquivos' {
        Assert-SourceRegex $source (
            'marker->setProperty\s*\(\s*"file_count"\s*,\s*totalFiles\s*\)'
        ) (
            'Marcador final usa outra metrica em file_count; ' +
            'esperado totalFiles, nao totalEntries'
        )
    }

    Invoke-Test 'source: marker raiz valido e ignorado nas metricas do staging' {
        Assert-SourceRegex $source (
            'inspectStagingContents\s*\(.*?' +
            'file\s*==\s*staging\.getChildFile\s*\(\s*' +
            '"\.churchdaw-library\.json"\s*\).*?' +
            'validLibraryMarkerVersion\s*\(\s*staging\s*,\s*pack\.id\s*\)' +
            '\s*!=\s*pack\.version.*?' +
            '"Staging contem marker final invalido".*?' +
            'continue\s*;.*?' +
            'const\s+auto\s+bytes\s*=\s*entry\.getFileSize\s*\(\s*\)'
        ) (
            'Inspecao nao valida e pula o marker raiz antes de contar bytes'
        )
    }

    Invoke-Test 'source: arquivo duplicado e rejeitado antes da extracao' {
        Assert-SourceRegex $source (
            'extractArchiveEntryWithLimits\s*\(.*?' +
            'if\s*\(\s*output\.exists\s*\(\s*\)\s*\).*?' +
            '"Pack contem caminho de arquivo duplicado"'
        ) 'Extracao nao rejeita arquivo ja existente no staging'
    }

    Invoke-Test 'source: extracao usa stream e temporario com limite exato' {
        Assert-SourceRegex $source (
            'extractArchiveEntryWithLimits\s*\(.*?' +
            'output\.getSiblingFile\s*\(\s*' +
            '"\.extracting-"\s*\+\s*juce::Uuid\s*\(\s*\)\.toString\s*\(\s*\)' +
            '\s*\).*?' +
            'archive\.createStreamForEntry\s*\(\s*entryIndex\s*\).*?' +
            'while\s*\(\s*written\s*<\s*entry->uncompressedSize\s*\).*?' +
            'entry->uncompressedSize\s*-\s*written.*?' +
            'input->read\s*\(\s*buffer\.getData\s*\(\s*\)\s*,\s*wanted\s*\)' +
            '.*?received\s*>\s*wanted'
        ) (
            'Extracao nao demonstra stream limitado ao tamanho declarado'
        )
    }

    Invoke-Test 'source: extracao le byte extra e so move apos tamanho exato' {
        Assert-SourceRegex $source (
            'char\s+unexpectedByte\s*=\s*0\s*;.*?' +
            'const\s+int\s+trailingBytes\s*=\s*' +
            'input->read\s*\(\s*&unexpectedByte\s*,\s*1\s*\)\s*;.*?' +
            'if\s*\(\s*trailingBytes\s*!=\s*0\s*\).*?' +
            'temporary\.getSize\s*\(\s*\)\s*!=\s*entry->uncompressedSize' +
            '.*?output\.exists\s*\(\s*\).*?' +
            'temporary\.moveFileTo\s*\(\s*output\s*\)'
        ) (
            'Extracao nao prova EOF e tamanho exato antes do move atomico'
        )
    }
}
finally {
    if ($KeepTemporaryFiles) {
        Write-Host "Temporarios preservados em: $temporaryRoot"
    }
    elseif ([System.IO.Directory]::Exists($temporaryRoot)) {
        Assert-True (
            $temporaryRoot.StartsWith(
                $temporaryBase,
                [System.StringComparison]::OrdinalIgnoreCase
            ) -and
            ([System.IO.Path]::GetFileName($temporaryRoot)).StartsWith(
                'ChurchDAW-InstallerRecoveryTests-',
                [System.StringComparison]::Ordinal
            )
        ) 'Recusa de limpar caminho temporario inesperado'
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}

Write-Host ''
Write-Host "Resumo: $($script:Passed) passou; $($script:Failed) falhou."

if ($script:Failures.Count -gt 0) {
    foreach ($failure in $script:Failures) {
        Write-Host " - $failure" -ForegroundColor Red
    }

    exit 1
}

exit 0
