<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;
use RuntimeException;

final class MigrationState
{
    /**
     * @return array{
     *   ready: bool,
     *   current: string,
     *   expected: string,
     *   applied_count: int,
     *   expected_count: int,
     *   missing: list<string>,
     *   unexpected: list<string>
     * }
     */
    public static function inspect(PDO $pdo, string $projectRoot): array
    {
        $migrationFiles = glob($projectRoot . '/migrations/*.sql');
        if ($migrationFiles === false || $migrationFiles === []) {
            throw new RuntimeException('Nenhuma migration foi encontrada.');
        }
        $expected = array_map(
            static fn (string $path): string => basename($path),
            $migrationFiles
        );
        sort($expected, SORT_STRING);

        $tableExists = $pdo->query(
            "SELECT COUNT(*)
             FROM information_schema.tables
             WHERE table_schema = DATABASE()
               AND table_name = 'schema_migrations'
               AND table_type = 'BASE TABLE'"
        )->fetchColumn();

        $applied = [];
        if ((int) $tableExists === 1) {
            $rows = $pdo->query(
                'SELECT version FROM schema_migrations ORDER BY version'
            )->fetchAll(PDO::FETCH_COLUMN);
            foreach ($rows as $version) {
                if (is_string($version) && $version !== '') {
                    $applied[] = $version;
                }
            }
        }

        return self::compare($expected, $applied);
    }

    /**
     * @param list<string> $expected
     * @param list<string> $applied
     * @return array{
     *   ready: bool,
     *   current: string,
     *   expected: string,
     *   applied_count: int,
     *   expected_count: int,
     *   missing: list<string>,
     *   unexpected: list<string>
     * }
     */
    public static function compare(array $expected, array $applied): array
    {
        $expected = array_values(array_unique($expected));
        $applied = array_values(array_unique($applied));
        sort($expected, SORT_STRING);
        sort($applied, SORT_STRING);

        $missing = array_values(array_diff($expected, $applied));
        $unexpected = array_values(array_diff($applied, $expected));

        return [
            'ready' => $missing === [] && $unexpected === [],
            'current' => $applied === []
                ? ''
                : (string) $applied[array_key_last($applied)],
            'expected' => $expected === []
                ? ''
                : (string) $expected[array_key_last($expected)],
            'applied_count' => count($applied),
            'expected_count' => count($expected),
            'missing' => $missing,
            'unexpected' => $unexpected,
        ];
    }
}
