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
use ChurchDAW\Licensing\WooviClient;
use ChurchDAW\Licensing\WooviPaymentService;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

$config = Config::load($root);
$config->assertRuntimeReady();
$database = new Database($config);
$crypto = new Crypto($config);
$limiter = new RateLimiter($database, $crypto);
$mailer = new Mailer($config, $database, $crypto);
$payments = new PaymentService(
    $config,
    $database,
    $crypto,
    $limiter,
    new MercadoPagoClient($config),
    $mailer
);
$woovi = new WooviPaymentService(
    $config,
    $database,
    $crypto,
    $limiter,
    new WooviClient($config),
    $mailer
);

$limit = isset($argv[1]) ? (int) $argv[1] : 100;
$stats = $payments->reconcilePayments($limit);
$wooviStats = $woovi->reconcilePending($limit);
$stats = [
    'checked' => $stats['checked'] + $wooviStats['checked'],
    'reconciled' => $stats['reconciled'] + $wooviStats['reconciled'],
    'failed' => $stats['failed'] + $wooviStats['failed'],
];
echo "CHECKED {$stats['checked']}\n";
echo "RECONCILED {$stats['reconciled']}\n";
echo "FAILED {$stats['failed']}\n";
if ($stats['failed'] > 0) {
    exit(1);
}
