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

$jobId = (string) ($argv[1] ?? '');
if (!preg_match(
    '/^[a-f0-9]{8}-[a-f0-9]{4}-[1-5][a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$/i',
    $jobId
)) {
    fwrite(STDERR, "Uso: php bin/requeue-email.php <job-uuid>\n");
    exit(64);
}

$config = Config::load($root);
$config->assertRuntimeReady();
$database = new Database($config);
$statement = $database->pdo()->prepare(
    "UPDATE email_jobs
     SET status = 'failed', attempts = 0,
         available_at = CURRENT_TIMESTAMP(6),
         processing_started_at = NULL, sent_at = NULL,
         last_error = 'Reenfileirado manualmente'
     WHERE id = :id AND status <> 'sent'"
);
$statement->execute(['id' => $jobId]);
if ($statement->rowCount() !== 1) {
    fwrite(
        STDERR,
        "Job inexistente ou ja enviado; nenhuma alteracao realizada.\n"
    );
    exit(1);
}

echo "REQUEUED {$jobId}\n";
