<?php

declare(strict_types=1);

use ChurchDAW\Licensing\Config;

if (PHP_SAPI !== 'cli') {
    http_response_code(404);
    exit;
}

if ($argc !== 3) {
    fwrite(STDERR, "Uso: php preflight-release.php RELEASE_ROOT APP_RELEASE\n");
    exit(64);
}

$projectRoot = realpath((string) $argv[1]);
$expectedRelease = (string) $argv[2];
if ($projectRoot === false || !is_dir($projectRoot)) {
    fwrite(STDERR, "Release inexistente.\n");
    exit(66);
}
if (!preg_match('/^[A-Za-z0-9._-]{1,64}$/', $expectedRelease)) {
    fwrite(STDERR, "APP_RELEASE invalida.\n");
    exit(64);
}

$autoload = $projectRoot . '/src/autoload.php';
if (!is_file($autoload)) {
    fwrite(STDERR, "Autoload da release ausente.\n");
    exit(66);
}

try {
    require $autoload;
    $config = Config::load($projectRoot);
    $config->assertRuntimeReady();

    if (!hash_equals($expectedRelease, $config->string('app.release'))) {
        throw new RuntimeException('APP_RELEASE nao corresponde ao diretorio da release.');
    }
    if ($config->bool('checkout.enabled')) {
        throw new RuntimeException(
            'CHECKOUT_ENABLED precisa permanecer 0 durante preflight, migracao e smoke.'
        );
    }

    $assets = [];
    foreach (['app.css', 'app.js'] as $filename) {
        $path = $projectRoot . '/public/account-assets/' . $filename;
        if (!is_file($path) || is_link($path)) {
            throw new RuntimeException("Asset ausente ou inseguro: {$filename}");
        }
        $hash = hash_file('sha256', $path);
        if ($hash === false) {
            throw new RuntimeException("Nao foi possivel calcular o hash de {$filename}.");
        }
        $assets[$filename] = $hash;
    }

    fwrite(STDOUT, json_encode([
        'ok' => true,
        'release' => $expectedRelease,
        'checkout_enabled' => false,
        'assets_sha256' => $assets,
    ], JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR) . PHP_EOL);
} catch (Throwable $error) {
    fwrite(STDERR, 'PREFLIGHT FAILED: ' . $error->getMessage() . PHP_EOL);
    exit(1);
}
