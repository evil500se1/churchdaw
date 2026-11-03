# Rebuilds timbre-catalog-v1.json from files actually present on the FTP/HTTP server.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$cred = Import-Clixml "$env:USERPROFILE\ChurchDAW-Secrets\churchdaw-ftp.credential.xml"
$net = New-Object System.Net.NetworkCredential(
    $cred.UserName,
    $cred.GetNetworkCredential().Password)

Add-Type -TypeDefinition @'
using System;
using System.Net;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
public static class ChurchDawPinRebuild {
    public static void Install() {
        ServicePointManager.ServerCertificateValidationCallback = (s, c, ch, e) => {
            using (var sha = SHA256.Create()) {
                var a = BitConverter.ToString(sha.ComputeHash(c.GetRawCertData())).Replace("-", "");
                return a.Equals("490A628ED4151A5CAB3FCBACB17C0DE25199CD902B3FD594FEF6201321107F8C",
                    StringComparison.Ordinal);
            }
        };
    }
}
'@
[ChurchDawPinRebuild]::Install()

function Get-FtpList([string]$path) {
    $req = [Net.FtpWebRequest]::Create([Uri]("ftp://churchdaw.com$path"))
    $req.Method = [Net.WebRequestMethods+Ftp]::ListDirectory
    $req.Credentials = $net
    $req.EnableSsl = $true
    $req.UsePassive = $true
    $resp = $req.GetResponse()
    $sr = New-Object IO.StreamReader($resp.GetResponseStream())
    $text = $sr.ReadToEnd()
    $resp.Close()
    return @($text -split "[\r\n]+" | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and $_ -ne '.' -and $_ -ne '..' })
}

function Get-RemoteHash([string]$url) {
    $tmp = Join-Path $env:TEMP ("cdaw-hash-" + [Guid]::NewGuid().ToString("N") + ".bin")
    & curl.exe -s -o $tmp $url
    if (-not (Test-Path $tmp)) { return $null }
    $hash = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
    $len = [int64](Get-Item -LiteralPath $tmp).Length
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    return @{ sha = $hash; bytes = $len }
}

$files = Get-FtpList "/app/android/timbres/"
Write-Host ("Online files: {0}" -f $files.Count)

& powershell -NoProfile -ExecutionPolicy Bypass -File ".\BuildTimbreCatalog.ps1" | Out-Null
$j = Get-Content ".\TimbrePacks\timbre-catalog-v1.json" -Raw | ConvertFrom-Json
$ready = @{}

foreach ($inst in $j.instruments) {
    $id = $inst.id
    $zip = "$id-v1.zip"
    $p1 = "$zip.part01"

    if ($files -contains $zip) {
        Write-Host ("hash {0}..." -f $zip)
        $info = Get-RemoteHash ("https://churchdaw.com/app/android/timbres/" + $zip)
        if ($null -ne $info) {
            $ready[$id] = @{
                sha   = $info.sha
                bytes = $info.bytes
                url   = "https://churchdaw.com/app/android/timbres/$zip"
                parts = @()
            }
            Write-Host ("  OK single {0} {1:N1} MB" -f $id, ($info.bytes / 1MB))
        }
        continue
    }

    if ($files -contains $p1) {
        $parts = @()
        $sum = [int64]0
        for ($n = 1; $n -le 20; $n++) {
            $pn = "{0}.part{1:D2}" -f $zip, $n
            if ($files -notcontains $pn) { break }
            $url = "https://churchdaw.com/app/android/timbres/$pn"
            Write-Host ("hash {0}..." -f $pn)
            $info = Get-RemoteHash $url
            if ($null -eq $info) { throw "Falha hash $pn" }
            $parts += [ordered]@{
                download_url   = $url
                sha256         = $info.sha
                download_bytes = $info.bytes
            }
            $sum += $info.bytes
        }

        Write-Host ("reassemble {0}..." -f $id)
        $full = Join-Path $env:TEMP ("cdaw-full-" + $id + ".zip")
        $fout = [IO.File]::Create($full)
        try {
            foreach ($p in $parts) {
                $partTmp = Join-Path $env:TEMP "cdaw-part-re.bin"
                & curl.exe -s -o $partTmp $p.download_url
                $buf = [IO.File]::ReadAllBytes($partTmp)
                $fout.Write($buf, 0, $buf.Length)
            }
        } finally {
            $fout.Close()
        }
        $fh = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
        $fb = [int64](Get-Item -LiteralPath $full).Length
        Remove-Item -LiteralPath $full -Force -ErrorAction SilentlyContinue

        if ($fh -and $fb -eq $sum) {
            $ready[$id] = @{
                sha   = $fh
                bytes = $fb
                url   = ""
                parts = $parts
            }
            Write-Host ("  OK multi {0} parts={1}" -f $id, $parts.Count)
        }
    }
}

Write-Host ("READY TOTAL: {0}" -f $ready.Count)

# Build JSON
$instrumentsJson = New-Object System.Collections.Generic.List[string]
foreach ($inst in $j.instruments) {
    $r = $ready[$inst.id]
    $isReady = $null -ne $r
    $sha = if ($isReady) { $r.sha } else { "" }
    $bytes = if ($isReady) { $r.bytes } else { [int64]$inst.download_bytes }
    $url = if ($isReady -and $r.url) { $r.url }
           elseif ($isReady) { "" }
           else { "https://churchdaw.com/app/android/timbres/$($inst.id)-v1.zip" }

    $name = ($inst.name -replace '\\', '\\' -replace '"', '\"')
    $raw  = ($inst.raw_label -replace '\\', '\\' -replace '"', '\"')
    $obj = '{' +
        '"id":"' + $inst.id + '",' +
        '"name":"' + $name + '",' +
        '"raw_label":"' + $raw + '",' +
        '"category":"' + $inst.category + '",' +
        '"library_id":"church-live",' +
        '"install_relative":"' + $inst.install_relative + '",' +
        '"file_count":' + [int]$inst.file_count + ',' +
        '"download_bytes":' + $bytes + ',' +
        '"download_url":"' + $url + '",' +
        '"sha256":"' + $sha + '",' +
        '"included":true,' +
        '"ready":' + ($(if ($isReady) { 'true' } else { 'false' }))

    if ($isReady -and $r.parts -and $r.parts.Count -gt 0) {
        $obj += ',"parts":['
        $pi = 0
        foreach ($p in $r.parts) {
            $pi++
            $obj += '{"download_url":"' + $p.download_url +
                    '","sha256":"' + $p.sha256 +
                    '","download_bytes":' + $p.download_bytes + '}'
            if ($pi -lt $r.parts.Count) { $obj += ',' }
        }
        $obj += ']'
    }
    $obj += '}'
    $instrumentsJson.Add($obj) | Out-Null
}

$json =
@'
{
  "schema_version": 1,
  "library_id": "church-live",
  "library_name": "Church Live",
  "generated_at": "GENERATED_AT",
  "base_url": "https://churchdaw.com/app/android/timbres",
  "instruments": [
INSTRUMENTS
  ]
}
'@
$json = $json.Replace('GENERATED_AT', (Get-Date).ToUniversalTime().ToString('o'))
$json = $json.Replace('INSTRUMENTS', ($instrumentsJson -join ",`n"))

$catPath = Join-Path $PSScriptRoot "TimbrePacks\timbre-catalog-v1.json"
[IO.File]::WriteAllText($catPath, $json, [Text.UTF8Encoding]::new($false))

$req = [Net.FtpWebRequest]::Create([Uri]"ftp://churchdaw.com/app/android/timbres/timbre-catalog-v1.json")
$req.Method = [Net.WebRequestMethods+Ftp]::UploadFile
$req.Credentials = $net
$req.EnableSsl = $true
$req.UseBinary = $true
$req.UsePassive = $true
$bytes = [Text.Encoding]::UTF8.GetBytes($json)
$req.ContentLength = $bytes.Length
$stream = $req.GetRequestStream()
$stream.Write($bytes, 0, $bytes.Length)
$stream.Close()
$resp = $req.GetResponse()
$resp.Close()

Write-Host ("PUBLISHED ready={0}" -f $ready.Count)
$ready.Keys | Sort-Object | ForEach-Object { Write-Host ("  " + $_) }
