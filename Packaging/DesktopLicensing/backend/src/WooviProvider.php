<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

interface WooviProvider
{
    /** @param array<string, mixed> $payload @return array<string, mixed> */
    public function createCharge(array $payload): array;

    /** @return array<string, mixed> */
    public function charge(string $correlationId): array;

    public function verifyWebhookSignature(string $payload, string $signature): bool;
}
