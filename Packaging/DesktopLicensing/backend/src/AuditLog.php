<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;

final class AuditLog
{
    public function __construct(
        private readonly Database $database,
        private readonly Crypto $crypto
    ) {
    }

    /** @param array<string, mixed> $metadata */
    public function record(
        string $eventType,
        ?Request $request = null,
        ?string $userId = null,
        ?string $licenseId = null,
        array $metadata = []
    ): void {
        $statement = $this->database->pdo()->prepare(
            'INSERT INTO audit_events
                (request_id, actor_user_id, license_id, event_type, ip_hash, metadata_json)
             VALUES
                (:request_id, :actor_user_id, :license_id, :event_type, :ip_hash, :metadata_json)'
        );
        $statement->execute([
            'request_id' => $request?->requestId,
            'actor_user_id' => $userId,
            'license_id' => $licenseId,
            'event_type' => $eventType,
            'ip_hash' => $request === null ? null : $this->crypto->hashIp($request->ip),
            'metadata_json' => $metadata === []
                ? null
                : json_encode($metadata, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR),
        ]);
    }

    /** @param array<string, mixed> $metadata */
    public static function recordWithPdo(
        PDO $pdo,
        string $eventType,
        ?string $requestId,
        ?string $userId,
        ?string $licenseId,
        ?string $ipHash,
        array $metadata = []
    ): void {
        $statement = $pdo->prepare(
            'INSERT INTO audit_events
                (request_id, actor_user_id, license_id, event_type, ip_hash, metadata_json)
             VALUES
                (:request_id, :actor_user_id, :license_id, :event_type, :ip_hash, :metadata_json)'
        );
        $statement->execute([
            'request_id' => $requestId,
            'actor_user_id' => $userId,
            'license_id' => $licenseId,
            'event_type' => $eventType,
            'ip_hash' => $ipHash,
            'metadata_json' => $metadata === []
                ? null
                : json_encode($metadata, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR),
        ]);
    }
}
