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

$eventId = (string) ($argv[1] ?? '');
if (!preg_match(
    '/^[a-f0-9]{8}-[a-f0-9]{4}-[1-5][a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/i',
    $eventId
)) {
    fwrite(STDERR, "Uso: php bin/requeue-webhook.php <event-uuid>\n");
    exit(64);
}

$config = Config::load($root);
$config->assertRuntimeReady();
$database = new Database($config);
$statement = $database->pdo()->prepare(
    "UPDATE webhook_events
     SET status = 'failed', attempts = 0,
         next_attempt_at = CURRENT_TIMESTAMP(6),
         processing_started_at = NULL, processed_at = NULL,
         last_error = 'Reenfileirado manualmente'
     WHERE id = :id AND status <> 'processed'"
);
$statement->execute(['id' => $eventId]);
if ($statement->rowCount() !== 1) {
    fwrite(
        STDERR,
        "Evento inexistente ou ja processado; nenhuma alteracao realizada.\n"
    );
    exit(1);
}

echo "REQUEUED {$eventId}\n";
