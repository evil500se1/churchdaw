<?php

declare(strict_types=1);

use ChurchDAW\Licensing\Config;
use ChurchDAW\Licensing\Crypto;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

$expectedStatus = isset($argv[1]) ? (int) $argv[1] : 206;
$url = $argv[2]
    ?? 'https://churchdaw.com/api/v1/library/files/'
        . 'church-live-v1-segment-01.zip';
if (!in_array($expectedStatus, [206, 403], true)
    || !filter_var($url, FILTER_VALIDATE_URL)
    || !str_starts_with($url, 'https://churchdaw.com/')) {
    throw new RuntimeException(
        'Uso: php legacy-library-download-smoke.php 206|403 [URL_HTTPS]'
    );
}

$privateKey = getenv('LICENSE_PRIVATE_KEY_PATH');
if ($privateKey === false || !is_file($privateKey)) {
    throw new RuntimeException('LICENSE_PRIVATE_KEY_PATH ausente.');
}
putenv('APP_ENV=test');
putenv('APP_BASE_URL=https://churchdaw.com');
putenv('LICENSE_KEY_ID=churchdaw-2026-01');

$config = Config::load($root);
$crypto = new Crypto($config);
$now = time();
$claims = [
    'iss' => 'https://churchdaw.com',
    'aud' => 'churchdaw-android',
    'sub' => '00000000-0000-4000-8000-000000000011',
    'jti' => Crypto::uuid(),
    'license_id' => '00000000-0000-4000-8000-000000000013',
    'product_code' => 'churchdaw-perpetual',
    'activation_id' => '00000000-0000-4000-8000-000000000014',
    'installation_id' => 'fixture-android-installation-id',
    'platform' => 'android',
    'mode' => 'licensed',
    'iat' => $now,
    'nbf' => $now - 30,
    'lease_expires_at' => $now + 3600,
    'offline_until' => $now + 604800,
];
$token = $crypto->signCompact($claims, 'churchdaw-lease+jwt');

$received = 0;
$curl = curl_init($url);
if ($curl === false) {
    throw new RuntimeException('Falha ao iniciar cURL.');
}
curl_setopt_array($curl, [
    CURLOPT_HTTPHEADER => [
        'Authorization: Bearer ' . $token,
        'Range: bytes=0-0',
    ],
    CURLOPT_FOLLOWLOCATION => false,
    CURLOPT_CONNECTTIMEOUT => 10,
    CURLOPT_TIMEOUT => 30,
    CURLOPT_SSL_OPTIONS => CURLSSLOPT_NATIVE_CA,
    CURLOPT_WRITEFUNCTION => static function (
        CurlHandle $handle,
        string $chunk
    ) use (&$received): int {
        $received += strlen($chunk);
        // O probe pede um byte. Limite respostas inesperadas a 4 KiB.
        return $received <= 4096 ? strlen($chunk) : 0;
    },
]);
$result = curl_exec($curl);
$status = (int) curl_getinfo($curl, CURLINFO_RESPONSE_CODE);
$error = curl_error($curl);
$curl = null;
$token = '';

if ($result === false && $status === 0) {
    throw new RuntimeException('Falha de rede no smoke: ' . $error);
}
echo json_encode([
    'ok' => $status === $expectedStatus,
    'expected_status' => $expectedStatus,
    'http_status' => $status,
    'response_bytes' => $received,
], JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . PHP_EOL;
if ($status !== $expectedStatus) {
    exit(1);
}
