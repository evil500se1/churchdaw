# BuildTimbreCatalog.ps1
# Gera catalogo de timbres individuais a partir de Libraries/church-live
# e (opcionalmente) empacota + envia um ZIP por instrumento via FTPS.
#
# Uso:
#   .\BuildTimbreCatalog.ps1                              # so catalogo local
#   .\BuildTimbreCatalog.ps1 -Upload -Only "Pad2"         # sobe um timbre
#   .\BuildTimbreCatalog.ps1 -Upload -MaxChunkMb 500      # partes de no max 500 MB
#
# Host so aceita ~500 MB por upload: ZIPs maiores sao partidos em
#   id-v1.zip.part01, .part02, ... e o app reconstroi antes de extrair.

param(
    [string] $LibraryRoot = "$env:APPDATA\ChurchDAW\Libraries\church-live",
    [string] $OutDir = "$PSScriptRoot\TimbrePacks",
    [string] $PublicBaseUrl = "https://churchdaw.com/app/android/timbres",
    [string] $FtpRemoteDir = "/app/android/timbres",
    [string] $CredentialFile = "$env:USERPROFILE\ChurchDAW-Secrets\churchdaw-ftp.credential.xml",
    [switch] $Upload,
    [double] $MaxChunkMb = 500,
    [double] $MaxMb = 0,
    [string[]] $Only = @(),
    [switch] $ForceReupload
)

$ErrorActionPreference = 'Stop'
$ftpHost = 'churchdaw.com'
$certificateSha256 =
    '490A628ED4151A5CAB3FCBACB17C0DE25199CD902B3FD594FEF6201321107F8C'

Add-Type -TypeDefinition @'
using System;
using System.Net;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

public static class ChurchDawTimbreFtpPin
{
    private static RemoteCertificateValidationCallback previous;

    private static bool Validate(object sender,
                                 X509Certificate certificate,
                                 X509Chain chain,
                                 SslPolicyErrors errors)
    {
        using (var sha = SHA256.Create())
        {
            var actual = BitConverter.ToString(
                sha.ComputeHash(certificate.GetRawCertData()))
                .Replace("-", "");
            return String.Equals(
                actual,
                "490A628ED4151A5CAB3FCBACB17C0DE25199CD902B3FD594FEF6201321107F8C",
                StringComparison.Ordinal);
        }
    }

    public static void Install()
    {
        previous = ServicePointManager.ServerCertificateValidationCallback;
        ServicePointManager.ServerCertificateValidationCallback = Validate;
    }

    public static void Restore()
    {
        ServicePointManager.ServerCertificateValidationCallback = previous;
    }
}
'@
[ChurchDawTimbreFtpPin]::Install()

if (-not (Test-Path -LiteralPath $LibraryRoot)) {
    throw "Biblioteca nao encontrada: $LibraryRoot"
}

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$catalogPath = Join-Path $OutDir 'timbre-catalog-v1.json'

function Get-InstrumentSlug([string] $name) {
    if ($name -match '(?i)pad_worship_2_0_ed_bento|pad worship 2\.0 ed bento|new pad') { return 'pad_worship_2_0_ed_bento' }
    $s = $name.ToLowerInvariant()
    $s = $s -replace '[^a-z0-9]+', '-'
    $s = $s.Trim('-')
    if ([string]::IsNullOrWhiteSpace($s)) { $s = 'timbre' }
    return $s
}

function Get-CategoryFor([string] $name, [string] $folder) {
    $n = ($name + ' ' + $folder).ToLowerInvariant()
    if ($n -match 'piano|upright|grand|ep |ep\b|stockholm|ballad|keys|tines') { return 'PIANOS' }
    if ($n -match 'organ|b3|farf|pipe|orgao') { return 'ORGAOS' }
    if ($n -match 'pad|ambience|warm|rev') { return 'PADS' }
    if ($n -match 'bell') { return 'BELLS' }
    if ($n -match 'string') { return 'STRINGS' }
    if ($n -match 'lead|synth|analog|saw|sine|pluck|fx') { return 'LEADS & SYNTHS' }
    return 'PIANOS'
}

function Test-IsOrganSample([string] $name, [string] $folder) {
    $n = ($name + ' ' + $folder).ToLowerInvariant()
    return ($n -match '\bb3\b|farf|pipe organ|pipe organs|organ|orgao|hammond|leslie')
}

function Get-DisplayName([string] $raw) {
    if ($raw -match '(?i)pad_worship_2_0_ed_bento|pad worship 2\.0 ed bento|new pad') { return 'New Pad' }
    # Remove codigos de acervo: LP4, LP, FC, NS4, etc. (so exibicao)
    $d = $raw.Trim()
    $d = $d -replace '(?i)\s+LP\d+\b', ''
    $d = $d -replace '(?i)\s+LP\b', ''
    $d = $d -replace '(?i)\s+FC\d*\b', ''
    $d = $d -replace '(?i)\s+NS\d+\b', ''
    $d = $d -replace '(?i)LP\d+$', ''
    $d = $d -replace '(?i)FC\d*$', ''
    $d = $d -replace '\s+', ' '
    return $d.Trim()
}

# --- Agrupa samples como o app: pastas pequenas = 1 timbre; Imported = por nome ---
$groups = @{}  # key -> @{ name; folder; files=ArrayList; bytes }

function Add-FileToGroup([string] $instr, [string] $folder, [System.IO.FileInfo] $file) {
    if ([string]::IsNullOrWhiteSpace($instr)) { return }
    $key = $instr.Trim()
    if (-not $groups.ContainsKey($key)) {
        $groups[$key] = @{
            name   = $key
            folder = $folder
            files  = New-Object System.Collections.Generic.List[string]
            bytes  = [int64]0
        }
    }
    $groups[$key].files.Add($file.FullName) | Out-Null
    $groups[$key].bytes += $file.Length
    if ($groups[$key].folder -ne $folder -and $folder -ne 'Imported') {
        $groups[$key].folder = $folder
    }
}

# 1) Pastas instrumentais (nao Imported)
Get-ChildItem -LiteralPath $LibraryRoot -Directory | Where-Object {
    $_.Name -ne 'Imported' -and -not $_.Name.StartsWith('__')
} | ForEach-Object {
    $topDir = $_
    $subDirs = Get-ChildItem -LiteralPath $topDir.FullName -Directory -ErrorAction SilentlyContinue | Where-Object { -not $_.Name.StartsWith('__') }
    if ($subDirs -and $subDirs.Count -gt 0) {
        foreach ($sub in $subDirs) {
            $folderRel = "$($topDir.Name)/$($sub.Name)"
            $audio = Get-ChildItem -LiteralPath $sub.FullName -File -Recurse -Include *.flac,*.wav,*.aif,*.aiff -ErrorAction SilentlyContinue
            if ($null -eq $audio -or $audio.Count -eq 0) { continue }
            foreach ($f in $audio) { Add-FileToGroup $sub.Name $folderRel $f }
        }
    } else {
        $folder = $topDir.Name
        $audio = Get-ChildItem -LiteralPath $topDir.FullName -File -Recurse -Include *.flac,*.wav,*.aif,*.aiff -ErrorAction SilentlyContinue
        if ($null -eq $audio -or $audio.Count -eq 0) { return }

        if ($audio.Count -le 120) {
            foreach ($f in $audio) { Add-FileToGroup $folder $folder $f }
        }
        else {
            # pasta grande com multiplos instrumentos no nome do arquivo (padrao B3-style)
            foreach ($f in $audio) {
                $stem = [IO.Path]::GetFileNameWithoutExtension($f.Name)
                $parts = $stem -split '-'
                if ($parts.Count -ge 4) {
                    $velTok = $parts[$parts.Count - 2]
                    if ($velTok -match '^V?\d+$') {
                        $nameParts = $parts[0..($parts.Count - 4)]
                        $instr = ($nameParts -join '-').Trim()
                        if ($instr) { Add-FileToGroup $instr $folder $f; continue }
                    }
                }
                Add-FileToGroup $folder $folder $f
            }
        }
    }
}

# 2) Imported: desmembra por instrumento
$imported = Join-Path $LibraryRoot 'Imported'
if (Test-Path $imported) {
    Get-ChildItem -LiteralPath $imported -File -Include *.flac,*.wav,*.aif,*.aiff -ErrorAction SilentlyContinue | ForEach-Object {
        $f = $_
        $stem = [IO.Path]::GetFileNameWithoutExtension($f.Name)

        # "Black Upright LP4 FC-A#-1-V102-U1AR" (espaco + FC-nota)
        if ($stem -match '^(.*?)\s+FC-') {
            Add-FileToGroup $Matches[1].Trim() 'Imported' $f
            return
        }
        # "B3 Pop-A0-V127-XXXX"
        $parts = $stem -split '-'
        if ($parts.Count -ge 4) {
            $velTok = $parts[$parts.Count - 2]
            if ($velTok -match '^V?\d+$') {
                # nota pode ser C#-1 -> 2 tokens
                $noteIdx = $parts.Count - 3
                if ($parts[$noteIdx] -match '^\d+$' -and $noteIdx -gt 0) { $noteIdx-- }
                if ($noteIdx -ge 1) {
                    $instr = ($parts[0..($noteIdx - 1)] -join '-').Trim()
                    if ($instr) { Add-FileToGroup $instr 'Imported' $f; return }
                }
            }
        }
        # "10Pad2" / "22Pad2" -> Pad2
        if ($stem -match '^(\d+)([A-Za-z].+)$') {
            Add-FileToGroup $Matches[2] 'Imported' $f
            return
        }
        # fallback: nome inteiro
        Add-FileToGroup $stem 'Imported' $f
    }
}

Write-Host ("Instrumentos detectados: {0}" -f $groups.Count)

# Filtra lixo (notas soltas etc.)
$minFiles = 8
$instruments = @()
foreach ($key in ($groups.Keys | Sort-Object)) {
    $g = $groups[$key]
    if ($g.files.Count -lt $minFiles) { continue }
    if ($key.Length -le 2 -and $key -match '^[A-G]#?$') { continue }
    # Orgaos de sample ficam fora: o app usa o Church Organ Engine.
    if (Test-IsOrganSample $key $g.folder) { continue }

    $slug = Get-InstrumentSlug $key
    $name = Get-DisplayName $key
    $rawLabel = if ($slug -eq 'pad_worship_2_0_ed_bento') { 'New Pad' } else { $key }
    $category = Get-CategoryFor $key $g.folder
    $installRelative = $g.folder
    $fileCount = $g.files.Count
    $downloadBytes = [int64]$g.bytes
    $downloadUrl = "$PublicBaseUrl/$slug-v1.zip"
    $sha256 = ''


    $instruments += [ordered]@{
        id               = $slug
        name             = $name
        raw_label        = $rawLabel
        category         = $category
        library_id       = 'church-live'
        install_relative = $installRelative
        file_count       = $fileCount
        download_bytes   = $downloadBytes
        download_url     = $downloadUrl
        sha256           = $sha256
        included         = $true
        _files           = @($g.files)
        _uploadNow       = $(
            if ($Only.Count -eq 0) { $true }
            else { ($Only -contains $key) -or ($Only -contains (Get-DisplayName $key)) }
        )
    }
}

Write-Host ("Instrumentos no catalogo (>= $minFiles samples): {0}" -f $instruments.Count)

function New-FtpRequest([string]$RemotePath, [string]$Method, $Credential) {
    $normalized = '/' + $RemotePath.TrimStart('/')
    $req = [Net.FtpWebRequest]::Create([Uri]("ftp://${ftpHost}${normalized}"))
    $req.Method = $Method
    $req.Credentials = $Credential
    $req.EnableSsl = $true
    $req.UseBinary = $true
    $req.UsePassive = $true
    $req.KeepAlive = $false
    return $req
}

function Ensure-FtpDir([string]$RemoteDir, $Credential) {
    $parts = $RemoteDir.Trim('/').Split('/') | Where-Object { $_ }
    $path = ''
    foreach ($p in $parts) {
        $path = "$path/$p"
        try {
            $req = New-FtpRequest $path ([Net.WebRequestMethods+Ftp]::MakeDirectory) $Credential
            $resp = $req.GetResponse()
            $resp.Close()
        } catch {
            # ja existe
        }
    }
}

function Upload-FtpFile([string]$LocalFile, [string]$RemoteFile, $Credential) {
    $info = Get-Item -LiteralPath $LocalFile
    $maxBytes = [int64]([math]::Floor($MaxChunkMb * 1MB))
    if ($info.Length -gt $maxBytes) {
        throw ("Arquivo {0} tem {1:N1} MB - acima do limite de chunk {2} MB. Partir antes do upload." -f `
            $info.Name, ($info.Length / 1MB), $MaxChunkMb)
    }

    $attempts = 0
    $maxAttempts = 4
    while ($true) {
        $attempts++
        try {
            $req = New-FtpRequest $RemoteFile ([Net.WebRequestMethods+Ftp]::UploadFile) $Credential
            $req.Timeout = 60 * 60 * 1000
            $req.ReadWriteTimeout = 60 * 60 * 1000
            $req.ContentLength = $info.Length
            $fs = [IO.File]::OpenRead($LocalFile)
            try {
                $stream = $req.GetRequestStream()
                try {
                    $buffer = New-Object byte[] (256 * 1024)
                    while (($n = $fs.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        $stream.Write($buffer, 0, $n)
                    }
                } finally {
                    $stream.Close()
                }
                $resp = $req.GetResponse()
                $resp.Close()
            } finally {
                $fs.Close()
            }
            # pausa entre uploads para nao estourar cota/rate limit do host
            Start-Sleep -Seconds 3
            return
        } catch {
            $msg = $_.Exception.Message
            if ($attempts -ge $maxAttempts) {
                throw ("Upload falhou apos {0} tentativas: {1}" -f $maxAttempts, $msg)
            }
            Write-Host ("  retry {0}/{1} em 20s... ({2})" -f $attempts, $maxAttempts, $msg)
            Start-Sleep -Seconds 20
        }
    }
}

# Parte um arquivo em pedacos <= MaxChunkMb. Retorna lista de caminhos .partNN
function Split-FileIntoChunks([string]$SourcePath, [string]$DestDir, [string]$BaseName) {
    $chunkSize = [int64]([math]::Floor($MaxChunkMb * 1MB))
    if ($chunkSize -lt 1MB) { $chunkSize = 500MB }

    $src = Get-Item -LiteralPath $SourcePath
    $parts = New-Object System.Collections.Generic.List[string]

    if ($src.Length -le $chunkSize) {
        $parts.Add($SourcePath) | Out-Null
        return [string[]]$parts.ToArray()
    }

    $fs = [IO.File]::OpenRead($SourcePath)
    try {
        $buffer = New-Object byte[] (1024 * 1024)
        $index = 1
        $remainingInChunk = $chunkSize
        $partPath = Join-Path $DestDir ("{0}.part{1:D2}" -f $BaseName, $index)
        $out = [IO.File]::Create($partPath)
        try {
            while (($n = $fs.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $offset = 0
                while ($offset -lt $n) {
                    $take = [int][math]::Min($n - $offset, $remainingInChunk)
                    $out.Write($buffer, $offset, $take)
                    $offset += $take
                    $remainingInChunk -= $take

                    if ($remainingInChunk -le 0) {
                        $out.Close()
                        $parts.Add($partPath) | Out-Null
                        Write-Host ("  parte {0}: {1:N1} MB" -f $index, ((Get-Item $partPath).Length / 1MB))
                        $index++
                        $partPath = Join-Path $DestDir ("{0}.part{1:D2}" -f $BaseName, $index)
                        $out = [IO.File]::Create($partPath)
                        $remainingInChunk = $chunkSize
                    }
                }
            }
        } finally {
            if ($null -ne $out) {
                $out.Close()
                if ((Get-Item $partPath).Length -gt 0) {
                    $parts.Add($partPath) | Out-Null
                    Write-Host ("  parte {0}: {1:N1} MB" -f $index, ((Get-Item $partPath).Length / 1MB))
                } else {
                    Remove-Item $partPath -Force -ErrorAction SilentlyContinue
                }
            }
        }
    } finally {
        $fs.Close()
    }

    return [string[]]$parts.ToArray()
}

$credential = $null
if ($Upload) {
    if (-not (Test-Path $CredentialFile)) { throw "FTP credential missing: $CredentialFile" }
    $credential = Import-Clixml -LiteralPath $CredentialFile
    $netCred = New-Object System.Net.NetworkCredential(
        $credential.UserName,
        $credential.GetNetworkCredential().Password)
    Ensure-FtpDir $FtpRemoteDir $netCred
}

$published = @()
foreach ($inst in $instruments) {
    $mb = [math]::Round($inst.download_bytes / 1MB, 1)
    $zipName = "$($inst.id)-v1.zip"
    $zipPath = Join-Path $OutDir $zipName
    $sizeOk = ($MaxMb -le 0) -or ($mb -le $MaxMb) -or ($Only.Count -gt 0)
    $shouldPack = $Upload -and $inst._uploadNow -and $sizeOk

    Write-Host ("- {0} ({1} files, {2} MB raw) cat={3}" -f $inst.name, $inst.file_count, $mb, $inst.category)

    $partEntries = @()

    if ($shouldPack) {
        if ((Test-Path $zipPath) -and -not $ForceReupload) {
            Write-Host "  zip existe, reutilizando"
        } else {
            Get-ChildItem -LiteralPath $OutDir -Filter "$($inst.id)-v1.zip*" -ErrorAction SilentlyContinue |
                Remove-Item -Force -ErrorAction SilentlyContinue

            $stage = Join-Path $OutDir (".stage-" + $inst.id)
            if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
            $destRoot = Join-Path $stage $inst.install_relative
            New-Item -ItemType Directory -Path $destRoot -Force | Out-Null
            foreach ($src in $inst._files) {
                $leaf = [IO.Path]::GetFileName($src)
                Copy-Item -LiteralPath $src -Destination (Join-Path $destRoot $leaf) -Force
            }
            Add-Type -AssemblyName System.IO.Compression
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            [System.IO.Compression.ZipFile]::CreateFromDirectory(
                $stage, $zipPath,
                [System.IO.Compression.CompressionLevel]::Optimal,
                $false)
            Remove-Item $stage -Recurse -Force
            Write-Host ("  zip completo {0:N1} MB" -f ((Get-Item $zipPath).Length / 1MB))
        }

        try {
            $fullHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
            $fullSize = [int64](Get-Item $zipPath).Length
            $inst.sha256 = $fullHash
            $inst.download_bytes = $fullSize
            $inst.download_url = "$PublicBaseUrl/$zipName"

            # Parte em chunks de MaxChunkMb (ex.: 500 MB) para o limite do host
            $chunkPaths = @(Split-FileIntoChunks -SourcePath $zipPath -DestDir $OutDir -BaseName $zipName)
            # Desembrulha array aninhado (PowerShell as vezes envelopa)
            if ($chunkPaths.Count -eq 1 -and $chunkPaths[0] -is [System.Array]) {
                $chunkPaths = @($chunkPaths[0])
            }
            $chunkPaths = [string[]]@($chunkPaths | ForEach-Object { "$_" })
            $multiPart = ($chunkPaths.Count -gt 1) -or (
                $chunkPaths.Count -eq 1 -and $chunkPaths[0] -ne $zipPath)

            Write-Host ("  chunks={0} multi={1}" -f $chunkPaths.Count, $multiPart)

            if (-not $multiPart) {
                if ($Upload) {
                    Write-Host ("  upload 1/1 {0} ({1:N1} MB)..." -f $zipName, ($fullSize / 1MB))
                    Upload-FtpFile $zipPath "$FtpRemoteDir/$zipName" $netCred
                    Write-Host "  ok"
                }
                $partEntries = @()
            } else {
                $partIndex = 0
                $partEntries = @()
                foreach ($partPath in $chunkPaths) {
                    $partIndex++
                    if (-not (Test-Path -LiteralPath $partPath)) {
                        throw ("Parte ausente: {0}" -f $partPath)
                    }
                    $partName = [IO.Path]::GetFileName($partPath)
                    $partHash = (Get-FileHash -LiteralPath $partPath -Algorithm SHA256).Hash.ToLowerInvariant()
                    $partSize = [int64](Get-Item -LiteralPath $partPath).Length
                    $partUrl = "$PublicBaseUrl/$partName"

                    if ($partSize -gt ([int64]([math]::Floor($MaxChunkMb * 1MB)) + 1MB)) {
                        throw ("Parte {0} ainda excede o limite ({1:N1} MB)" -f $partName, ($partSize / 1MB))
                    }

                    if ($Upload) {
                        Write-Host ("  upload {0}/{1} {2} ({3:N1} MB)..." -f `
                            $partIndex, $chunkPaths.Count, $partName, ($partSize / 1MB))
                        Upload-FtpFile $partPath "$FtpRemoteDir/$partName" $netCred
                        Write-Host "  ok parte $partIndex"
                        if ($partPath -ne $zipPath) {
                            Remove-Item -LiteralPath $partPath -Force -ErrorAction SilentlyContinue
                        }
                    }

                    $partEntries += [ordered]@{
                        download_url   = $partUrl
                        sha256         = $partHash
                        download_bytes = $partSize
                    }
                }

                # ZIP completo nao sobe inteiro - so as partes
                $inst.download_url = ""
            }

            if ($Upload) {
                Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
                Get-ChildItem -LiteralPath $OutDir -Filter "$($inst.id)-v1.zip.part*" -ErrorAction SilentlyContinue |
                    Remove-Item -Force -ErrorAction SilentlyContinue
            }
        } catch {
            Write-Host ("  FALHA upload {0}: {1}" -f $inst.name, $_.Exception.Message)
            $inst.sha256 = ''
            $inst.download_url = ''
            $partEntries = @()
            # nao aborta o lote: segue para o proximo timbre apos pausa
            Start-Sleep -Seconds 15
        }
    }

    $ready = [bool]($inst.sha256 -and $inst.sha256.Length -eq 64 -and (
        ($partEntries.Count -gt 0) -or
        ($inst.download_url -and $inst.download_url.StartsWith('https://'))
    ))

    $entry = [ordered]@{
        id               = $inst.id
        name             = $inst.name
        raw_label        = $inst.raw_label
        category         = $inst.category
        library_id       = $inst.library_id
        install_relative = $inst.install_relative
        file_count       = $inst.file_count
        download_bytes   = $inst.download_bytes
        download_url     = $inst.download_url
        sha256           = $inst.sha256
        included         = [bool]$inst.included
        ready            = $ready
    }
    if ($partEntries.Count -gt 0) {
        $entry.parts = $partEntries
    }
    $published += $entry
}

# Preserva ready de catalogo remoto se esta rodada falhou em algum
try {
    $prevRaw = & curl.exe -s "$PublicBaseUrl/timbre-catalog-v1.json"
    if ($prevRaw -and $prevRaw.Length -gt 50) {
        $prev = $prevRaw | ConvertFrom-Json
        $map = @{}
        for ($i = 0; $i -lt $published.Count; $i++) { $map[$published[$i].id] = $i }
        foreach ($old in @($prev.instruments)) {
            if (-not $old.ready) { continue }
            if (-not $map.ContainsKey($old.id)) { continue }
            $idx = $map[$old.id]
            if (-not $published[$idx].ready -and $old.sha256 -and ("$($old.sha256)".Length -eq 64)) {
                $published[$idx].sha256 = "$($old.sha256)"
                $published[$idx].download_bytes = [int64]$old.download_bytes
                $published[$idx].download_url = "$($old.download_url)"
                $published[$idx].ready = $true
                if ($old.parts) { $published[$idx].parts = $old.parts }
                Write-Host ("  preservado ready: {0}" -f $old.id)
            }
        }
    }
} catch {
    Write-Host "  (merge catalogo remoto ignorado)"
}

$catalog = [ordered]@{
    schema_version = 1
    library_id     = 'church-live'
    library_name   = 'Church Live'
    generated_at   = (Get-Date).ToUniversalTime().ToString('o')
    base_url       = $PublicBaseUrl
    instruments    = $published
}

$json = $catalog | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($catalogPath, $json, [Text.UTF8Encoding]::new($false))
Write-Host "Catalogo: $catalogPath ($($published.Count) instrumentos)"

if ($Upload) {
    Write-Host "Enviando catalogo..."
    Upload-FtpFile $catalogPath "$FtpRemoteDir/timbre-catalog-v1.json" $netCred
    Write-Host "Catalogo publicado em $PublicBaseUrl/timbre-catalog-v1.json"
    $ready = @($published | Where-Object { $_.ready }).Count
    Write-Host "Prontos para download individual: $ready / $($published.Count)"
}

[ChurchDawTimbreFtpPin]::Restore()
