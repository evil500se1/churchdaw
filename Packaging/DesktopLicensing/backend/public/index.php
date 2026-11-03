<?php

declare(strict_types=1);

use ChurchDAW\Licensing\App;

$requestPath = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
if (PHP_SAPI === 'cli-server' && is_string($requestPath)) {
    $assetRoot = realpath(__DIR__ . '/account-assets');
    $assetPath = realpath(__DIR__ . $requestPath);
    if (is_string($assetRoot)
        && is_string($assetPath)
        && str_starts_with($assetPath, $assetRoot . DIRECTORY_SEPARATOR)
        && preg_match('/\.(?:css|js)$/', $assetPath) === 1) {
        return false;
    }
}

$projectRoot = dirname(__DIR__);
require $projectRoot . '/src/autoload.php';

(new App($projectRoot))->run();
