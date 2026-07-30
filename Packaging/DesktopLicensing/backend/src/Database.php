<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;
use Throwable;

final class Database
{
    private PDO $pdo;

    public function __construct(Config $config)
    {
        $initCommandAttribute = defined('Pdo\\Mysql::ATTR_INIT_COMMAND')
            ? constant('Pdo\\Mysql::ATTR_INIT_COMMAND')
            : PDO::MYSQL_ATTR_INIT_COMMAND;
        $this->pdo = new PDO(
            $config->string('db.dsn'),
            $config->string('db.user'),
            $config->string('db.password'),
            [
                PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
                PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
                PDO::ATTR_EMULATE_PREPARES => false,
                $initCommandAttribute => "SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci",
            ]
        );
        $this->pdo->exec("SET SESSION time_zone = '+00:00'");
        $this->pdo->exec(
            "SET SESSION sql_mode = "
            . "'STRICT_TRANS_TABLES,ONLY_FULL_GROUP_BY,"
            . "ERROR_FOR_DIVISION_BY_ZERO,NO_ZERO_IN_DATE,NO_ZERO_DATE,"
            . "NO_ENGINE_SUBSTITUTION'"
        );
    }

    public function pdo(): PDO
    {
        return $this->pdo;
    }

    /** @template T @param callable(PDO): T $operation @return T */
    public function transaction(callable $operation): mixed
    {
        for ($attempt = 0; $attempt < 3; ++$attempt) {
            $this->pdo->beginTransaction();
            try {
                $result = $operation($this->pdo);
                $this->pdo->commit();
                return $result;
            } catch (Throwable $error) {
                if ($this->pdo->inTransaction()) {
                    $this->pdo->rollBack();
                }
                if (!$this->isRetryableTransactionFailure($error)
                    || $attempt >= 2) {
                    throw $error;
                }
                usleep(random_int(20_000, 80_000) * ($attempt + 1));
            }
        }
        throw new \LogicException('Retry transacional terminou sem resultado.');
    }

    private function isRetryableTransactionFailure(Throwable $error): bool
    {
        if (!$error instanceof \PDOException) {
            return false;
        }
        $sqlState = (string) $error->getCode();
        $driverCode = (int) ($error->errorInfo[1] ?? 0);
        return $sqlState === '40001'
            || in_array($driverCode, [1205, 1213], true);
    }
}
