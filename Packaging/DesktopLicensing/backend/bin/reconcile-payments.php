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

$limit = isset($argv[1]) ? (int) $argv[1] : 100;
$stats = $payments->reconcilePayments($limit);
echo "CHECKED {$stats['checked']}\n";
echo "RECONCILED {$stats['reconciled']}\n";
echo "FAILED {$stats['failed']}\n";
if ($stats['failed'] > 0) {
    exit(1);
}
