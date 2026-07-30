<?php

declare(strict_types=1);

use ChurchDAW\Licensing\Config;
use ChurchDAW\Licensing\Crypto;
use ChurchDAW\Licensing\Database;
use ChurchDAW\Licensing\RateLimiter;
use ChurchDAW\Licensing\Request;
use ChurchDAW\Licensing\TrialService;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

function trialExpect(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException('FAIL: ' . $message);
    }
}

$config = Config::load($root);
$database = new Database($config);
$crypto = new Crypto($config);
$trials = new TrialService(
    $config,
    $database,
    $crypto,
    new RateLimiter($database, $crypto)
);

$private = openssl_pkey_new([
    'private_key_type' => OPENSSL_KEYTYPE_EC,
    'curve_name' => 'prime256v1',
]);
trialExpect(
    $private instanceof OpenSSLAsymmetricKey,
    'chave EC P-256 criada'
);
$details = openssl_pkey_get_details($private);
trialExpect(
    is_array($details) && is_string($details['key'] ?? null),
    'chave publica EC criada'
);
$encoded = preg_replace(
    '/-----BEGIN PUBLIC KEY-----|-----END PUBLIC KEY-----|\s+/',
    '',
    (string) $details['key']
);
$der = is_string($encoded) ? base64_decode($encoded, true) : false;
trialExpect(is_string($der), 'DER EC criado');

$suffix = bin2hex(random_bytes(8));
$installationId = 'trial-idempotent-' . $suffix;
$request = Request::fromValues(
    'POST',
    '/api/v1/trial/reserve',
    [],
    [
        'installation_id' => $installationId,
        'device_public_key' => Crypto::base64UrlEncode($der),
        'device_fingerprint' => 'android-id:' . $suffix . ':tank3',
        'device_label' => 'TANK3 / Android',
    ],
    '127.0.0.90'
);

$tokens = [];
for ($attempt = 0; $attempt < 16; ++$attempt) {
    $reservation = $trials->reserve($request);
    trialExpect(
        ($reservation['mode'] ?? '') === 'trial_setup',
        'reserva repetida permaneceu em preparacao'
    );
    $tokens[] = (string) ($reservation['setup_token'] ?? '');
}

trialExpect(
    count(array_unique($tokens)) === 16
        && !in_array('', $tokens, true),
    'cada repeticao renovou um token de preparacao'
);

$rowCount = $database->pdo()->prepare(
    'SELECT COUNT(*) FROM android_trials WHERE installation_id = :id'
);
$rowCount->execute(['id' => $installationId]);
trialExpect(
    (int) $rowCount->fetchColumn() === 1,
    'repeticoes preservaram uma unica demonstracao'
);

echo "PASS: reserva Android idempotente em 16 repeticoes\n";
