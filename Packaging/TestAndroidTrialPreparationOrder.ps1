param(
    [string] $ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$mainPath = Join-Path $ProjectRoot 'Source\Main.cpp'
$source = [IO.File]::ReadAllText($mainPath)

function Assert-Match {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Pattern,
        [Parameter(Mandatory = $true)]
        [string] $Message
    )
    if ($source -notmatch $Pattern) {
        throw $Message
    }
}

Assert-Match `
    'panel\.onTrial\s*=\s*\[this\]\s*\{\s*beginAndroidTrialLibraryPreparation\(\);\s*\};' `
    'O botao Preparar biblioteca nao inicia o fluxo local primeiro.'

Assert-Match `
    'void beginAndroidTrialLibraryPreparation\(\)[\s\S]*?androidTrialPreparationPending\s*=\s*true;[\s\S]*?setLibraryAuthorizationToken\s*\(\s*\{\}\s*\);[\s\S]*?beginPostLicenseStartup\(\);' `
    'O preparo local nao foi separado da reserva do trial.'

Assert-Match `
    'if\s*\(androidTrialPreparationPending\)[\s\S]*?androidTrialReserveWaiting\s*=\s*true;[\s\S]*?androidLicenseClient->reserveTrial\(\);' `
    'O trial nao esta sendo reservado depois do preparo da biblioteca.'

Assert-Match `
    'if\s*\(androidTrialReserveWaiting\)[\s\S]*?androidTrialStartWaiting\s*=\s*true;[\s\S]*?androidLicenseClient->startReservedTrial\(\);' `
    'O cronometro nao esta sendo iniciado depois da reserva valida.'

$trialButtonBlock = [regex]::Match(
    $source,
    'panel\.onTrial\s*=\s*\[this\]\s*\{(?<body>[\s\S]*?)\};'
)
if (-not $trialButtonBlock.Success -or
    $trialButtonBlock.Groups['body'].Value -match 'reserveTrial|startReservedTrial') {
    throw 'O clique inicial ainda reserva ou inicia o trial antes da biblioteca.'
}

Write-Output 'ANDROID_TRIAL_PREPARATION_ORDER_OK'
