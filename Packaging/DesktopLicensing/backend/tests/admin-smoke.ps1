param(
    [string] $DatabaseName = 'churchdaw_integration_admin_20260728',
    [int] $Port = 8765
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$phpRoot =
    'C:\Users\Administrator\AppData\Local\Temp\churchdaw-php-8.5.8'
$php = Join-Path $phpRoot 'php.exe'
$mysql =
    'C:\Users\Administrator\AppData\Local\Temp\churchdaw-mysql57-runtime\mysql-5.7.44-winx64\bin\mysql.exe'
$adminPassword = 'LocalAdminSmoke!2026'
$keyBytes = New-Object byte[] 32
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
$rng.GetBytes($keyBytes)
$rng.Dispose()

$env:APP_ENV = 'development'
$env:APP_RELEASE = 'admin-test-1'
$env:APP_BASE_URL = 'https://churchdaw.com'
$env:APP_ENCRYPTION_KEY = [Convert]::ToBase64String($keyBytes)
$env:IP_HASH_KEY = 'test-ip-hash-key-2026-07-28-abcdefghijklmno'
$env:DB_DSN =
    "mysql:host=127.0.0.1;port=33308;dbname=$DatabaseName;charset=utf8mb4"
$env:DB_USER = 'root'
$env:DB_PASSWORD = ''
$env:LICENSE_PRIVATE_KEY_PATH =
    'C:\Users\Administrator\MinhaDAW\.secrets\churchdaw-license-private.pem'
$env:LIBRARY_STORAGE_PATH =
    'C:\Users\Administrator\MinhaDAW\Packaging\AndroidPacks'
$env:PRODUCT_PRICE_CENTS = '59900'
$env:MAIL_ENABLED = '0'
$env:CHECKOUT_ENABLED = '0'
$env:ADMIN_ENABLED = '1'
$env:ADMIN_USERNAME = 'igorteixeira'
$env:ADMIN_PASSWORD_HASH = & $php `
    '-d' "extension_dir=$phpRoot\ext" `
    '-d' 'extension=php_sodium.dll' `
    '-r' "echo password_hash('$adminPassword', PASSWORD_ARGON2ID);"

$phpArgs = @(
    '-d', "extension_dir=$phpRoot\ext",
    '-d', 'extension=php_curl.dll',
    '-d', 'extension=php_mbstring.dll',
    '-d', 'extension=php_openssl.dll',
    '-d', 'extension=php_pdo_mysql.dll',
    '-d', 'extension=php_sodium.dll',
    '-S', "127.0.0.1:$Port",
    'index.php'
)
$outputRoot =
    'C:\Users\Administrator\Documents\Codex\2026-07-27\ac\outputs'
$stdout = Join-Path $outputRoot 'admin-local-server.stdout.log'
$stderr = Join-Path $outputRoot 'admin-local-server.stderr.log'
$process = $null

function Invoke-CurlStatus {
    param([string[]] $Arguments)
    $response = & curl.exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "curl failed with exit code $LASTEXITCODE"
    }
    return $response
}

function Invoke-JsonLogin {
    param([string] $Body)
    $response = $Body | & curl.exe `
        '-sS' '-w' "`n%{http_code}" `
        '-X' 'POST' `
        "http://127.0.0.1:$Port/api/v1/auth/login" `
        '-H' 'Content-Type: application/json' `
        '--data-binary' '@-'
    if ($LASTEXITCODE -ne 0) {
        throw "curl JSON failed with exit code $LASTEXITCODE"
    }
    return $response
}

try {
    & $mysql `
        '--protocol=tcp' '-h' '127.0.0.1' '-P' '33308' `
        '-u' 'root' '-e' `
        "USE $DatabaseName; DELETE FROM rate_limits;"
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not reset local smoke rate limits'
    }

    $process = Start-Process `
        -FilePath $php `
        -ArgumentList $phpArgs `
        -WorkingDirectory (Join-Path $root 'public') `
        -WindowStyle Hidden `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru
    Start-Sleep -Milliseconds 800

    $unauth = Invoke-CurlStatus @(
        '-sS', '-o', 'NUL', '-w', '%{http_code}',
        "http://127.0.0.1:$Port/admin"
    )
    if ($unauth -ne '200') {
        throw "Unauthenticated admin expected 200 login, got $unauth"
    }

    $wrong = Invoke-CurlStatus @(
        '-sS', '-o', 'NUL', '-w', '%{http_code}',
        '-X', 'POST', "http://127.0.0.1:$Port/admin/login",
        '-H', 'Content-Type: application/x-www-form-urlencoded',
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1',
        '--data', 'username=igorteixeira&password=wrong-smoke-password'
    )
    if ($wrong -ne '401') {
        throw "Wrong login expected 401, got $wrong"
    }

    $login = Invoke-CurlStatus @(
        '-sS', '-i', '-X', 'POST',
        "http://127.0.0.1:$Port/admin/login",
        '-H', 'Content-Type: application/x-www-form-urlencoded',
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1',
        '--data',
        ('username=igorteixeira&password=' +
            [Uri]::EscapeDataString($adminPassword))
    )
    $status =
        ($login | Select-String '^HTTP/' | Select-Object -First 1).Line
    $cookie =
        ($login |
            Select-String '^Set-Cookie: __Host-churchdaw_admin=' |
            Select-Object -First 1).Line
    if ($status -notmatch ' 303 ') {
        throw "Correct login expected 303: $status"
    }
    if (($cookie -notmatch 'Secure') -or
        ($cookie -notmatch 'HttpOnly') -or
        ($cookie -notmatch 'SameSite=Strict')) {
        throw 'Admin cookie attributes invalid'
    }
    $token =
        [regex]::Match(
            $cookie,
            '__Host-churchdaw_admin=([^;]+)'
        ).Groups[1].Value
    if ($token -eq '') {
        throw 'Admin cookie token missing'
    }
    $cookieHeader = "Cookie: __Host-churchdaw_admin=$token"

    $dashboard = Invoke-CurlStatus @(
        '-sS', "http://127.0.0.1:$Port/admin",
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1'
    )
    $csrf = [Net.WebUtility]::HtmlDecode(
        [regex]::Match(
            $dashboard,
            'name="csrf_token" value="([^"]+)"'
        ).Groups[1].Value
    )
    if ($csrf -eq '') {
        throw 'CSRF token missing'
    }

    $missingCsrf = Invoke-CurlStatus @(
        '-sS', '-o', 'NUL', '-w', '%{http_code}',
        '-X', 'POST',
        "http://127.0.0.1:$Port/admin/licenses/issue",
        '-H', 'Content-Type: application/x-www-form-urlencoded',
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1',
        '--data',
        'display_name=Smoke&email=smoke.invalid%40example.invalid'
    )
    if ($missingCsrf -ne '403') {
        throw "Missing CSRF expected 403, got $missingCsrf"
    }

    $stamp = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $email = "smoke.$stamp@example.invalid"
    $customerPassword = '7'
    $issueData =
        'csrf_token=' + [Uri]::EscapeDataString($csrf) +
        '&display_name=' + [Uri]::EscapeDataString('Cliente Smoke') +
        '&email=' + [Uri]::EscapeDataString($email) +
        '&customer_password=' + [Uri]::EscapeDataString($customerPassword) +
        '&customer_password_confirmation=' +
            [Uri]::EscapeDataString($customerPassword)
    $issue = Invoke-CurlStatus @(
        '-sS', '-o', 'NUL', '-w', '%{http_code}',
        '-X', 'POST',
        "http://127.0.0.1:$Port/admin/licenses/issue",
        '-H', 'Content-Type: application/x-www-form-urlencoded',
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1',
        '--data', $issueData
    )
    if ($issue -ne '303') {
        throw "License issue expected 303, got $issue"
    }

    $dashboard = Invoke-CurlStatus @(
        '-sS', "http://127.0.0.1:$Port/admin",
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1'
    )
    if (($dashboard -notmatch [regex]::Escape($email)) -or
        ($dashboard -notmatch 'name="customer_password"') -or
        ($dashboard -notmatch 'minlength="8"') -or
        ($dashboard -notmatch 'id="admin-search"') -or
        ($dashboard -notmatch 'id="admin-status-filter"')) {
        throw 'Issued customer or chosen-password form missing'
    }

    $windowsSecret = [Convert]::ToBase64String([byte[]] (1..32)).
        TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $windowsBody = @{
        email = $email
        password = $customerPassword
        installation_id = 'windows-smoke-install-0001'
        device_secret = $windowsSecret
        device_public_key = ''
        device_fingerprint = 'win-smoke-fingerprint'
        device_label = 'Windows Smoke'
        platform = 'windows'
    } | ConvertTo-Json -Compress
    $windowsLogin = Invoke-JsonLogin $windowsBody
    if ($windowsLogin[-1] -ne '200') {
        throw "Windows activation failed: $($windowsLogin -join ' ')"
    }

    $macSecret = [Convert]::ToBase64String([byte[]] (33..64)).
        TrimEnd('=').Replace('+', '-').Replace('/', '_')
    $macBody = @{
        email = $email
        password = $customerPassword
        installation_id = 'macos-smoke-install-000001'
        device_secret = $macSecret
        device_public_key = ''
        device_fingerprint = 'mac-smoke-fingerprint'
        device_label = 'macOS Smoke'
        platform = 'macos'
    } | ConvertTo-Json -Compress
    $macLogin = Invoke-JsonLogin $macBody
    if ($macLogin[-1] -ne '200') {
        throw "macOS activation failed: $($macLogin -join ' ')"
    }

    $windowsExtraBody = @{
        email = $email
        password = $customerPassword
        installation_id = 'windows-smoke-install-0002'
        device_secret = $macSecret
        device_public_key = ''
        device_fingerprint = 'win-smoke-fingerprint-extra'
        device_label = 'Segundo Windows Smoke'
        platform = 'windows'
    } | ConvertTo-Json -Compress
    $windowsBlocked = Invoke-JsonLogin $windowsExtraBody
    if (($windowsBlocked[-1] -ne '409') -or
        (($windowsBlocked -join "`n") -notmatch 'ACTIVATION_LIMIT')) {
        throw 'Second Windows was not blocked'
    }

    $dashboard = Invoke-CurlStatus @(
        '-sS', "http://127.0.0.1:$Port/admin",
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1'
    )
    $deviceId =
        [regex]::Match(
            $dashboard,
            '/admin/devices/([0-9a-f-]{36})/deactivate'
        ).Groups[1].Value
    if ($deviceId -eq '') {
        throw 'Activated device missing from dashboard'
    }
    $deactivate = Invoke-CurlStatus @(
        '-sS', '-o', 'NUL', '-w', '%{http_code}',
        '-X', 'POST',
        "http://127.0.0.1:$Port/admin/devices/$deviceId/deactivate",
        '-H', 'Content-Type: application/x-www-form-urlencoded',
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1',
        '--data',
        ('csrf_token=' + [Uri]::EscapeDataString($csrf))
    )
    if ($deactivate -ne '303') {
        throw "Device transfer expected 303, got $deactivate"
    }

    $windowsExtraLogin = Invoke-JsonLogin $windowsExtraBody
    if ($windowsExtraLogin[-1] -ne '200') {
        throw "Windows activation after releasing its slot failed: $($windowsExtraLogin[-1])"
    }

    $dashboard = Invoke-CurlStatus @(
        '-sS', "http://127.0.0.1:$Port/admin",
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1'
    )
    $licenseId =
        [regex]::Match(
            $dashboard,
            '/admin/licenses/([0-9a-f-]{36})/revoke'
        ).Groups[1].Value
    if ($licenseId -eq '') {
        throw 'Active license missing from dashboard'
    }
    $revokeData =
        'csrf_token=' + [Uri]::EscapeDataString($csrf) +
        '&reason=' +
        [Uri]::EscapeDataString('Smoke test concluído')
    $revoke = Invoke-CurlStatus @(
        '-sS', '-o', 'NUL', '-w', '%{http_code}',
        '-X', 'POST',
        "http://127.0.0.1:$Port/admin/licenses/$licenseId/revoke",
        '-H', 'Content-Type: application/x-www-form-urlencoded',
        '-H', $cookieHeader,
        '-H', 'User-Agent: ChurchDAW-Admin-Smoke/1',
        '--data', $revokeData
    )
    if ($revoke -ne '303') {
        throw "License revoke expected 303, got $revoke"
    }

    $afterRevoke = Invoke-JsonLogin $macBody
    if (($afterRevoke[-1] -ne '403') -or
        (($afterRevoke -join "`n") -notmatch 'NO_LICENSE')) {
        throw 'Revoked license was still accepted'
    }

    $sql =
        "USE $DatabaseName; " +
        "SELECT CONCAT('revoked_licenses=',SUM(status='revoked')) " +
        "FROM licenses WHERE id='$licenseId'; " +
        "SELECT CONCAT('admin_events=',COUNT(*)) " +
        "FROM audit_events WHERE event_type LIKE 'admin.%'; " +
        "SELECT CONCAT('active_activations=',COUNT(*)) " +
        "FROM activations " +
        "WHERE license_id='$licenseId' AND status='active';"
    $databaseResult = & $mysql `
        '--protocol=tcp' '-h' '127.0.0.1' '-P' '33308' `
        '-u' 'root' '-N' '-e' $sql
    if ($LASTEXITCODE -ne 0) {
        throw 'Database assertions failed'
    }

    'ADMIN_HTTP_SMOKE_OK'
    'wrong_login=401 secure_cookie=yes csrf_rejected=403 issue=303'
    'windows=200 macos=200 second_windows=409 release_slot=303 replacement_windows=200'
    'revoke=303 post_revoke=403'
    $databaseResult
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
    }
}
