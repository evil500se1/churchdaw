<?php

declare(strict_types=1);

if (PHP_SAPI !== 'cli') {
    http_response_code(404);
    exit;
}

use ChurchDAW\Licensing\Config;
use ChurchDAW\Licensing\Database;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

$config = Config::load($root);
$config->assertRuntimeReady();
$pdo = (new Database($config))->pdo();

$operations = [
    'request_nonces' => "DELETE FROM request_nonces
                         WHERE expires_at < CURRENT_TIMESTAMP(6)",
    'rate_limits' => "DELETE FROM rate_limits
                      WHERE window_started_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 2 DAY)",
    'user_tokens' => "DELETE FROM user_tokens
                      WHERE expires_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 7 DAY)
                         OR used_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 7 DAY)",
    'web_sessions' => "DELETE FROM web_sessions
                       WHERE expires_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 7 DAY)
                          OR revoked_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 7 DAY)",
    'refresh_tokens' => "DELETE FROM refresh_tokens
                         WHERE expires_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 30 DAY)
                            OR revoked_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 30 DAY)",
    'webhook_events' => "DELETE FROM webhook_events
                         WHERE processed_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 90 DAY)",
    'email_jobs' => "DELETE FROM email_jobs
                     WHERE sent_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 7 DAY)
                        OR (
                            status = 'failed' AND attempts >= 10
                            AND created_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 30 DAY)
                        )",
    'audit_events' => "DELETE FROM audit_events
                       WHERE created_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 365 DAY)",
];

foreach ($operations as $name => $sql) {
    $count = $pdo->exec($sql);
    echo strtoupper($name) . '=' . (int) $count . "\n";
}
