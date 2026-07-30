$ErrorActionPreference = 'Stop'

$allowedRoot =
    'C:\Users\Administrator\AppData\Local\Temp\ChurchDAW-x86-799dc56e69b2401480494dc365588875\'
$target = [IO.Path]::GetFullPath((Join-Path $allowedRoot 'Builds'))

if (-not $target.StartsWith(
        $allowedRoot,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Alvo fora do temporario permitido: $target"
}

if (-not (Test-Path -LiteralPath $target -PathType Container)) {
    Write-Output "Temporario regeneravel ja ausente: $target"
    exit 0
}

Remove-Item -LiteralPath $target -Recurse -Force
Write-Output "Removido build temporario regeneravel: $target"
