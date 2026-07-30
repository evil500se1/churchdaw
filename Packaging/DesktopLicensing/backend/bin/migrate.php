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

$serverVersion = trim((string) $pdo->query('SELECT VERSION()')->fetchColumn());
$isMariaDb = stripos($serverVersion, 'mariadb') !== false;
$versionMatches = [];
preg_match_all(
    '/(?<![0-9])([0-9]+\.[0-9]+\.[0-9]+)(?![0-9])/',
    $serverVersion,
    $versionMatches
);
$detectedVersions = $versionMatches[1] ?? [];
$parsedVersion = $isMariaDb
    ? end($detectedVersions)
    : ($detectedVersions[0] ?? false);
$minimumVersion = $isMariaDb ? '10.6.0' : '5.7.44';
$databaseFamily = $isMariaDb ? 'MariaDB' : 'MySQL';

if (!is_string($parsedVersion)
    || version_compare($parsedVersion, $minimumVersion, '<')) {
    throw new RuntimeException(sprintf(
        'Banco de dados nao suportado: detectado "%s". '
        . 'O ChurchDAW requer MySQL >= 5.7.44 ou MariaDB >= 10.6.',
        $serverVersion !== '' ? $serverVersion : 'versao desconhecida'
    ));
}

echo "DATABASE {$databaseFamily} {$parsedVersion} SUPPORTED\n";
$sessionState = $pdo->query(
    "SELECT @@SESSION.time_zone AS time_zone,
            @@SESSION.sql_mode AS sql_mode"
)->fetch();
if (!is_array($sessionState)
    || (string) ($sessionState['time_zone'] ?? '') !== '+00:00') {
    throw new RuntimeException('A sessao SQL nao esta fixada em UTC.');
}
foreach ([
    'STRICT_TRANS_TABLES',
    'ONLY_FULL_GROUP_BY',
    'ERROR_FOR_DIVISION_BY_ZERO',
    'NO_ZERO_IN_DATE',
    'NO_ZERO_DATE',
    'NO_ENGINE_SUBSTITUTION',
] as $requiredSqlMode) {
    $sqlModes = explode(',', strtoupper((string) ($sessionState['sql_mode'] ?? '')));
    if (!in_array($requiredSqlMode, $sqlModes, true)) {
        throw new RuntimeException("SQL mode obrigatorio ausente: {$requiredSqlMode}");
    }
}
$lockName = 'churchdaw_desktop_licensing_migrations';

$pdo->exec(
    'CREATE TABLE IF NOT EXISTS schema_migrations (
        version VARCHAR(64) PRIMARY KEY,
        applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
     ) ENGINE=InnoDB
       DEFAULT CHARSET=utf8mb4
       COLLATE=utf8mb4_unicode_ci
       ROW_FORMAT=DYNAMIC'
);

$lock = $pdo->prepare('SELECT GET_LOCK(:name, 15)');
$lock->execute(['name' => $lockName]);
if ((int) $lock->fetchColumn() !== 1) {
    throw new RuntimeException('Outra migracao esta em andamento.');
}

try {
    $files = glob($root . '/migrations/*.sql') ?: [];
    sort($files, SORT_STRING);
    foreach ($files as $file) {
        $version = basename($file);
        $check = $pdo->prepare(
            'SELECT COUNT(*) FROM schema_migrations WHERE version = :version'
        );
        $check->execute(['version' => $version]);
        if ((int) $check->fetchColumn() > 0) {
            continue;
        }

        $sql = file_get_contents($file);
        if (!is_string($sql) || trim($sql) === '') {
            throw new RuntimeException("Migracao vazia: {$version}");
        }
        $pdo->exec($sql);
        $pdo->prepare(
            'INSERT INTO schema_migrations (version) VALUES (:version)'
        )->execute(['version' => $version]);
        echo "APPLIED {$version}\n";
    }

    $pdo->prepare(
        "INSERT INTO products (code, name, currency, price_cents, status)
         VALUES (:code, :name, :currency, :price_cents, 'active')
         ON DUPLICATE KEY UPDATE
            name = VALUES(name),
            currency = VALUES(currency),
            price_cents = VALUES(price_cents),
            status = 'active'"
    )->execute([
        'code' => $config->string('product.code'),
        'name' => $config->string('product.name'),
        'currency' => $config->string('product.currency'),
        'price_cents' => $config->int('product.price_cents'),
    ]);
    echo "READY product=" . $config->string('product.code') . "\n";
} finally {
    $release = $pdo->prepare('SELECT RELEASE_LOCK(:name)');
    $release->execute(['name' => $lockName]);
}
