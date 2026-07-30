<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

interface PaymentProvider
{
    /**
     * @param array<string, mixed> $payload
     * @return array<string, mixed>
     */
    public function createPreference(array $payload): array;

    /** @return array<string, mixed>|null */
    public function findPreferenceByExternalReference(string $externalReference): ?array;

    /** @return array<string, mixed> */
    public function preference(string $preferenceId): array;

    /** @return array<string, mixed> */
    public function payment(string $paymentId): array;

    /** @return array<string, mixed> */
    public function merchantOrder(string $merchantOrderId): array;

    /** @return list<array<string, mixed>> */
    public function merchantOrdersByExternalReference(
        string $externalReference
    ): array;

    /** @return array<string, mixed> */
    public function chargeback(string $chargebackId): array;

    public function verifyWebhookSignature(
        string $signatureHeader,
        string $requestId,
        string $dataId
    ): bool;
}
