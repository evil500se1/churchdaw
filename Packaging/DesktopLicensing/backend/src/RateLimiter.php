<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;

final class RateLimiter
{
    public function __construct(
        private readonly Database $database,
        private readonly Crypto $crypto
    ) {
    }

    public function assertAllowed(
        string $bucket,
        string $dimension,
        int $maximumAttempts,
        int $windowSeconds
    ): void {
        $keyHash = hash('sha256', $bucket . '|' . $dimension);
        $now = time();
        $windowStart = $now - ($now % $windowSeconds);

        $attempts = $this->database->transaction(function (PDO $pdo) use (
            $bucket,
            $keyHash,
            $windowStart
        ): int {
            $insert = $pdo->prepare(
                'INSERT INTO rate_limits (bucket, key_hash, window_started_at, attempts)
                 VALUES (:bucket, :key_hash, FROM_UNIXTIME(:window_start), 1)
                 ON DUPLICATE KEY UPDATE attempts = attempts + 1'
            );
            $insert->execute([
                'bucket' => $bucket,
                'key_hash' => $keyHash,
                'window_start' => $windowStart,
            ]);

            $select = $pdo->prepare(
                'SELECT attempts FROM rate_limits
                 WHERE bucket = :bucket AND key_hash = :key_hash
                   AND window_started_at = FROM_UNIXTIME(:window_start)'
            );
            $select->execute([
                'bucket' => $bucket,
                'key_hash' => $keyHash,
                'window_start' => $windowStart,
            ]);
            return (int) $select->fetchColumn();
        });

        if ($attempts > $maximumAttempts) {
            throw new ApiException(429, 'RATE_LIMITED', 'Muitas tentativas. Aguarde e tente novamente.');
        }
    }

    public function ipDimension(string $ip): string
    {
        return $this->crypto->hashIp($ip);
    }
}
