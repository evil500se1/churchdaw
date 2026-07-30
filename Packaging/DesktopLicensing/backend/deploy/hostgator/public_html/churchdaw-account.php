<?php

declare(strict_types=1);

use ChurchDAW\Licensing\App;

// Este arquivo estavel fica em /home2/USUARIO/public_html. O backend, a
// configuracao, as migrations e as chaves ficam fora da DocumentRoot.
ini_set('display_errors', '0');

$accountUnavailable = static function (): never {
    http_response_code(503);
    header_remove('X-Powered-By');
    header('Content-Type: text/plain; charset=utf-8');
    header('Cache-Control: no-store, max-age=0');
    header('X-Content-Type-Options: nosniff');
    header('X-Frame-Options: DENY');
    header('Referrer-Policy: no-referrer');
    header('Permissions-Policy: camera=(), microphone=(), geolocation=(), payment=()');
    header("Content-Security-Policy: default-src 'none'; frame-ancestors 'none'");
    header('Strict-Transport-Security: max-age=31536000; includeSubDomains');
    echo 'Servico de conta temporariamente indisponivel.';
    exit;
};

$accountHome = dirname(__DIR__) . '/churchdaw';
$releasesRoot = realpath($accountHome . '/releases');
$projectRoot = realpath($accountHome . '/current');

if ($releasesRoot === false
    || $projectRoot === false
    || !str_starts_with(
        $projectRoot . DIRECTORY_SEPARATOR,
        $releasesRoot . DIRECTORY_SEPARATOR
    )
    || !is_file($projectRoot . '/src/autoload.php')) {
    error_log('ChurchDAW deployment unavailable: invalid current release.');
    $accountUnavailable();
}

try {
    require $projectRoot . '/src/autoload.php';
    (new App($projectRoot))->run();
} catch (Throwable $error) {
    error_log(sprintf(
        'ChurchDAW bootstrap failed: %s at %s:%d',
        $error->getMessage(),
        $error->getFile(),
        $error->getLine()
    ));
    $accountUnavailable();
}
