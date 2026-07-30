<?php

declare(strict_types=1);

$expectedStatus = isset($argv[1]) ? (int) $argv[1] : 206;
$url = $argv[2]
    ?? 'https://churchdaw.com/api/v1/library/files/'
        . 'church-live-v1-segment-01.zip';
if (!in_array($expectedStatus, [206, 401, 403, 404], true)
    || !filter_var($url, FILTER_VALIDATE_URL)
    || !str_starts_with($url, 'https://churchdaw.com/')) {
    throw new RuntimeException(
        'Uso: php public-library-download-smoke.php STATUS [URL_HTTPS]'
    );
}

$received = 0;
$headers = [];
$curl = curl_init($url);
if ($curl === false) {
    throw new RuntimeException('Falha ao iniciar cURL.');
}
curl_setopt_array($curl, [
    CURLOPT_HTTPHEADER => ['Range: bytes=0-0'],
    CURLOPT_FOLLOWLOCATION => false,
    CURLOPT_CONNECTTIMEOUT => 10,
    CURLOPT_TIMEOUT => 30,
    CURLOPT_SSL_OPTIONS => CURLSSLOPT_NATIVE_CA,
    CURLOPT_HEADERFUNCTION => static function (
        CurlHandle $handle,
        string $header
    ) use (&$headers): int {
        $separator = strpos($header, ':');
        if ($separator !== false) {
            $name = strtolower(trim(substr($header, 0, $separator)));
            $headers[$name] = trim(substr($header, $separator + 1));
        }
        return strlen($header);
    },
    CURLOPT_WRITEFUNCTION => static function (
        CurlHandle $handle,
        string $chunk
    ) use (&$received): int {
        $received += strlen($chunk);
        return $received <= 4096 ? strlen($chunk) : 0;
    },
]);
$result = curl_exec($curl);
$status = (int) curl_getinfo($curl, CURLINFO_RESPONSE_CODE);
$error = curl_error($curl);
$curl = null;

if ($result === false && $status === 0) {
    throw new RuntimeException('Falha de rede no smoke: ' . $error);
}
$rangeOk = $status !== 206
    || ($received === 1
        && ($headers['accept-ranges'] ?? '') === 'bytes'
        && preg_match(
            '/^bytes 0-0\/[1-9][0-9]*$/',
            $headers['content-range'] ?? ''
        ) === 1);
$ok = $status === $expectedStatus && $rangeOk;
echo json_encode([
    'ok' => $ok,
    'expected_status' => $expectedStatus,
    'http_status' => $status,
    'response_bytes' => $received,
    'range_ok' => $rangeOk,
], JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . PHP_EOL;
if (!$ok) {
    exit(1);
}
