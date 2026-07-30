<?php

declare(strict_types=1);

if (PHP_SAPI !== 'cli') {
    http_response_code(404);
    exit;
}

use ChurchDAW\Licensing\Config;
use ChurchDAW\Licensing\Crypto;
use ChurchDAW\Licensing\Database;
use ChurchDAW\Licensing\Mailer;
use ChurchDAW\Licensing\MercadoPagoClient;
use ChurchDAW\Licensing\PaymentService;
use ChurchDAW\Licensing\RateLimiter;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

$config = Config::load($root);
$config->assertRuntimeReady();
$database = new Database($config);
$crypto = new Crypto($config);
$payments = new PaymentService(
    $config,
    $database,
    $crypto,
    new RateLimiter($database, $crypto),
    new MercadoPagoClient($config),
    new Mailer($config, $database, $crypto)
);

$limit = isset($argv[1]) ? (int) $argv[1] : 25;
$processed = $payments->processPendingWebhooks($limit);
echo "PROCESSED {$processed}\n";

$queueState = $database->pdo()->query(
    "SELECT
        SUM(status = 'failed' AND attempts < 10) AS retrying,
        SUM(status = 'failed' AND attempts >= 10) AS dead_letter
     FROM webhook_events"
)->fetch();
$retrying = is_array($queueState) ? (int) $queueState['retrying'] : 0;
$deadLetter = is_array($queueState) ? (int) $queueState['dead_letter'] : 0;
echo "RETRYING {$retrying}\n";
echo "DEAD_LETTER {$deadLetter}\n";
if ($deadLetter > 0) {
    exit(2);
}
if ($retrying > 0) {
    exit(1);
}
