<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;
use PDOException;

final class PaymentService
{
    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto,
        private readonly RateLimiter $rateLimiter,
        private readonly PaymentProvider $mercadoPago,
        private readonly Mailer $mailer
    ) {
    }

    /** @return array<string, mixed> */
    public function startCheckout(Request $request): array
    {
        $this->rateLimiter->assertAllowed(
            'checkout-ip',
            $this->rateLimiter->ipDimension($request->ip),
            8,
            3600
        );
        $idempotencyKey = $request->header('idempotency-key');
        if (!preg_match('/^[A-Za-z0-9._:-]{16,128}$/', $idempotencyKey)) {
            throw new ApiException(
                400,
                'IDEMPOTENCY_KEY_REQUIRED',
                'Atualize a pagina e tente novamente.'
            );
        }
        if (!$request->bool('accept_terms')) {
            throw new ApiException(422, 'TERMS_REQUIRED', 'Aceite os termos para continuar.');
        }

        $email = AuthService::normaliseEmail($request->string('email', 254));
        $this->rateLimiter->assertAllowed(
            'checkout-email',
            hash('sha256', $email),
            10,
            900
        );
        $displayName = preg_replace('/\s+/', ' ', $request->string('display_name', 96)) ?? '';
        $password = $request->string('password', 128);
        if (mb_strlen($displayName) < 2) {
            throw new ApiException(422, 'INVALID_NAME', 'Informe seu nome.');
        }
        try {
            PasswordPolicy::assertValid($password, $email);
            $checkoutClaimToken = Crypto::uuid();
            $account = $this->prepareAccountAndOrder(
                $email,
                $displayName,
                $password,
                $idempotencyKey,
                $checkoutClaimToken,
                $request
            );
        } finally {
            sodium_memzero($password);
        }

        if (($account['existing_checkout_url'] ?? '') !== '') {
            return [
                'order_id' => $account['order_id'],
                'checkout_url' => $account['existing_checkout_url'],
                'email_verification_sent' => false,
            ];
        }
        if (!($account['owns_checkout_claim'] ?? false)) {
            throw new ApiException(
                409,
                'CHECKOUT_IN_PROGRESS',
                'O checkout desta conta ja esta sendo preparado. Aguarde alguns segundos.'
            );
        }

        $orderId = (string) $account['order_id'];
        $productCode = (string) $account['product_code'];
        $currency = (string) $account['currency'];
        $expectedAmountCents = (int) $account['expected_amount_cents'];
        $expirationMinutes = $this->config->int('checkout.expiration_minutes');
        $now = new \DateTimeImmutable('now', new \DateTimeZone('UTC'));
        $expirationFrom = $now->modify('-5 minutes');
        $expirationTo = $now->modify("+{$expirationMinutes} minutes");
        $providerOutcomeAmbiguous =
            !($account['may_create_preference'] ?? false);
        $preferenceKnownToExist = false;
        try {
            $preferencePayload = [
                'items' => [[
                    'id' => $productCode,
                    'title' => $this->config->string('product.name'),
                    'description' => 'Licenca desktop para um computador ativo',
                    'currency_id' => $currency,
                    'quantity' => 1,
                    'unit_price' => Money::fromCents($expectedAmountCents),
                ]],
                'payer' => [
                    'name' => $displayName,
                    'email' => $email,
                ],
                'external_reference' => $orderId,
                'statement_descriptor' => 'CHURCHDAW',
                'binary_mode' => false,
                'auto_return' => 'approved',
                'expires' => true,
                'expiration_date_from' => $expirationFrom->format('Y-m-d\TH:i:s.vP'),
                'expiration_date_to' => $expirationTo->format('Y-m-d\TH:i:s.vP'),
                'back_urls' => [
                    'success' => $this->config->string('app.base_url') . '/compra/sucesso',
                    'pending' => $this->config->string('app.base_url') . '/compra/pendente',
                    'failure' => $this->config->string('app.base_url') . '/compra/falhou',
                ],
            ];
            $preference = null;
            if ($account['recover_preference'] ?? false) {
                $preference = $this->mercadoPago
                    ->findPreferenceByExternalReference($orderId);
                $preferenceKnownToExist = $preference !== null;
            }
            if ($preference === null) {
                if (!($account['may_create_preference'] ?? false)) {
                    $providerOutcomeAmbiguous = true;
                    throw new ApiException(
                        503,
                        'CHECKOUT_RECONCILIATION_REQUIRED',
                        'A compra anterior ainda esta sendo conferida. Aguarde e tente novamente.'
                    );
                }
                try {
                    $preference = $this->mercadoPago
                        ->createPreference($preferencePayload);
                    $preferenceKnownToExist = true;
                } catch (\Throwable $providerError) {
                    $operationMayHaveSucceeded = !($providerError
                        instanceof PaymentProviderException)
                        || $providerError->operationMayHaveSucceeded;
                    try {
                        $preference = $this->mercadoPago
                            ->findPreferenceByExternalReference($orderId);
                    } catch (\Throwable $searchError) {
                        $providerOutcomeAmbiguous = $operationMayHaveSucceeded;
                        throw $providerError;
                    }
                    if ($preference === null) {
                        if ($operationMayHaveSucceeded) {
                            $providerOutcomeAmbiguous = true;
                            throw new ApiException(
                                503,
                                'CHECKOUT_RECONCILIATION_REQUIRED',
                                'O provedor pode ter criado a compra. Ela sera conferida antes de uma nova tentativa.'
                            );
                        }
                        throw $providerError;
                    }
                    $preferenceKnownToExist = true;
                }
            }
            $this->assertPreferenceMatchesOrder(
                $preference,
                $orderId,
                $productCode,
                $currency,
                $expectedAmountCents
            );

            $preferenceId = (string) ($preference['id'] ?? '');
            $checkoutUrl = (string) (
                $this->config->bool('mercado_pago.sandbox')
                    ? ($preference['sandbox_init_point'] ?? '')
                    : ($preference['init_point'] ?? '')
            );
            if ($preferenceId === '' || !str_starts_with($checkoutUrl, 'https://')) {
                throw new \RuntimeException('Preferencia de pagamento incompleta.');
            }

            $persist = $this->database->pdo()->prepare(
                "UPDATE orders
                 SET provider_preference_id = :preference_id,
                     checkout_url = :checkout_url, status = 'pending',
                     checkout_expires_at = :checkout_expires_at,
                     checkout_claim_token = NULL, checkout_claimed_at = NULL
                 WHERE id = :id
                   AND status = 'creating'
                   AND checkout_claim_token = :checkout_claim_token"
            );
            $persist->execute([
                'preference_id' => $preferenceId,
                'checkout_url' => $checkoutUrl,
                'checkout_expires_at' => $expirationTo->format('Y-m-d H:i:s.u'),
                'checkout_claim_token' => $checkoutClaimToken,
                'id' => $orderId,
            ]);
            if ($persist->rowCount() !== 1) {
                throw new \RuntimeException(
                    'A posse atomica do checkout foi perdida antes de persistir a preferencia.'
                );
            }
        } catch (\Throwable $error) {
            $failureStatus = $providerOutcomeAmbiguous
                || $preferenceKnownToExist
                ? 'provider_ambiguous'
                : 'provider_error';
            $this->database->pdo()->prepare(
                "UPDATE orders
                 SET status = :status,
                     checkout_claim_token = NULL, checkout_claimed_at = NULL
                 WHERE id = :id AND checkout_claim_token = :checkout_claim_token"
            )->execute([
                'status' => $failureStatus,
                'id' => $orderId,
                'checkout_claim_token' => $checkoutClaimToken,
            ]);
            throw $error;
        }

        if (($account['verification_token'] ?? '') !== '') {
            try {
                $this->mailer->sendVerification(
                    $email,
                    $displayName,
                    (string) $account['verification_token']
                );
            } catch (\Throwable) {
                error_log('ChurchDAW: falha ao enviar verificacao para user ' . $account['user_id']);
            }
        }

        return [
            'order_id' => $orderId,
            'checkout_url' => $checkoutUrl,
            'email_verification_sent' => ($account['verification_token'] ?? '') !== '',
        ];
    }

    public function receiveMercadoPagoWebhook(
        Request $request,
        string $dataId
    ): void {
        $signature = $request->header('x-signature');
        $providerRequestId = $request->header('x-request-id');
        if (!$this->mercadoPago->verifyWebhookSignature(
            $signature,
            $providerRequestId,
            $dataId
        )) {
            throw new ApiException(401, 'INVALID_WEBHOOK_SIGNATURE', 'Assinatura invalida.');
        }

        $type = (string) ($request->body['type'] ?? '');
        $bodyData = $request->body['data'] ?? null;
        $bodyDataId = is_array($bodyData)
            ? (string) ($bodyData['id'] ?? '')
            : '';
        if (!in_array($type, ['payment', 'topic_chargebacks_wh'], true)
            || !preg_match('/^[A-Za-z0-9._:-]{1,160}$/', $dataId)
            || ($bodyDataId !== '' && !hash_equals($dataId, $bodyDataId))) {
            throw new ApiException(400, 'INVALID_WEBHOOK', 'Evento invalido.');
        }
        $eventType = $type === 'topic_chargebacks_wh'
            ? 'chargeback'
            : 'payment';

        $eventId = Crypto::uuid();
        try {
            $insert = $this->database->pdo()->prepare(
                "INSERT INTO webhook_events
                    (id, provider, delivery_id, resource_id, event_type, payload_json)
                 VALUES
                    (:id, 'mercado_pago', :delivery_id, :resource_id,
                     :event_type, :payload_json)"
            );
            $insert->execute([
                'id' => $eventId,
                'delivery_id' => $providerRequestId,
                'resource_id' => $dataId,
                'event_type' => $eventType,
                'payload_json' => $request->rawBody,
            ]);
        } catch (PDOException $error) {
            if ((string) $error->getCode() === '23000') {
                return;
            }
            throw $error;
        }
    }

    public function processPendingWebhooks(int $limit = 25): int
    {
        $limit = max(1, min(100, $limit));
        $statement = $this->database->pdo()->query(
            "SELECT id FROM webhook_events
             WHERE attempts < 10
               AND (
                    status = 'received'
                    OR (
                        status = 'failed'
                        AND (
                            next_attempt_at IS NULL
                            OR next_attempt_at <= CURRENT_TIMESTAMP(6)
                        )
                    )
                    OR (
                        status = 'processing'
                        AND processing_started_at < DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 5 MINUTE)
                    )
               )
             ORDER BY received_at ASC LIMIT {$limit}"
        );
        $processed = 0;
        foreach ($statement->fetchAll(PDO::FETCH_COLUMN) as $id) {
            try {
                $this->processWebhookEvent((string) $id);
                ++$processed;
            } catch (\Throwable $error) {
                error_log('ChurchDAW webhook retry: ' . $error->getMessage());
            }
        }
        return $processed;
    }

    /**
     * Reconciles orders independently from webhook delivery. This is the
     * recovery path after provider outages, delayed preference visibility,
     * or restoring a database backup.
     *
     * @return array{checked: int, reconciled: int, failed: int}
     */
    public function reconcilePayments(int $limit = 100): array
    {
        $limit = max(1, min(500, $limit));
        $statement = $this->database->pdo()->query(
            "SELECT id FROM orders
             WHERE provider = 'mercado_pago'
                AND (
                     created_at >= DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 90 DAY)
                     OR provider_order_id IS NOT NULL
                     OR status = 'paid_review'
              )
             ORDER BY
                COALESCE(
                    provider_reconcile_attempted_at,
                    '1970-01-01 00:00:00'
                ) ASC,
                created_at ASC
             LIMIT {$limit}"
        );
        $stats = ['checked' => 0, 'reconciled' => 0, 'failed' => 0];
        foreach ($statement->fetchAll(PDO::FETCH_COLUMN) as $orderId) {
            ++$stats['checked'];
            try {
                if ($this->reconcilePaymentOrder((string) $orderId)) {
                    ++$stats['reconciled'];
                }
                $this->recordReconciliationSuccess((string) $orderId);
            } catch (\Throwable $error) {
                ++$stats['failed'];
                $this->recordReconciliationFailure(
                    (string) $orderId,
                    $error
                );
                error_log(
                    'ChurchDAW reconciliation order=' . $orderId
                    . ' error=' . $error->getMessage()
                );
            }
        }
        return $stats;
    }

    private function recordReconciliationSuccess(string $orderId): void
    {
        $this->database->pdo()->prepare(
            'UPDATE orders
             SET provider_reconcile_attempted_at = CURRENT_TIMESTAMP(6),
                 provider_reconcile_failures = 0,
                 provider_reconcile_error = NULL
             WHERE id = :id'
        )->execute(['id' => $orderId]);
    }

    private function recordReconciliationFailure(
        string $orderId,
        \Throwable $error
    ): void {
        $this->database->pdo()->prepare(
            'UPDATE orders
             SET provider_reconcile_attempted_at = CURRENT_TIMESTAMP(6),
                 provider_reconcile_failures =
                    LEAST(provider_reconcile_failures + 1, 65535),
                 provider_reconcile_error = :provider_reconcile_error
             WHERE id = :id'
        )->execute([
            'provider_reconcile_error' => mb_substr(
                $error->getMessage(),
                0,
                500
            ),
            'id' => $orderId,
        ]);
    }

    public function reconcilePaymentOrder(string $orderId): bool
    {
        if (!preg_match('/^[a-f0-9-]{36}$/i', $orderId)) {
            throw new \InvalidArgumentException(
                'Pedido local invalido para reconciliacao.'
            );
        }
        $select = $this->database->pdo()->prepare(
            "SELECT * FROM orders
             WHERE id = :id AND provider = 'mercado_pago' LIMIT 1"
        );
        $select->execute(['id' => $orderId]);
        $order = $select->fetch();
        if (!is_array($order)) {
            throw new \RuntimeException(
                'Pedido local inexistente para reconciliacao.'
            );
        }

        $preferenceId = (string) ($order['provider_preference_id'] ?? '');
        $canonicalPreference = null;
        if ($preferenceId === '') {
            $preference = $this->mercadoPago
                ->findPreferenceByExternalReference($orderId);
            if ($preference === null) {
                $this->touchReconciledOrder(
                    $orderId,
                    in_array(
                        (string) $order['status'],
                        ['creating', 'provider_ambiguous'],
                        true
                    )
                );
                return false;
            }
            $canonicalPreference = $preference;
            $this->assertPreferenceMatchesOrder(
                $preference,
                $orderId,
                (string) $order['product_code'],
                strtoupper((string) $order['currency']),
                (int) $order['expected_amount_cents']
            );
            $preferenceId = (string) ($preference['id'] ?? '');
            $checkoutUrl = (string) (
                $this->config->bool('mercado_pago.sandbox')
                    ? ($preference['sandbox_init_point'] ?? '')
                    : ($preference['init_point'] ?? '')
            );
            if ($preferenceId === ''
                || !str_starts_with($checkoutUrl, 'https://')) {
                throw new \RuntimeException(
                    'Preferencia recuperada esta incompleta.'
                );
            }
            $persistPreference = $this->database->pdo()->prepare(
                "UPDATE orders
                 SET provider_preference_id = :preference_id,
                     checkout_url = :checkout_url,
                     checkout_expires_at = NULL,
                     status = CASE
                         WHEN status IN (
                            'creating', 'provider_error', 'provider_ambiguous'
                         ) THEN 'pending'
                         ELSE status
                     END,
                     checkout_claim_token = NULL,
                     checkout_claimed_at = NULL,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6)
                 WHERE id = :id
                   AND provider_preference_id IS NULL"
            );
            $persistPreference->execute([
                'preference_id' => $preferenceId,
                'checkout_url' => $checkoutUrl,
                'id' => $orderId,
            ]);
            if ($persistPreference->rowCount() !== 1) {
                throw new \RuntimeException(
                    'Preferencia mudou durante a reconciliacao.'
                );
            }
            $order['provider_preference_id'] = $preferenceId;
            $order['checkout_url'] = $checkoutUrl;
        }
        if ($canonicalPreference === null) {
            $canonicalPreference = $this->mercadoPago
                ->preference($preferenceId);
            $this->assertPreferenceMatchesOrder(
                $canonicalPreference,
                $orderId,
                (string) $order['product_code'],
                strtoupper((string) $order['currency']),
                (int) $order['expected_amount_cents']
            );
        }

        $merchantOrderIds = [];
        $storedMerchantOrderId = (string) ($order['provider_order_id'] ?? '');
        if ($storedMerchantOrderId !== '') {
            if (!preg_match('/^[0-9]{1,32}$/', $storedMerchantOrderId)) {
                throw new \RuntimeException(
                    'Pedido comercial local possui identificador invalido.'
                );
            }
            $merchantOrderIds[$storedMerchantOrderId] = true;
        }
        $createdAt = strtotime((string) ($order['created_at'] ?? ''));
        $withinSearchWindow = $createdAt !== false
            && $createdAt >= time() - (89 * 86400);
        if ($storedMerchantOrderId === ''
            || $withinSearchWindow
            || (string) $order['status'] === 'paid_review') {
            $summaries = $this->mercadoPago
                ->merchantOrdersByExternalReference($orderId);
            foreach ($summaries as $summary) {
                $merchantOrderId = (string) ($summary['id'] ?? '');
                if (!preg_match('/^[0-9]{1,32}$/', $merchantOrderId)) {
                    throw new \RuntimeException(
                        'Busca de reconciliacao retornou pedido invalido.'
                    );
                }
                $merchantOrderIds[$merchantOrderId] = true;
            }
        }
        if ((string) $order['status'] === 'paid_review') {
            $knownPayments = $this->database->pdo()->prepare(
                "SELECT provider_payment_id
                 FROM payments
                 WHERE order_id = :order_id
                   AND provider = 'mercado_pago'"
            );
            $knownPayments->execute(['order_id' => $orderId]);
            foreach ($knownPayments->fetchAll(PDO::FETCH_COLUMN) as $paymentId) {
                $canonicalPayment = $this->mercadoPago->payment(
                    (string) $paymentId
                );
                if (!hash_equals(
                    $orderId,
                    (string) ($canonicalPayment['external_reference'] ?? '')
                )) {
                    throw new \RuntimeException(
                        'Pagamento conhecido diverge da compra em revisao.'
                    );
                }
                $knownMerchantOrderId = (string) (
                    $canonicalPayment['order']['id'] ?? ''
                );
                if (!preg_match(
                    '/^[0-9]{1,32}$/',
                    $knownMerchantOrderId
                )) {
                    throw new \RuntimeException(
                        'Pagamento conhecido sem Merchant Order valido.'
                    );
                }
                $merchantOrderIds[$knownMerchantOrderId] = true;
            }
        }
        $merchantOrders = [];
        foreach (array_keys($merchantOrderIds) as $merchantOrderId) {
            $merchantOrders[] = $this->mercadoPago
                ->merchantOrder((string) $merchantOrderId);
        }

        if ($merchantOrders === []) {
            if ($this->reconcileOrderFromCanonicalPreference(
                $orderId,
                $preferenceId,
                $canonicalPreference
            )) {
                return true;
            }
            $this->touchReconciledOrder($orderId);
            return false;
        }

        $matchingOrders = [];
        $unexpectedPaidOrder = false;
        foreach ($merchantOrders as $merchantOrder) {
            if (!hash_equals(
                $orderId,
                (string) ($merchantOrder['external_reference'] ?? '')
            )) {
                throw new \RuntimeException(
                    'Pedido comercial buscado possui referencia divergente.'
                );
            }
            if (!hash_equals(
                $preferenceId,
                (string) ($merchantOrder['preference_id'] ?? '')
            )) {
                $unexpectedPaidOrder = $unexpectedPaidOrder
                    || !empty($merchantOrder['payments']);
                continue;
            }
            $matchingOrders[] = $merchantOrder;
        }

        $newReviewReason = $unexpectedPaidOrder
            || count($matchingOrders) > 1;
        $reviewRequired = (string) $order['status'] === 'paid_review'
            || $newReviewReason;
        if ($newReviewReason) {
            $this->markOrderForPaymentReview(
                $orderId,
                'multiple_or_unexpected_merchant_orders'
            );
        }
        if ($matchingOrders === []) {
            $this->touchReconciledOrder($orderId);
            return $reviewRequired;
        }

        try {
            foreach ($matchingOrders as $merchantOrder) {
                $payments = $merchantOrder['payments'] ?? [];
                if (!is_array($payments)) {
                    throw new \RuntimeException(
                        'Pedido comercial possui pagamentos invalidos.'
                    );
                }
                if ($payments === []) {
                    $this->applyCanonicalEmptyMerchantOrder(
                        $orderId,
                        $merchantOrder
                    );
                    if (!$reviewRequired
                        && $this->merchantOrderIsTerminalUnpaid(
                            $merchantOrder
                        )) {
                        $this->reconcileOrderFromCanonicalPreference(
                            $orderId,
                            $preferenceId,
                            $canonicalPreference,
                            true
                        );
                    }
                    continue;
                }
                $triggerPaymentId = '';
                foreach ($payments as $paymentSummary) {
                    if (is_array($paymentSummary)
                        && preg_match(
                            '/^[0-9]{1,32}$/',
                            (string) ($paymentSummary['id'] ?? '')
                        )) {
                        $triggerPaymentId = (string) $paymentSummary['id'];
                        break;
                    }
                }
                if ($triggerPaymentId === '') {
                    throw new \RuntimeException(
                        'Pedido comercial possui pagamentos sem identificador.'
                    );
                }
                $triggerPayment = $this->mercadoPago->payment(
                    $triggerPaymentId
                );
                $this->applyCanonicalMerchantOrder(
                    $triggerPayment,
                    $merchantOrder,
                    $reviewRequired
                );
                if (!$reviewRequired
                    && $this->merchantOrderIsTerminalUnpaid(
                        $merchantOrder
                    )) {
                    $this->reconcileOrderFromCanonicalPreference(
                        $orderId,
                        $preferenceId,
                        $canonicalPreference,
                        true
                    );
                }
            }
        } finally {
            if ($reviewRequired) {
                $this->enforcePaymentReviewCoverage($orderId);
            }
        }
        $this->touchReconciledOrder($orderId);
        return true;
    }

    /**
     * A provider preference may release the billing hold only when every
     * canonical Merchant Order payment is conclusively unpaid. Unknown or
     * asynchronous states remain fail-closed.
     *
     * @param array<string, mixed> $merchantOrder
     */
    private function merchantOrderIsTerminalUnpaid(array $merchantOrder): bool
    {
        $merchantStatus = (string) ($merchantOrder['status'] ?? '');
        $payments = $merchantOrder['payments'] ?? null;
        if ($merchantStatus !== 'expired'
            || Money::toCents($merchantOrder['paid_amount'] ?? 0) !== 0
            || Money::toCents($merchantOrder['refunded_amount'] ?? 0) !== 0) {
            return false;
        }

        if (!is_array($payments)) {
            return false;
        }
        foreach ($payments as $payment) {
            if (!is_array($payment)
                || !in_array(
                    (string) ($payment['status'] ?? ''),
                    ['rejected', 'cancelled', 'canceled', 'returned'],
                    true
                )) {
                return false;
            }
        }
        return true;
    }

    public function processWebhookEvent(string $eventId): void
    {
        $event = $this->database->transaction(function (PDO $pdo) use ($eventId): ?array {
            $select = $pdo->prepare(
                'SELECT * FROM webhook_events WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $eventId]);
            $row = $select->fetch();
            if (!is_array($row)) {
                throw new \RuntimeException('Evento de webhook inexistente.');
            }
            if ($row['status'] === 'processed') {
                return null;
            }
            if ((int) $row['attempts'] >= 10) {
                return null;
            }
            if ($row['status'] === 'failed'
                && $row['next_attempt_at'] !== null
                && strtotime((string) $row['next_attempt_at']) > time()) {
                return null;
            }
            if ($row['status'] === 'processing'
                && $row['processing_started_at'] !== null
                && strtotime((string) $row['processing_started_at']) > time() - 300) {
                return null;
            }
            $pdo->prepare(
                "UPDATE webhook_events
                 SET status = 'processing', processing_started_at = CURRENT_TIMESTAMP(6),
                     attempts = attempts + 1, last_error = NULL
                 WHERE id = :id"
            )->execute(['id' => $eventId]);
            return $row;
        });

        if ($event === null) {
            return;
        }

        try {
            if ((string) $event['event_type'] === 'chargeback') {
                $chargeback = $this->mercadoPago->chargeback(
                    (string) $event['resource_id']
                );
                if (!hash_equals(
                    (string) $event['resource_id'],
                    (string) ($chargeback['id'] ?? '')
                )) {
                    throw new \RuntimeException(
                        'Contestacao canonica nao corresponde ao webhook.'
                    );
                }
                $chargebackResources = [];
                foreach ($this->chargebackPaymentIds($chargeback) as $paymentId) {
                    $canonicalPayment = $this->mercadoPago->payment($paymentId);
                    $merchantOrderId = (string) (
                        $canonicalPayment['order']['id'] ?? ''
                    );
                    if (!preg_match('/^[0-9]{1,32}$/', $merchantOrderId)) {
                        throw new \RuntimeException(
                            'Pagamento contestado sem pedido comercial canonico.'
                        );
                    }
                    $merchantOrder = $this->mercadoPago
                        ->merchantOrder($merchantOrderId);
                    $chargebackResources[] = [
                        'payment' => $canonicalPayment,
                        'merchant_order' => $merchantOrder,
                    ];
                }
                $this->applyCanonicalChargebackBatch(
                    $chargeback,
                    $chargebackResources
                );
            } else {
                $canonicalPayment = $this->mercadoPago->payment(
                    (string) $event['resource_id']
                );
                $merchantOrderId = (string) (
                    $canonicalPayment['order']['id'] ?? ''
                );
                if (!preg_match('/^[0-9]{1,32}$/', $merchantOrderId)) {
                    throw new \RuntimeException(
                        'Pagamento sem pedido comercial canonico.'
                    );
                }
                $merchantOrder = $this->mercadoPago
                    ->merchantOrder($merchantOrderId);
                $this->applyCanonicalMerchantOrder(
                    $canonicalPayment,
                    $merchantOrder
                );
            }
            $this->database->pdo()->prepare(
                "UPDATE webhook_events
                 SET status = 'processed', processed_at = CURRENT_TIMESTAMP(6),
                     processing_started_at = NULL, next_attempt_at = NULL
                 WHERE id = :id"
            )->execute(['id' => $eventId]);
        } catch (\Throwable $error) {
            $delaySeconds = min(
                3600,
                15 * (2 ** min(8, max(0, (int) $event['attempts'])))
            );
            $nextAttemptAt = (new \DateTimeImmutable(
                'now',
                new \DateTimeZone('UTC')
            ))->modify("+{$delaySeconds} seconds");
            $this->database->pdo()->prepare(
                "UPDATE webhook_events
                 SET status = 'failed', processing_started_at = NULL,
                     last_error = :last_error,
                     next_attempt_at = :next_attempt_at
                 WHERE id = :id"
            )->execute([
                'last_error' => mb_substr($error->getMessage(), 0, 500),
                'next_attempt_at' => $nextAttemptAt->format(
                    'Y-m-d H:i:s.u'
                ),
                'id' => $eventId,
            ]);
            throw $error;
        }
    }

    /**
     * @param array<string, mixed> $triggerPayment
     * @param array<string, mixed> $merchantOrder
     */
    private function applyCanonicalMerchantOrder(
        array $triggerPayment,
        array $merchantOrder,
        bool $deferReviewEntitlement = false
    ): void {
        $triggerPaymentId = (string) ($triggerPayment['id'] ?? '');
        $triggerOrderId = (string) ($triggerPayment['external_reference'] ?? '');
        $triggerMerchantOrderId = (string) ($triggerPayment['order']['id'] ?? '');
        $triggerOrderType = (string) ($triggerPayment['order']['type'] ?? '');
        $triggerCollectorId = (string) ($triggerPayment['collector_id'] ?? '');
        $triggerPaymentStatus = (string) ($triggerPayment['status'] ?? '');
        $triggerCurrency = strtoupper(
            (string) ($triggerPayment['currency_id'] ?? '')
        );
        $triggerAmountCents = Money::toCents(
            $triggerPayment['transaction_amount'] ?? ''
        );
        $triggerRefundedCents = Money::toCents(
            $triggerPayment['transaction_amount_refunded']
                ?? $triggerPayment['amount_refunded']
                ?? 0
        );

        $merchantOrderId = (string) ($merchantOrder['id'] ?? '');
        $orderId = (string) ($merchantOrder['external_reference'] ?? '');
        $preferenceId = (string) ($merchantOrder['preference_id'] ?? '');
        $collectorId = (string) ($merchantOrder['collector']['id'] ?? '');
        $applicationId = (string) ($merchantOrder['application_id'] ?? '');
        $merchantStatus = (string) ($merchantOrder['status'] ?? '');
        $merchantOrderStatus = (string) ($merchantOrder['order_status'] ?? '');
        $totalCents = Money::toCents($merchantOrder['total_amount'] ?? '');
        $paidCents = Money::toCents($merchantOrder['paid_amount'] ?? '');
        $refundedCents = Money::toCents($merchantOrder['refunded_amount'] ?? '');
        $providerStatus = $merchantStatus . '/' . $merchantOrderStatus;

        if (!preg_match('/^[0-9]{1,32}$/', $triggerPaymentId)
            || !preg_match('/^[0-9]{1,32}$/', $merchantOrderId)
            || !hash_equals($triggerMerchantOrderId, $merchantOrderId)
            || $triggerOrderType !== 'mercadopago'
            || !preg_match('/^[a-f0-9-]{36}$/i', $orderId)
            || !hash_equals($triggerOrderId, $orderId)
            || $preferenceId === ''
            || !preg_match('/^[a-z_]{2,64}$/', $triggerPaymentStatus)
            || !preg_match('/^[a-z_]{2,64}$/', $merchantStatus)
            || ($merchantOrderStatus !== ''
                && !preg_match('/^[a-z_]{2,64}$/', $merchantOrderStatus))
            || $paidCents < 0
            || $refundedCents < 0
            || $refundedCents > $paidCents
            || $triggerRefundedCents < 0
            || $triggerRefundedCents > $triggerAmountCents) {
            throw new \RuntimeException('Pedido comercial canonico incompleto.');
        }

        $configuredCollector = $this->config->string('mercado_pago.collector_id');
        if ($collectorId === ''
            || !hash_equals($collectorId, $triggerCollectorId)
            || ($configuredCollector !== ''
                && !hash_equals($configuredCollector, $collectorId))) {
            throw new \RuntimeException('Recebedor do pagamento nao corresponde.');
        }
        $configuredApplication = $this->config->string(
            'mercado_pago.application_id'
        );
        if ($configuredApplication !== ''
            && !hash_equals($configuredApplication, $applicationId)) {
            throw new \RuntimeException('Aplicacao do pagamento nao corresponde.');
        }

        $snapshotSelect = $this->database->pdo()->prepare(
            'SELECT provider, expected_amount_cents, currency,
                    provider_preference_id, provider_order_id, status
             FROM orders WHERE id = :id LIMIT 1'
        );
        $snapshotSelect->execute(['id' => $orderId]);
        $orderSnapshot = $snapshotSelect->fetch();
        if (!is_array($orderSnapshot)
            || (string) $orderSnapshot['provider'] !== 'mercado_pago'
            || (int) $orderSnapshot['expected_amount_cents'] !== $totalCents
            || !hash_equals(
                (string) $orderSnapshot['provider_preference_id'],
                $preferenceId
            )
            || ($orderSnapshot['provider_order_id'] !== null
                && !hash_equals(
                    (string) $orderSnapshot['provider_order_id'],
                    $merchantOrderId
                )
                && (string) $orderSnapshot['status'] !== 'paid_review')) {
            throw new \RuntimeException(
                'Pedido comercial nao corresponde a compra local.'
            );
        }
        $expectedCurrency = strtoupper((string) $orderSnapshot['currency']);
        if (!preg_match('/^[A-Z]{3}$/', $expectedCurrency)) {
            throw new \RuntimeException('Moeda gravada na compra local e invalida.');
        }

        $paymentRows = [];
        $grossFromDetailsCents = 0;
        $refundFromDetailsCents = 0;
        $triggerFound = false;
        $disputeReason = null;
        $firstApprovedAt = null;
        $seenPaymentIds = [];
        foreach (($merchantOrder['payments'] ?? []) as $payment) {
            if (!is_array($payment)) {
                throw new \RuntimeException('Detalhe de pagamento invalido.');
            }
            $paymentId = (string) ($payment['id'] ?? '');
            $paymentStatus = (string) ($payment['status'] ?? '');
            $currency = strtoupper((string) ($payment['currency_id'] ?? ''));
            $amountCents = Money::toCents($payment['transaction_amount'] ?? '');
            $amountRefundedCents = Money::toCents(
                $payment['amount_refunded'] ?? 0
            );
            if (!preg_match('/^[0-9]{1,32}$/', $paymentId)
                || !preg_match('/^[a-z_]{2,64}$/', $paymentStatus)
                || !hash_equals($expectedCurrency, $currency)
                || $amountCents < 0
                || $amountRefundedCents < 0
                || $amountRefundedCents > $amountCents
                || ($paymentStatus === 'refunded'
                    && $amountRefundedCents !== $amountCents)) {
                throw new \RuntimeException('Detalhe de pagamento inconsistente.');
            }
            if (isset($seenPaymentIds[$paymentId])) {
                throw new \RuntimeException(
                    'Pagamento duplicado no pedido comercial canonico.'
                );
            }
            $seenPaymentIds[$paymentId] = true;

            $canonicalPayment = hash_equals($triggerPaymentId, $paymentId)
                ? $triggerPayment
                : $this->mercadoPago->payment($paymentId);
            $canonicalPaymentId = (string) (
                $canonicalPayment['id'] ?? ''
            );
            $canonicalOrderId = (string) (
                $canonicalPayment['external_reference'] ?? ''
            );
            $canonicalMerchantOrderId = (string) (
                $canonicalPayment['order']['id'] ?? ''
            );
            $canonicalOrderType = (string) (
                $canonicalPayment['order']['type'] ?? ''
            );
            $canonicalCollectorId = (string) (
                $canonicalPayment['collector_id'] ?? ''
            );
            $canonicalStatus = (string) (
                $canonicalPayment['status'] ?? ''
            );
            $canonicalCurrency = strtoupper((string) (
                $canonicalPayment['currency_id'] ?? ''
            ));
            $canonicalAmountCents = Money::toCents(
                $canonicalPayment['transaction_amount'] ?? ''
            );
            $canonicalRefundedCents = Money::toCents(
                $canonicalPayment['transaction_amount_refunded']
                    ?? $canonicalPayment['amount_refunded']
                    ?? 0
            );
            if (!hash_equals($paymentId, $canonicalPaymentId)
                || !hash_equals($orderId, $canonicalOrderId)
                || !hash_equals(
                    $merchantOrderId,
                    $canonicalMerchantOrderId
                )
                || $canonicalOrderType !== 'mercadopago'
                || !hash_equals($collectorId, $canonicalCollectorId)
                || !hash_equals($currency, $canonicalCurrency)
                || $amountCents !== $canonicalAmountCents
                || !hash_equals($paymentStatus, $canonicalStatus)
                || $amountRefundedCents !== $canonicalRefundedCents) {
                throw new \RuntimeException(
                    'Snapshots de Payment e Merchant Order divergem.'
                );
            }

            $approvedAt = self::mysqlDate(
                (string) ($canonicalPayment['date_approved'] ?? '')
            );
            if ($approvedAt !== null
                && ($firstApprovedAt === null || $approvedAt < $firstApprovedAt)) {
                $firstApprovedAt = $approvedAt;
            }
            if (in_array(
                $paymentStatus,
                ['approved', 'refunded', 'charged_back', 'in_mediation'],
                true
            )) {
                $grossFromDetailsCents += $amountCents;
            }
            $refundFromDetailsCents += $amountRefundedCents;
            if ($paymentStatus === 'charged_back') {
                $disputeReason = 'charged_back';
            } elseif ($paymentStatus === 'in_mediation'
                && $disputeReason === null) {
                $disputeReason = 'in_mediation';
            }
            if (hash_equals($triggerPaymentId, $paymentId)) {
                $triggerFound = true;
                if (!hash_equals($triggerCurrency, $currency)
                    || $triggerAmountCents !== $amountCents
                    || !hash_equals($triggerPaymentStatus, $paymentStatus)
                    || $triggerRefundedCents !== $amountRefundedCents) {
                    throw new \RuntimeException(
                        'Snapshots do pagamento disparador e do pedido comercial divergem.'
                    );
                }
            }

            $paymentRows[] = [
                'provider_payment_id' => $paymentId,
                'amount_cents' => $amountCents,
                'refunded_amount_cents' => $amountRefundedCents,
                'currency' => $currency,
                'status' => $paymentStatus,
                'approved_at' => $approvedAt,
                'fingerprint' => hash('sha256', json_encode([
                    'merchant_order_id' => $merchantOrderId,
                    'id' => $paymentId,
                    'status' => $paymentStatus,
                    'amount_cents' => $amountCents,
                    'refunded_amount_cents' => $amountRefundedCents,
                    'currency' => $currency,
                ], JSON_THROW_ON_ERROR)),
            ];
        }

        if (!$triggerFound
            || $grossFromDetailsCents !== $paidCents
            || $refundFromDetailsCents !== $refundedCents) {
            throw new \RuntimeException(
                'Agregado do pedido diverge dos pagamentos detalhados.'
            );
        }

        $this->database->transaction(function (PDO $pdo) use (
            $paymentRows,
            $triggerPaymentId,
            $merchantOrderId,
            $orderId,
            $preferenceId,
            $providerStatus,
            $totalCents,
            $paidCents,
            $refundedCents,
            $disputeReason,
            $merchantStatus,
            $merchantOrderStatus,
            $firstApprovedAt,
            $expectedCurrency,
            $deferReviewEntitlement
        ): void {
            $select = $pdo->prepare(
                'SELECT * FROM orders WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $orderId]);
            $order = $select->fetch();
            if (!is_array($order)
                || (string) $order['provider'] !== 'mercado_pago'
                || (int) $order['expected_amount_cents'] !== $totalCents
                || !hash_equals(
                    $expectedCurrency,
                    strtoupper((string) $order['currency'])
                )
                || !hash_equals(
                    (string) $order['provider_preference_id'],
                    $preferenceId
                )
                || ($order['provider_order_id'] !== null
                    && !hash_equals(
                        (string) $order['provider_order_id'],
                        $merchantOrderId
                    )
                    && (string) $order['status'] !== 'paid_review')) {
                throw new \RuntimeException(
                    'Pedido comercial nao corresponde a compra local.'
                );
            }

            foreach ($paymentRows as $paymentRow) {
                $mapping = $pdo->prepare(
                    "SELECT order_id FROM payments
                     WHERE provider = 'mercado_pago'
                       AND provider_payment_id = :provider_payment_id
                     LIMIT 1 FOR UPDATE"
                );
                $mapping->execute([
                    'provider_payment_id' => $paymentRow['provider_payment_id'],
                ]);
                $mappedOrderId = $mapping->fetchColumn();
                if (is_string($mappedOrderId)
                    && !hash_equals($orderId, $mappedOrderId)) {
                    throw new \RuntimeException(
                        'Pagamento ja vinculado a outra compra local.'
                    );
                }
                $pdo->prepare(
                    "INSERT INTO payments
                        (id, order_id, provider, provider_payment_id, amount_cents,
                         refunded_amount_cents, currency, status, raw_fingerprint,
                         approved_at)
                     VALUES
                        (:id, :order_id, 'mercado_pago', :provider_payment_id,
                         :amount_cents, :refunded_amount_cents, :currency, :status,
                         :raw_fingerprint, :approved_at)
                     ON DUPLICATE KEY UPDATE
                        amount_cents = VALUES(amount_cents),
                        refunded_amount_cents = GREATEST(
                            refunded_amount_cents,
                            VALUES(refunded_amount_cents)
                        ),
                        currency = VALUES(currency),
                        raw_fingerprint = CASE
                            WHEN status IN (
                                'refunded', 'charged_back', 'in_mediation'
                            ) AND VALUES(status) = 'approved'
                            THEN raw_fingerprint
                            ELSE VALUES(raw_fingerprint)
                        END,
                        status = CASE
                            WHEN status IN (
                                'refunded', 'charged_back', 'in_mediation'
                            ) AND VALUES(status) = 'approved'
                            THEN status
                            ELSE VALUES(status)
                        END,
                        approved_at = COALESCE(VALUES(approved_at), approved_at)"
                )->execute([
                    'id' => Crypto::uuid(),
                    'order_id' => $orderId,
                    'provider_payment_id' => $paymentRow['provider_payment_id'],
                    'amount_cents' => $paymentRow['amount_cents'],
                    'refunded_amount_cents' =>
                        $paymentRow['refunded_amount_cents'],
                    'currency' => $paymentRow['currency'],
                    'status' => $paymentRow['status'],
                    'raw_fingerprint' => $paymentRow['fingerprint'],
                    'approved_at' => $paymentRow['approved_at'],
                ]);
            }

            $userLock = $pdo->prepare(
                'SELECT id FROM users WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $userLock->execute(['id' => $order['user_id']]);
            if ($userLock->fetchColumn() === false) {
                throw new \RuntimeException('Comprador da compra local inexistente.');
            }

            $licenseSelect = $pdo->prepare(
                'SELECT id, status, active_slot
                 FROM licenses WHERE order_id = :order_id LIMIT 1 FOR UPDATE'
            );
            $licenseSelect->execute(['order_id' => $orderId]);
            $orderLicense = $licenseSelect->fetch();

            $netPaidCents = $paidCents - $refundedCents;
            $fullyCovered = $merchantStatus === 'closed'
                && $merchantOrderStatus === 'paid'
                && $netPaidCents >= $totalCents
                && $disputeReason === null;
            $canIssueFirstLicense = $fullyCovered && $refundedCents === 0;
            $licenseIsActive = is_array($orderLicense)
                && $orderLicense['status'] === 'active'
                && (int) $orderLicense['active_slot'] === 1;
            $mustRevoke = $disputeReason !== null
                || ($refundedCents > 0 && $netPaidCents < $totalCents);

            // paid_review is a manual, fail-closed state. Canonical updates may
            // still revoke an entitlement, but they must never silently clear
            // the review or issue/reactivate a licence. A refund/chargeback on
            // one Merchant Order does not prove that every duplicate was fixed.
            if ((string) $order['status'] === 'paid_review') {
                $reviewCoverageCents = null;
                $reviewHasDispute = null;
                if (!$deferReviewEntitlement) {
                    $reviewCoverageCents = $this->safeCoveredCents(
                        $pdo,
                        $orderId
                    );
                    $reviewHasDispute = $this->hasDisputedPayment(
                        $pdo,
                        $orderId
                    );
                    if (($reviewCoverageCents < $totalCents
                            || $reviewHasDispute)
                        && is_array($orderLicense)) {
                        $this->revokeLicense(
                            $pdo,
                            (string) $orderLicense['id'],
                            'payment_review_underfunded'
                        );
                    }
                }
                AuditLog::recordWithPdo(
                    $pdo,
                    'payment.review_preserved',
                    null,
                    (string) $order['user_id'],
                    is_array($orderLicense)
                        ? (string) $orderLicense['id']
                        : null,
                    null,
                    [
                        'order_id' => $orderId,
                        'merchant_order_id' => $merchantOrderId,
                        'provider_status' => $providerStatus,
                        'net_paid_cents' => $netPaidCents,
                        'adverse_reason' => $mustRevoke
                            ? ($disputeReason ?? 'refunded')
                            : null,
                        'review_coverage_cents' => $reviewCoverageCents,
                        'review_has_dispute' => $reviewHasDispute,
                        'entitlement_deferred' => $deferReviewEntitlement,
                    ]
                );
                return;
            }

            $currentStatus = (string) $order['status'];
            if (in_array(
                $currentStatus,
                ['in_mediation', 'charged_back'],
                true
            )) {
                if (is_array($orderLicense)) {
                    $this->revokeLicense(
                        $pdo,
                        (string) $orderLicense['id'],
                        $currentStatus
                    );
                }
                AuditLog::recordWithPdo(
                    $pdo,
                    'payment.adverse_state_preserved',
                    null,
                    (string) $order['user_id'],
                    is_array($orderLicense)
                        ? (string) $orderLicense['id']
                        : null,
                    null,
                    [
                        'order_id' => $orderId,
                        'preserved_status' => $currentStatus,
                        'provider_status' => $providerStatus,
                    ]
                );
                return;
            }
            if ($currentStatus === 'partial_refund' && !$mustRevoke) {
                if (is_array($orderLicense)) {
                    $this->revokeLicense(
                        $pdo,
                        (string) $orderLicense['id'],
                        'partial_refund'
                    );
                }
                return;
            }
            if (in_array($currentStatus, ['refunded', 'expired'], true)
                && !$mustRevoke) {
                if ($paidCents > 0) {
                    $this->updatePaidOrder(
                        $pdo,
                        $orderId,
                        $merchantOrderId,
                        $triggerPaymentId,
                        'review/stale_after_' . $currentStatus,
                        $firstApprovedAt,
                        'paid_review'
                    );
                    AuditLog::recordWithPdo(
                        $pdo,
                        'payment.terminal_repayment_review',
                        null,
                        (string) $order['user_id'],
                        is_array($orderLicense)
                            ? (string) $orderLicense['id']
                            : null,
                        null,
                        [
                            'order_id' => $orderId,
                            'previous_status' => $currentStatus,
                            'net_paid_cents' => $netPaidCents,
                        ]
                    );
                }
                return;
            }

            if ($canIssueFirstLicense || ($fullyCovered && $licenseIsActive)) {
                $activeSelect = $pdo->prepare(
                    'SELECT id, order_id FROM licenses
                     WHERE user_id = :user_id
                       AND product_code = :product_code
                       AND active_slot = 1
                     LIMIT 1 FOR UPDATE'
                );
                $activeSelect->execute([
                    'user_id' => $order['user_id'],
                    'product_code' => $order['product_code'],
                ]);
                $activeLicense = $activeSelect->fetch();
                if ((is_array($activeLicense)
                        && !hash_equals(
                            (string) $activeLicense['order_id'],
                            $orderId
                        ))
                    || (is_array($orderLicense) && !$licenseIsActive)) {
                    $this->updatePaidOrder(
                        $pdo,
                        $orderId,
                        $merchantOrderId,
                        $triggerPaymentId,
                        $providerStatus,
                        $firstApprovedAt,
                        'paid_review'
                    );
                    AuditLog::recordWithPdo(
                        $pdo,
                        'payment.entitlement_conflict',
                        null,
                        (string) $order['user_id'],
                        null,
                        null,
                        [
                            'order_id' => $orderId,
                            'merchant_order_id' => $merchantOrderId,
                            'net_paid_cents' => $netPaidCents,
                        ]
                    );
                    return;
                }

                if (!is_array($orderLicense)) {
                    $pdo->prepare(
                        "INSERT INTO licenses
                            (id, user_id, product_code, order_id, status,
                             max_active_devices)
                         VALUES
                            (:id, :user_id, :product_code, :order_id,
                             'active', 1)"
                    )->execute([
                        'id' => Crypto::uuid(),
                        'user_id' => $order['user_id'],
                        'product_code' => $order['product_code'],
                        'order_id' => $orderId,
                    ]);
                }
                $settledStatus = $netPaidCents === $totalCents
                    ? 'paid'
                    : 'paid_review';
                $settledProviderStatus = $settledStatus === 'paid'
                    ? $providerStatus
                    : 'review/overpaid';
                $this->updatePaidOrder(
                    $pdo,
                    $orderId,
                    $merchantOrderId,
                    $triggerPaymentId,
                    $settledProviderStatus,
                    $firstApprovedAt,
                    $settledStatus
                );
                AuditLog::recordWithPdo(
                    $pdo,
                    $settledStatus === 'paid'
                        ? 'payment.approved'
                        : 'payment.overpaid_review',
                    null,
                    (string) $order['user_id'],
                    is_array($orderLicense)
                        ? (string) $orderLicense['id']
                        : null,
                    null,
                    [
                        'order_id' => $orderId,
                        'merchant_order_id' => $merchantOrderId,
                        'payment_count' => count($paymentRows),
                        'net_paid_cents' => $netPaidCents,
                    ]
                );
                return;
            }

            if ($fullyCovered) {
                $this->updatePaidOrder(
                    $pdo,
                    $orderId,
                    $merchantOrderId,
                    $triggerPaymentId,
                    $providerStatus,
                    $firstApprovedAt,
                    'paid_review'
                );
                AuditLog::recordWithPdo(
                    $pdo,
                    'payment.repaid_review',
                    null,
                    (string) $order['user_id'],
                    is_array($orderLicense)
                        ? (string) $orderLicense['id']
                        : null,
                    null,
                    [
                        'order_id' => $orderId,
                        'merchant_order_id' => $merchantOrderId,
                        'net_paid_cents' => $netPaidCents,
                        'refunded_amount_cents' => $refundedCents,
                    ]
                );
                return;
            }

            if ($mustRevoke) {
                $reason = $disputeReason
                    ?? ($netPaidCents === 0
                        ? 'refunded'
                        : 'partial_refund');
                $storedProviderStatus = $reason === 'partial_refund'
                    ? 'review/partial_refund_balance'
                    : $providerStatus;
                $pdo->prepare(
                    "UPDATE orders
                     SET status = :status,
                         provider_order_id = :provider_order_id,
                         provider_payment_id = :payment_id,
                         provider_status = :provider_status,
                         reserves_checkout_slot = 0
                     WHERE id = :id"
                )->execute([
                    'status' => $reason,
                    'provider_order_id' => $merchantOrderId,
                    'payment_id' => $triggerPaymentId,
                    'provider_status' => $storedProviderStatus,
                    'id' => $orderId,
                ]);
                if (is_array($orderLicense)) {
                    $this->revokeLicense(
                        $pdo,
                        (string) $orderLicense['id'],
                        $reason
                    );
                }
                AuditLog::recordWithPdo(
                    $pdo,
                    'payment.' . $reason,
                    null,
                    (string) $order['user_id'],
                    is_array($orderLicense)
                        ? (string) $orderLicense['id']
                        : null,
                    null,
                    [
                        'order_id' => $orderId,
                        'merchant_order_id' => $merchantOrderId,
                        'net_paid_cents' => $netPaidCents,
                    ]
                );
                return;
            }

            if ($licenseIsActive) {
                throw new \RuntimeException(
                    'Reconciliacao inconclusiva para uma licenca ja ativa.'
                );
            }

            // A failed/rejected payment does not prove that its Checkout Pro
            // preference can no longer be paid. Keep reusing/blocking this
            // checkout until canonical preference expiration is confirmed.
            $localStatus = 'pending';
            $pdo->prepare(
                'UPDATE orders
                 SET status = :status,
                     provider_order_id = :provider_order_id,
                     provider_payment_id = :payment_id,
                     provider_status = :provider_status,
                     reserves_checkout_slot = 1
                 WHERE id = :id'
            )->execute([
                'status' => $localStatus,
                'provider_order_id' => $merchantOrderId,
                'payment_id' => $triggerPaymentId,
                'provider_status' => $providerStatus,
                'id' => $orderId,
            ]);
        });
        $this->touchReconciledOrder($orderId);
    }

    /**
     * @param array<string, mixed> $preference
     */
    private function reconcileOrderFromCanonicalPreference(
        string $orderId,
        string $preferenceId,
        array $preference,
        bool $allowKnownEmptyMerchantOrder = false
    ): bool {
        if (!array_key_exists('preference_expired', $preference)) {
            return false;
        }
        $preferenceExpired = filter_var(
            $preference['preference_expired'] ?? false,
            FILTER_VALIDATE_BOOL
        );
        $expirationText = (string) (
            $preference['expiration_date_to'] ?? ''
        );
        $expiration = null;
        if ($expirationText !== '') {
            try {
                $expiration = new \DateTimeImmutable($expirationText);
            } catch (\Exception) {
                throw new \RuntimeException(
                    'Preferencia possui vencimento canonico invalido.'
                );
            }
        }
        $now = new \DateTimeImmutable(
            'now',
            new \DateTimeZone('UTC')
        );
        if ($preferenceExpired) {
            $safeCutoff = $now->modify('-15 minutes');
            if ($expiration === null || $expiration > $safeCutoff) {
                return false;
            }
        }

        return $this->database->transaction(function (PDO $pdo) use (
            $orderId,
            $preferenceId,
            $preferenceExpired,
            $expiration,
            $now,
            $allowKnownEmptyMerchantOrder
        ): bool {
            $select = $pdo->prepare(
                'SELECT user_id, status, provider_preference_id,
                        provider_order_id
                 FROM orders WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $orderId]);
            $order = $select->fetch();
            if (!is_array($order)
                || !hash_equals(
                    (string) $order['provider_preference_id'],
                    $preferenceId
                )
                || ($order['provider_order_id'] !== null
                    && !$allowKnownEmptyMerchantOrder)
                || !in_array(
                    (string) $order['status'],
                    [
                        'creating',
                        'provider_error',
                        'provider_ambiguous',
                        'pending',
                    ],
                    true
                )) {
                return false;
            }
            if ($preferenceExpired) {
                $pdo->prepare(
                    "UPDATE orders
                     SET status = 'expired', reserves_checkout_slot = 0,
                         provider_status = 'preference/expired',
                         provider_reconciled_at = CURRENT_TIMESTAMP(6),
                         checkout_claim_token = NULL,
                         checkout_claimed_at = NULL
                     WHERE id = :id"
                )->execute(['id' => $orderId]);
                AuditLog::recordWithPdo(
                    $pdo,
                    'payment.preference_expired_canonical',
                    null,
                    (string) $order['user_id'],
                    null,
                    null,
                    ['order_id' => $orderId]
                );
            } else {
                $canonicalExpiration = $expiration !== null
                    && $expiration > $now
                    ? $expiration->setTimezone(
                        new \DateTimeZone('UTC')
                    )->format('Y-m-d H:i:s.u')
                    : null;
                $pdo->prepare(
                    "UPDATE orders
                     SET status = 'pending', reserves_checkout_slot = 1,
                         checkout_expires_at = :checkout_expires_at,
                         provider_status = 'preference/active',
                         provider_reconciled_at = CURRENT_TIMESTAMP(6),
                         checkout_claim_token = NULL,
                         checkout_claimed_at = NULL
                     WHERE id = :id"
                )->execute([
                    'checkout_expires_at' => $canonicalExpiration,
                    'id' => $orderId,
                ]);
            }
            return true;
        });
    }

    private function touchReconciledOrder(
        string $orderId,
        bool $markAmbiguous = false
    ): void {
        $this->database->pdo()->prepare(
            "UPDATE orders
             SET provider_reconciled_at = CURRENT_TIMESTAMP(6),
                 status = CASE
                    WHEN :mark_status = 1
                     AND status IN ('creating', 'provider_ambiguous')
                    THEN 'provider_ambiguous'
                    ELSE status
                 END,
                 checkout_claim_token = CASE
                    WHEN :mark_claim = 1 THEN NULL
                    ELSE checkout_claim_token
                 END,
                 checkout_claimed_at = CASE
                    WHEN :mark_claimed = 1 THEN NULL
                    ELSE checkout_claimed_at
                 END
             WHERE id = :id"
        )->execute([
            'mark_status' => $markAmbiguous ? 1 : 0,
            'mark_claim' => $markAmbiguous ? 1 : 0,
            'mark_claimed' => $markAmbiguous ? 1 : 0,
            'id' => $orderId,
        ]);
    }

    private function markOrderForPaymentReview(
        string $orderId,
        string $reason
    ): void {
        $this->database->transaction(function (PDO $pdo) use (
            $orderId,
            $reason
        ): void {
            $select = $pdo->prepare(
                'SELECT user_id FROM orders
                 WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $orderId]);
            $userId = $select->fetchColumn();
            if (!is_string($userId)) {
                throw new \RuntimeException(
                    'Pedido para revisao deixou de existir.'
                );
            }
            $pdo->prepare(
                "UPDATE orders
                 SET status = 'paid_review',
                     provider_status = :provider_status,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6),
                     checkout_claim_token = NULL,
                     checkout_claimed_at = NULL
                 WHERE id = :id"
            )->execute([
                'provider_status' => mb_substr(
                    'reconcile/' . $reason,
                    0,
                    64
                ),
                'id' => $orderId,
            ]);
            AuditLog::recordWithPdo(
                $pdo,
                'payment.reconciliation_review',
                null,
                $userId,
                null,
                null,
                ['order_id' => $orderId, 'reason' => $reason]
            );
        });
    }

    /**
     * @param array<string, mixed> $merchantOrder
     */
    private function applyCanonicalEmptyMerchantOrder(
        string $orderId,
        array $merchantOrder
    ): void {
        $merchantOrderId = (string) ($merchantOrder['id'] ?? '');
        $externalReference = (string) (
            $merchantOrder['external_reference'] ?? ''
        );
        $preferenceId = (string) ($merchantOrder['preference_id'] ?? '');
        $merchantStatus = (string) ($merchantOrder['status'] ?? '');
        $merchantOrderStatus = (string) (
            $merchantOrder['order_status'] ?? ''
        );
        $collectorId = (string) ($merchantOrder['collector']['id'] ?? '');
        $applicationId = (string) ($merchantOrder['application_id'] ?? '');
        $totalCents = Money::toCents(
            $merchantOrder['total_amount'] ?? ''
        );
        $payments = $merchantOrder['payments'] ?? [];

        if (!preg_match('/^[0-9]{1,32}$/', $merchantOrderId)
            || !hash_equals($orderId, $externalReference)
            || $preferenceId === ''
            || !in_array($merchantStatus, ['opened', 'expired'], true)
            || ($merchantOrderStatus !== ''
                && !preg_match('/^[a-z_]{2,64}$/', $merchantOrderStatus))
            || !is_array($payments)
            || $payments !== []) {
            throw new \RuntimeException(
                'Pedido comercial vazio e inconsistente.'
            );
        }
        $configuredCollector = $this->config->string(
            'mercado_pago.collector_id'
        );
        if ($collectorId === ''
            || ($configuredCollector !== ''
                && !hash_equals($configuredCollector, $collectorId))) {
            throw new \RuntimeException(
                'Recebedor do pedido vazio nao corresponde.'
            );
        }
        $configuredApplication = $this->config->string(
            'mercado_pago.application_id'
        );
        if ($configuredApplication !== ''
            && !hash_equals($configuredApplication, $applicationId)) {
            throw new \RuntimeException(
                'Aplicacao do pedido vazio nao corresponde.'
            );
        }

        $this->database->transaction(function (PDO $pdo) use (
            $orderId,
            $merchantOrderId,
            $preferenceId,
            $merchantStatus,
            $merchantOrderStatus,
            $totalCents
        ): void {
            $select = $pdo->prepare(
                'SELECT * FROM orders WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $orderId]);
            $order = $select->fetch();
            if (!is_array($order)
                || (string) $order['provider'] !== 'mercado_pago'
                || !in_array(
                    (string) $order['status'],
                    [
                        'creating',
                        'provider_error',
                        'provider_ambiguous',
                        'pending',
                        'paid_review',
                    ],
                    true
                )
                || (int) $order['expected_amount_cents'] !== $totalCents
                || !hash_equals(
                    (string) $order['provider_preference_id'],
                    $preferenceId
                )
                || ($order['provider_order_id'] !== null
                    && !hash_equals(
                        (string) $order['provider_order_id'],
                        $merchantOrderId
                    )
                    && (string) $order['status'] !== 'paid_review')) {
                throw new \RuntimeException(
                    'Pedido vazio nao corresponde a compra local.'
                );
            }
            $reviewIsSticky = (string) $order['status'] === 'paid_review';
            $localStatus = $reviewIsSticky ? 'paid_review' : 'pending';
            $providerStatus = mb_substr(
                $merchantStatus . '/' . $merchantOrderStatus,
                0,
                64
            );
            $pdo->prepare(
                "UPDATE orders
                 SET status = :status,
                     reserves_checkout_slot = :reserves_checkout_slot,
                     provider_order_id = CASE
                        WHEN status = 'paid_review' THEN provider_order_id
                        ELSE :provider_order_id
                     END,
                     provider_status = CASE
                        WHEN status = 'paid_review' THEN provider_status
                        ELSE :provider_status
                     END,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6),
                     checkout_claim_token = NULL,
                     checkout_claimed_at = NULL
                 WHERE id = :id"
            )->execute([
                'status' => $localStatus,
                'reserves_checkout_slot' => $localStatus === 'pending'
                    ? 1
                    : (int) $order['reserves_checkout_slot'],
                'provider_order_id' => $merchantOrderId,
                'provider_status' => $providerStatus,
                'id' => $orderId,
            ]);
        });
    }

    /**
     * @param array<string, mixed> $chargeback
     * @return list<string>
     */
    private function chargebackPaymentIds(array $chargeback): array
    {
        $rawPayments = $chargeback['payments'] ?? null;
        if (is_string($rawPayments) || is_int($rawPayments)) {
            $rawPayments = [$rawPayments];
        }
        if (!is_array($rawPayments) || $rawPayments === []) {
            throw new \RuntimeException(
                'Contestacao canonica sem pagamentos vinculados.'
            );
        }

        $paymentIds = [];
        foreach ($rawPayments as $rawPayment) {
            $paymentId = is_array($rawPayment)
                ? (string) ($rawPayment['id'] ?? '')
                : (string) $rawPayment;
            if (!preg_match('/^[0-9]{1,32}$/', $paymentId)) {
                throw new \RuntimeException(
                    'Contestacao canonica possui pagamento invalido.'
                );
            }
            if (!in_array($paymentId, $paymentIds, true)) {
                $paymentIds[] = $paymentId;
            }
        }
        return $paymentIds;
    }

    /**
     * A chargeback is authoritative evidence of a dispute. It has a dedicated
     * path so reconciliation can never grant a license immediately before
     * revoking it because the payment resource has not changed status yet.
     *
     * @param array<string, mixed> $chargeback
     * @param array<string, mixed> $payment
     * @param array<string, mixed> $merchantOrder
     */
    private function applyCanonicalChargeback(
        array $chargeback,
        array $payment,
        array $merchantOrder
    ): void {
        $chargebackId = (string) ($chargeback['id'] ?? '');
        $chargebackStatus = (string) ($chargeback['status'] ?? 'received');
        $chargebackCurrency = strtoupper((string) (
            $chargeback['currency']
                ?? $chargeback['currency_id']
                ?? ''
        ));
        $chargebackAmountCents = Money::toCents(
            $chargeback['amount'] ?? ''
        );

        $paymentId = (string) ($payment['id'] ?? '');
        $orderId = (string) ($payment['external_reference'] ?? '');
        $merchantOrderId = (string) ($payment['order']['id'] ?? '');
        $paymentOrderType = (string) ($payment['order']['type'] ?? '');
        $paymentCollectorId = (string) ($payment['collector_id'] ?? '');
        $paymentCurrency = strtoupper((string) ($payment['currency_id'] ?? ''));
        $paymentAmountCents = Money::toCents(
            $payment['transaction_amount'] ?? ''
        );
        $paymentRefundedCents = Money::toCents(
            $payment['transaction_amount_refunded']
                ?? $payment['amount_refunded']
                ?? 0
        );

        $canonicalMerchantOrderId = (string) ($merchantOrder['id'] ?? '');
        $merchantExternalReference = (string) (
            $merchantOrder['external_reference'] ?? ''
        );
        $preferenceId = (string) ($merchantOrder['preference_id'] ?? '');
        $merchantCollectorId = (string) (
            $merchantOrder['collector']['id'] ?? ''
        );
        $applicationId = (string) ($merchantOrder['application_id'] ?? '');
        $merchantTotalCents = Money::toCents(
            $merchantOrder['total_amount'] ?? ''
        );

        if (!preg_match('/^[A-Za-z0-9._:-]{1,128}$/', $chargebackId)
            || !preg_match('/^[A-Za-z0-9._:-]{1,48}$/', $chargebackStatus)
            || !preg_match('/^[0-9]{1,32}$/', $paymentId)
            || !in_array(
                $paymentId,
                $this->chargebackPaymentIds($chargeback),
                true
            )
            || !preg_match('/^[a-f0-9-]{36}$/i', $orderId)
            || !preg_match('/^[0-9]{1,32}$/', $merchantOrderId)
            || !hash_equals($merchantOrderId, $canonicalMerchantOrderId)
            || !hash_equals($orderId, $merchantExternalReference)
            || $paymentOrderType !== 'mercadopago'
            || $preferenceId === ''
            || $chargebackAmountCents <= 0
            || $paymentAmountCents <= 0
            || $chargebackAmountCents > $paymentAmountCents
            || $paymentRefundedCents < 0
            || $paymentRefundedCents > $paymentAmountCents
            || !hash_equals($chargebackCurrency, $paymentCurrency)) {
            throw new \RuntimeException(
                'Contestacao canonica nao corresponde ao pagamento.'
            );
        }

        $configuredCollector = $this->config->string(
            'mercado_pago.collector_id'
        );
        if ($merchantCollectorId === ''
            || !hash_equals($paymentCollectorId, $merchantCollectorId)
            || ($configuredCollector !== ''
                && !hash_equals($configuredCollector, $merchantCollectorId))) {
            throw new \RuntimeException(
                'Recebedor da contestacao nao corresponde.'
            );
        }
        $configuredApplication = $this->config->string(
            'mercado_pago.application_id'
        );
        if ($configuredApplication !== ''
            && !hash_equals($configuredApplication, $applicationId)) {
            throw new \RuntimeException(
                'Aplicacao da contestacao nao corresponde.'
            );
        }

        $approvedAt = self::mysqlDate(
            (string) ($payment['date_approved'] ?? '')
        );
        $fingerprint = hash('sha256', json_encode([
            'chargeback_id' => $chargebackId,
            'chargeback_status' => $chargebackStatus,
            'chargeback_amount_cents' => $chargebackAmountCents,
            'merchant_order_id' => $merchantOrderId,
            'payment_id' => $paymentId,
            'payment_amount_cents' => $paymentAmountCents,
            'currency' => $paymentCurrency,
        ], JSON_THROW_ON_ERROR));

        $this->database->transaction(function (PDO $pdo) use (
            $orderId,
            $preferenceId,
            $merchantOrderId,
            $merchantTotalCents,
            $paymentId,
            $paymentAmountCents,
            $paymentRefundedCents,
            $paymentCurrency,
            $approvedAt,
            $fingerprint,
            $chargebackId,
            $chargebackStatus,
            $chargebackAmountCents
        ): void {
            $select = $pdo->prepare(
                'SELECT * FROM orders WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $orderId]);
            $order = $select->fetch();
            if (!is_array($order)
                || (string) $order['provider'] !== 'mercado_pago'
                || (int) $order['expected_amount_cents'] !== $merchantTotalCents
                || !hash_equals(
                    strtoupper((string) $order['currency']),
                    $paymentCurrency
                )
                || !hash_equals(
                    (string) $order['provider_preference_id'],
                    $preferenceId
                )
                || ($order['provider_order_id'] !== null
                    && !hash_equals(
                        (string) $order['provider_order_id'],
                        $merchantOrderId
                    ))) {
                throw new \RuntimeException(
                    'Contestacao nao corresponde a compra local.'
                );
            }

            $mapping = $pdo->prepare(
                "SELECT order_id FROM payments
                 WHERE provider = 'mercado_pago'
                   AND provider_payment_id = :provider_payment_id
                 LIMIT 1 FOR UPDATE"
            );
            $mapping->execute(['provider_payment_id' => $paymentId]);
            $mappedOrderId = $mapping->fetchColumn();
            if (is_string($mappedOrderId)
                && !hash_equals($orderId, $mappedOrderId)) {
                throw new \RuntimeException(
                    'Pagamento contestado ja pertence a outra compra.'
                );
            }

            $pdo->prepare(
                "INSERT INTO payments
                    (id, order_id, provider, provider_payment_id, amount_cents,
                     refunded_amount_cents, currency, status, raw_fingerprint,
                     approved_at)
                 VALUES
                    (:id, :order_id, 'mercado_pago', :provider_payment_id,
                     :amount_cents, :refunded_amount_cents, :currency,
                     'charged_back', :raw_fingerprint, :approved_at)
                 ON DUPLICATE KEY UPDATE
                    amount_cents = VALUES(amount_cents),
                    refunded_amount_cents = VALUES(refunded_amount_cents),
                    currency = VALUES(currency),
                    status = 'charged_back',
                    raw_fingerprint = VALUES(raw_fingerprint),
                    approved_at = COALESCE(VALUES(approved_at), approved_at)"
            )->execute([
                'id' => Crypto::uuid(),
                'order_id' => $orderId,
                'provider_payment_id' => $paymentId,
                'amount_cents' => $paymentAmountCents,
                'refunded_amount_cents' => $paymentRefundedCents,
                'currency' => $paymentCurrency,
                'raw_fingerprint' => $fingerprint,
                'approved_at' => $approvedAt,
            ]);

            $providerStatus = mb_substr(
                'chargeback/' . $chargebackStatus,
                0,
                64
            );
            $reviewIsSticky = (string) $order['status'] === 'paid_review';
            $pdo->prepare(
                "UPDATE orders
                 SET status = :status,
                     provider_order_id = :provider_order_id,
                     provider_payment_id = :payment_id,
                     provider_status = :provider_status,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6),
                     reserves_checkout_slot = :reserves_checkout_slot
                  WHERE id = :id"
            )->execute([
                'status' => $reviewIsSticky
                    ? 'paid_review'
                    : 'charged_back',
                'provider_order_id' => $merchantOrderId,
                'payment_id' => $paymentId,
                'provider_status' => $reviewIsSticky
                    ? (string) $order['provider_status']
                    : $providerStatus,
                'reserves_checkout_slot' => $reviewIsSticky
                    ? (int) $order['reserves_checkout_slot']
                    : 0,
                'id' => $orderId,
            ]);

            $licenseSelect = $pdo->prepare(
                'SELECT id FROM licenses
                 WHERE order_id = :order_id LIMIT 1 FOR UPDATE'
            );
            $licenseSelect->execute(['order_id' => $orderId]);
            $licenseId = $licenseSelect->fetchColumn();
            if (is_string($licenseId)) {
                $this->revokeLicense($pdo, $licenseId, 'charged_back');
            }

            AuditLog::recordWithPdo(
                $pdo,
                'payment.charged_back',
                null,
                (string) $order['user_id'],
                is_string($licenseId) ? $licenseId : null,
                null,
                [
                    'order_id' => $orderId,
                    'merchant_order_id' => $merchantOrderId,
                    'payment_id' => $paymentId,
                    'chargeback_id' => $chargebackId,
                    'chargeback_amount_cents' => $chargebackAmountCents,
                ]
            );
        });
    }

    /**
     * Validates every payment in a chargeback before making one atomic state
     * transition. The provider reports one aggregate disputed amount even when
     * the chargeback references split payments.
     *
     * @param array<string, mixed> $chargeback
     * @param list<array{
     *   payment: array<string, mixed>,
     *   merchant_order: array<string, mixed>
     * }> $resources
     */
    private function applyCanonicalChargebackBatch(
        array $chargeback,
        array $resources
    ): void {
        $chargebackId = (string) ($chargeback['id'] ?? '');
        $chargebackStatus = (string) ($chargeback['status'] ?? 'received');
        $chargebackCurrency = strtoupper((string) (
            $chargeback['currency']
                ?? $chargeback['currency_id']
                ?? ''
        ));
        $chargebackAmountCents = Money::toCents(
            $chargeback['amount'] ?? ''
        );
        $expectedPaymentIds = $this->chargebackPaymentIds($chargeback);
        if (!preg_match('/^[A-Za-z0-9._:-]{1,128}$/', $chargebackId)
            || !preg_match(
                '/^[A-Za-z0-9._:-]{1,48}$/',
                $chargebackStatus
            )
            || !preg_match('/^[A-Z]{3}$/', $chargebackCurrency)
            || $chargebackAmountCents <= 0
            || count($resources) !== count($expectedPaymentIds)) {
            throw new \RuntimeException(
                'Contestacao canonica agregada e invalida.'
            );
        }

        $configuredCollector = $this->config->string(
            'mercado_pago.collector_id'
        );
        $configuredApplication = $this->config->string(
            'mercado_pago.application_id'
        );
        $orderId = null;
        $preferenceId = null;
        $merchantTotalCents = null;
        $paymentAmountTotalCents = 0;
        $paymentRows = [];
        $merchantOrderIds = [];
        $seenPaymentIds = [];

        foreach ($resources as $resource) {
            $payment = $resource['payment'] ?? null;
            $merchantOrder = $resource['merchant_order'] ?? null;
            if (!is_array($payment) || !is_array($merchantOrder)) {
                throw new \RuntimeException(
                    'Recurso canonico da contestacao e invalido.'
                );
            }

            $paymentId = (string) ($payment['id'] ?? '');
            $candidateOrderId = (string) (
                $payment['external_reference'] ?? ''
            );
            $merchantOrderId = (string) (
                $payment['order']['id'] ?? ''
            );
            $paymentOrderType = (string) (
                $payment['order']['type'] ?? ''
            );
            $paymentCollectorId = (string) (
                $payment['collector_id'] ?? ''
            );
            $paymentCurrency = strtoupper((string) (
                $payment['currency_id'] ?? ''
            ));
            $paymentAmountCents = Money::toCents(
                $payment['transaction_amount'] ?? ''
            );
            $paymentRefundedCents = Money::toCents(
                $payment['transaction_amount_refunded']
                    ?? $payment['amount_refunded']
                    ?? 0
            );

            $canonicalMerchantOrderId = (string) (
                $merchantOrder['id'] ?? ''
            );
            $merchantExternalReference = (string) (
                $merchantOrder['external_reference'] ?? ''
            );
            $candidatePreferenceId = (string) (
                $merchantOrder['preference_id'] ?? ''
            );
            $merchantCollectorId = (string) (
                $merchantOrder['collector']['id'] ?? ''
            );
            $applicationId = (string) (
                $merchantOrder['application_id'] ?? ''
            );
            $candidateMerchantTotalCents = Money::toCents(
                $merchantOrder['total_amount'] ?? ''
            );

            if (!preg_match('/^[0-9]{1,32}$/', $paymentId)
                || !in_array($paymentId, $expectedPaymentIds, true)
                || in_array($paymentId, $seenPaymentIds, true)
                || !preg_match(
                    '/^[a-f0-9-]{36}$/i',
                    $candidateOrderId
                )
                || !preg_match('/^[0-9]{1,32}$/', $merchantOrderId)
                || !hash_equals(
                    $merchantOrderId,
                    $canonicalMerchantOrderId
                )
                || !hash_equals(
                    $candidateOrderId,
                    $merchantExternalReference
                )
                || $paymentOrderType !== 'mercadopago'
                || $candidatePreferenceId === ''
                || $paymentAmountCents <= 0
                || $paymentRefundedCents < 0
                || $paymentRefundedCents > $paymentAmountCents
                || !hash_equals(
                    $chargebackCurrency,
                    $paymentCurrency
                )
                || $merchantCollectorId === ''
                || !hash_equals(
                    $paymentCollectorId,
                    $merchantCollectorId
                )
                || ($configuredCollector !== ''
                    && !hash_equals(
                        $configuredCollector,
                        $merchantCollectorId
                    ))
                || ($configuredApplication !== ''
                    && !hash_equals(
                        $configuredApplication,
                        $applicationId
                    ))) {
                throw new \RuntimeException(
                    'Contestacao canonica nao corresponde aos pagamentos.'
                );
            }
            if (($orderId !== null
                    && !hash_equals($orderId, $candidateOrderId))
                || ($preferenceId !== null
                    && !hash_equals(
                        $preferenceId,
                        $candidatePreferenceId
                    ))
                || ($merchantTotalCents !== null
                    && $merchantTotalCents
                        !== $candidateMerchantTotalCents)) {
                throw new \RuntimeException(
                    'Pagamentos contestados pertencem a compras distintas.'
                );
            }

            $orderId ??= $candidateOrderId;
            $preferenceId ??= $candidatePreferenceId;
            $merchantTotalCents ??= $candidateMerchantTotalCents;
            $seenPaymentIds[] = $paymentId;
            $merchantOrderIds[$merchantOrderId] = true;
            $paymentAmountTotalCents += $paymentAmountCents;
            $approvedAt = self::mysqlDate(
                (string) ($payment['date_approved'] ?? '')
            );
            $paymentRows[] = [
                'provider_payment_id' => $paymentId,
                'merchant_order_id' => $merchantOrderId,
                'amount_cents' => $paymentAmountCents,
                'refunded_amount_cents' => $paymentRefundedCents,
                'currency' => $paymentCurrency,
                'approved_at' => $approvedAt,
                'fingerprint' => hash('sha256', json_encode([
                    'chargeback_id' => $chargebackId,
                    'chargeback_status' => $chargebackStatus,
                    'chargeback_amount_cents' => $chargebackAmountCents,
                    'merchant_order_id' => $merchantOrderId,
                    'payment_id' => $paymentId,
                    'payment_amount_cents' => $paymentAmountCents,
                    'currency' => $paymentCurrency,
                ], JSON_THROW_ON_ERROR)),
            ];
        }

        $seenIds = $seenPaymentIds;
        sort($seenIds);
        $expectedIds = $expectedPaymentIds;
        sort($expectedIds);
        if ($seenIds !== $expectedIds
            || $orderId === null
            || $preferenceId === null
            || $merchantTotalCents === null
            || $chargebackAmountCents > $paymentAmountTotalCents) {
            throw new \RuntimeException(
                'Valor ou pagamentos da contestacao agregada divergem.'
            );
        }

        $this->database->transaction(function (PDO $pdo) use (
            $orderId,
            $preferenceId,
            $merchantTotalCents,
            $paymentRows,
            $merchantOrderIds,
            $chargebackId,
            $chargebackStatus,
            $chargebackAmountCents,
            $chargebackCurrency
        ): void {
            $select = $pdo->prepare(
                'SELECT * FROM orders WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $orderId]);
            $order = $select->fetch();
            $reviewIsSticky = is_array($order)
                && (string) $order['status'] === 'paid_review';
            $storedMerchantOrderId = is_array($order)
                ? (string) ($order['provider_order_id'] ?? '')
                : '';
            if (!is_array($order)
                || (string) $order['provider'] !== 'mercado_pago'
                || (int) $order['expected_amount_cents']
                    !== $merchantTotalCents
                || !hash_equals(
                    strtoupper((string) $order['currency']),
                    $chargebackCurrency
                )
                || !hash_equals(
                    (string) $order['provider_preference_id'],
                    $preferenceId
                )
                || (!$reviewIsSticky
                    && $storedMerchantOrderId !== ''
                    && (count($merchantOrderIds) !== 1
                        || !isset(
                            $merchantOrderIds[$storedMerchantOrderId]
                        )))) {
                throw new \RuntimeException(
                    'Contestacao agregada nao corresponde a compra local.'
                );
            }

            foreach ($paymentRows as $paymentRow) {
                $mapping = $pdo->prepare(
                    "SELECT order_id FROM payments
                     WHERE provider = 'mercado_pago'
                       AND provider_payment_id = :provider_payment_id
                     LIMIT 1 FOR UPDATE"
                );
                $mapping->execute([
                    'provider_payment_id' =>
                        $paymentRow['provider_payment_id'],
                ]);
                $mappedOrderId = $mapping->fetchColumn();
                if (is_string($mappedOrderId)
                    && !hash_equals($orderId, $mappedOrderId)) {
                    throw new \RuntimeException(
                        'Pagamento contestado ja pertence a outra compra.'
                    );
                }
                $pdo->prepare(
                    "INSERT INTO payments
                        (id, order_id, provider, provider_payment_id,
                         amount_cents, refunded_amount_cents, currency,
                         status, raw_fingerprint, approved_at)
                     VALUES
                        (:id, :order_id, 'mercado_pago',
                         :provider_payment_id, :amount_cents,
                         :refunded_amount_cents, :currency,
                         'charged_back', :raw_fingerprint, :approved_at)
                     ON DUPLICATE KEY UPDATE
                        amount_cents = VALUES(amount_cents),
                        refunded_amount_cents =
                            VALUES(refunded_amount_cents),
                        currency = VALUES(currency),
                        status = 'charged_back',
                        raw_fingerprint = VALUES(raw_fingerprint),
                        approved_at =
                            COALESCE(VALUES(approved_at), approved_at)"
                )->execute([
                    'id' => Crypto::uuid(),
                    'order_id' => $orderId,
                    'provider_payment_id' =>
                        $paymentRow['provider_payment_id'],
                    'amount_cents' => $paymentRow['amount_cents'],
                    'refunded_amount_cents' =>
                        $paymentRow['refunded_amount_cents'],
                    'currency' => $paymentRow['currency'],
                    'raw_fingerprint' => $paymentRow['fingerprint'],
                    'approved_at' => $paymentRow['approved_at'],
                ]);
            }

            $firstPayment = $paymentRows[0];
            $firstMerchantOrderId = (string) (
                $firstPayment['merchant_order_id'] ?? ''
            );
            $providerStatus = mb_substr(
                'chargeback/' . $chargebackStatus,
                0,
                64
            );
            $pdo->prepare(
                "UPDATE orders
                 SET status = :status,
                     provider_order_id = :provider_order_id,
                     provider_payment_id = :payment_id,
                     provider_status = :provider_status,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6),
                     reserves_checkout_slot = :reserves_checkout_slot
                 WHERE id = :id"
            )->execute([
                'status' => $reviewIsSticky
                    ? 'paid_review'
                    : 'charged_back',
                'provider_order_id' => $reviewIsSticky
                    ? $order['provider_order_id']
                    : $firstMerchantOrderId,
                'payment_id' => $reviewIsSticky
                    ? $order['provider_payment_id']
                    : $firstPayment['provider_payment_id'],
                'provider_status' => $reviewIsSticky
                    ? $order['provider_status']
                    : $providerStatus,
                'reserves_checkout_slot' => $reviewIsSticky
                    ? (int) $order['reserves_checkout_slot']
                    : 0,
                'id' => $orderId,
            ]);

            $licenseSelect = $pdo->prepare(
                'SELECT id FROM licenses
                 WHERE order_id = :order_id LIMIT 1 FOR UPDATE'
            );
            $licenseSelect->execute(['order_id' => $orderId]);
            $licenseId = $licenseSelect->fetchColumn();
            $coverageCents = $reviewIsSticky
                ? $this->safeCoveredCents($pdo, $orderId)
                : 0;
            $reviewHasDispute = $reviewIsSticky
                && $this->hasDisputedPayment($pdo, $orderId);
            if (is_string($licenseId)
                && (!$reviewIsSticky
                    || $reviewHasDispute
                    || $coverageCents
                        < (int) $order['expected_amount_cents'])) {
                $this->revokeLicense(
                    $pdo,
                    $licenseId,
                    'charged_back'
                );
            }

            AuditLog::recordWithPdo(
                $pdo,
                'payment.charged_back',
                null,
                (string) $order['user_id'],
                is_string($licenseId) ? $licenseId : null,
                null,
                [
                    'order_id' => $orderId,
                    'merchant_order_ids' =>
                        array_keys($merchantOrderIds),
                    'payment_ids' => array_column(
                        $paymentRows,
                        'provider_payment_id'
                    ),
                    'chargeback_id' => $chargebackId,
                    'chargeback_amount_cents' => $chargebackAmountCents,
                    'review_coverage_cents' => $reviewIsSticky
                        ? $coverageCents
                        : null,
                    'review_has_dispute' => $reviewIsSticky
                        ? $reviewHasDispute
                        : null,
                ]
            );
        });
    }

    private function safeCoveredCents(PDO $pdo, string $orderId): int
    {
        $coverage = $pdo->prepare(
            "SELECT COALESCE(SUM(
                CASE
                    WHEN status IN ('approved', 'refunded')
                    THEN GREATEST(amount_cents - refunded_amount_cents, 0)
                    ELSE 0
                END
             ), 0)
             FROM payments
             WHERE order_id = :order_id"
        );
        $coverage->execute(['order_id' => $orderId]);
        return (int) $coverage->fetchColumn();
    }

    private function hasDisputedPayment(PDO $pdo, string $orderId): bool
    {
        $disputes = $pdo->prepare(
            "SELECT COUNT(*)
             FROM payments
             WHERE order_id = :order_id
               AND status IN ('in_mediation', 'charged_back')"
        );
        $disputes->execute(['order_id' => $orderId]);
        return (int) $disputes->fetchColumn() > 0;
    }

    private function enforcePaymentReviewCoverage(string $orderId): void
    {
        $this->database->transaction(function (PDO $pdo) use ($orderId): void {
            $select = $pdo->prepare(
                'SELECT user_id, expected_amount_cents
                 FROM orders
                 WHERE id = :id AND status = :status
                 LIMIT 1 FOR UPDATE'
            );
            $select->execute([
                'id' => $orderId,
                'status' => 'paid_review',
            ]);
            $order = $select->fetch();
            if (!is_array($order)) {
                return;
            }

            $coverageCents = $this->safeCoveredCents($pdo, $orderId);
            $hasDispute = $this->hasDisputedPayment($pdo, $orderId);
            $licenseSelect = $pdo->prepare(
                'SELECT id, status, active_slot
                 FROM licenses
                 WHERE order_id = :order_id LIMIT 1 FOR UPDATE'
            );
            $licenseSelect->execute(['order_id' => $orderId]);
            $license = $licenseSelect->fetch();
            if (($coverageCents < (int) $order['expected_amount_cents']
                    || $hasDispute)
                && is_array($license)) {
                $this->revokeLicense(
                    $pdo,
                    (string) $license['id'],
                    'payment_review_underfunded'
                );
            }
            AuditLog::recordWithPdo(
                $pdo,
                'payment.review_coverage_checked',
                null,
                (string) $order['user_id'],
                is_array($license) ? (string) $license['id'] : null,
                null,
                [
                    'order_id' => $orderId,
                    'coverage_cents' => $coverageCents,
                    'has_dispute' => $hasDispute,
                    'expected_amount_cents' =>
                        (int) $order['expected_amount_cents'],
                ]
            );
        });
    }

    private function updatePaidOrder(
        PDO $pdo,
        string $orderId,
        string $merchantOrderId,
        string $paymentId,
        string $providerStatus,
        ?string $approvedAt,
        string $status
    ): void {
        $pdo->prepare(
            'UPDATE orders
             SET status = :status,
                 provider_order_id = :provider_order_id,
                 provider_payment_id = :payment_id,
                 provider_status = :provider_status,
                 paid_at = COALESCE(paid_at, :approved_at, CURRENT_TIMESTAMP(6)),
                 reserves_checkout_slot = CASE
                     WHEN :preserve_checkout_slot = 1
                     THEN reserves_checkout_slot
                     ELSE 0
                 END
             WHERE id = :id'
        )->execute([
            'status' => $status,
            'provider_order_id' => $merchantOrderId,
            'payment_id' => $paymentId,
            'provider_status' => $providerStatus,
            'approved_at' => $approvedAt,
            'preserve_checkout_slot' => $status === 'paid_review' ? 1 : 0,
            'id' => $orderId,
        ]);
    }

    private function revokeLicense(PDO $pdo, string $licenseId, string $reason): void
    {
        $pdo->prepare(
            "UPDATE licenses
             SET status = 'revoked',
                 revoked_at = CURRENT_TIMESTAMP(6), revoke_reason = :reason
             WHERE id = :id"
        )->execute(['reason' => $reason, 'id' => $licenseId]);
        $pdo->prepare(
            "UPDATE activations
             SET status = 'revoked',
                 deactivated_at = CURRENT_TIMESTAMP(6),
                 deactivation_reason = :reason
             WHERE license_id = :license_id AND active_slot = 1"
        )->execute(['reason' => $reason, 'license_id' => $licenseId]);
        $pdo->prepare(
            'UPDATE refresh_tokens
             SET revoked_at = COALESCE(revoked_at, CURRENT_TIMESTAMP(6))
             WHERE license_id = :license_id'
        )->execute(['license_id' => $licenseId]);
    }

    /**
     * @return array{
     *   user_id: string,
     *   order_id: string,
     *   product_code: string,
     *   expected_amount_cents: int,
     *   currency: string,
     *   verification_token: string,
     *   existing_checkout_url: string,
     *   owns_checkout_claim: bool,
     *   recover_preference: bool,
     *   may_create_preference: bool
     * }
     */
    private function prepareAccountAndOrder(
        string $email,
        string $displayName,
        string $password,
        string $idempotencyKey,
        string $checkoutClaimToken,
        Request $request,
        int $conflictRetry = 0
    ): array {
        try {
            return $this->database->transaction(function (PDO $pdo) use (
                $email,
                $displayName,
                $password,
                $idempotencyKey,
                $checkoutClaimToken,
                $request
            ): array {
            $select = $pdo->prepare(
                'SELECT * FROM users WHERE email = :email LIMIT 1 FOR UPDATE'
            );
            $select->execute(['email' => $email]);
            $user = $select->fetch();
            $verificationToken = '';

            if (is_array($user)) {
                if (!password_verify($password, (string) $user['password_hash'])
                    || $user['status'] !== 'active') {
                    throw new ApiException(
                        401,
                        'INVALID_CREDENTIALS',
                        'Nao foi possivel usar esta conta.'
                    );
                }
                $licensed = $pdo->prepare(
                    "SELECT COUNT(*) FROM licenses
                     WHERE user_id = :user_id
                       AND product_code = :product_code
                       AND status = 'active'
                       AND active_slot = 1"
                );
                $licensed->execute([
                    'user_id' => $user['id'],
                    'product_code' => $this->config->string('product.code'),
                ]);
                if ((int) $licensed->fetchColumn() > 0) {
                    throw new ApiException(
                        409,
                        'ALREADY_LICENSED',
                        'Esta conta ja possui uma licenca ativa.'
                    );
                }
                $paymentReview = $pdo->prepare(
                    "SELECT id FROM orders
                     WHERE user_id = :user_id
                       AND product_code = :product_code
                       AND status IN (
                           'paid_review', 'partial_refund',
                           'in_mediation', 'charged_back'
                       )
                     LIMIT 1 FOR UPDATE"
                );
                $paymentReview->execute([
                    'user_id' => $user['id'],
                    'product_code' => $this->config->string('product.code'),
                ]);
                if ($paymentReview->fetchColumn() !== false) {
                    throw new ApiException(
                        409,
                        'PAYMENT_REVIEW_REQUIRED',
                        'Existe um pagamento ou contestacao desta conta em revisao. Fale com o suporte antes de tentar novamente.'
                    );
                }
            } else {
                $user = [
                    'id' => Crypto::uuid(),
                    'email' => $email,
                    'display_name' => $displayName,
                ];
                $pdo->prepare(
                    "INSERT INTO users
                        (id, email, display_name, password_hash, status)
                     VALUES
                        (:id, :email, :display_name, :password_hash, 'active')"
                )->execute([
                    'id' => $user['id'],
                    'email' => $email,
                    'display_name' => $displayName,
                    'password_hash' => PasswordPolicy::hash($password),
                ]);
            }

            $existing = $pdo->prepare(
                'SELECT id, product_code, expected_amount_cents, currency,
                        status, checkout_url, checkout_expires_at,
                        checkout_claim_token, checkout_claimed_at
                 FROM orders
                 WHERE user_id = :user_id AND idempotency_key = :idempotency_key
                 LIMIT 1 FOR UPDATE'
            );
            $existing->execute([
                'user_id' => $user['id'],
                'idempotency_key' => $idempotencyKey,
            ]);
            $order = $existing->fetch();
            $matchedIdempotency = is_array($order);
            if (!is_array($order)) {
                $open = $pdo->prepare(
                    'SELECT id, product_code, expected_amount_cents, currency,
                            status, checkout_url, checkout_expires_at,
                            checkout_claim_token, checkout_claimed_at
                     FROM orders
                     WHERE user_id = :user_id
                       AND product_code = :product_code
                       AND open_slot = 1
                     LIMIT 1 FOR UPDATE'
                );
                $open->execute([
                    'user_id' => $user['id'],
                    'product_code' => $this->config->string('product.code'),
                ]);
                $order = $open->fetch();
            }
            if (is_array($order)) {
                $checkoutUrl = (string) ($order['checkout_url'] ?? '');
                $checkoutExpiry = $order['checkout_expires_at'] === null
                    ? null
                    : strtotime((string) $order['checkout_expires_at']);
                $checkoutExpired = $checkoutUrl !== ''
                    && $checkoutExpiry !== null
                    && $checkoutExpiry <= time();
                if ($checkoutExpired) {
                    throw new ApiException(
                        503,
                        'CHECKOUT_RECONCILIATION_REQUIRED',
                        'Esta compra precisa ser conferida com o provedor antes de uma nova cobranca.'
                    );
                } elseif ($checkoutUrl !== '') {
                    return [
                        'user_id' => (string) $user['id'],
                        'order_id' => (string) $order['id'],
                        'product_code' => (string) $order['product_code'],
                        'expected_amount_cents' =>
                            (int) $order['expected_amount_cents'],
                        'currency' => (string) $order['currency'],
                        'verification_token' => '',
                        'existing_checkout_url' => $checkoutUrl,
                        'owns_checkout_claim' => false,
                        'recover_preference' => false,
                        'may_create_preference' => false,
                    ];
                } elseif ($matchedIdempotency
                    && !in_array(
                        (string) $order['status'],
                        [
                            'creating',
                            'provider_error',
                            'provider_ambiguous',
                            'pending',
                        ],
                        true
                    )) {
                    throw new ApiException(
                        409,
                        'IDEMPOTENCY_KEY_REUSED',
                        'Atualize a pagina antes de iniciar uma nova compra.'
                    );
                }
            }

            if (is_array($order)) {
                $mayCreatePreference =
                    (string) $order['status'] === 'provider_error';
                $claimTimestamp = $order['checkout_claimed_at'] === null
                    ? null
                    : strtotime((string) $order['checkout_claimed_at']);
                $claimIsStale = $claimTimestamp === null
                    || $claimTimestamp <= time()
                        - $this->config->int('checkout.claim_ttl_seconds');
                $ownsClaim = $order['checkout_claim_token'] === null
                    || $claimIsStale;
                if ($ownsClaim) {
                    $pdo->prepare(
                        "UPDATE orders
                         SET status = 'creating',
                             checkout_claim_token = :checkout_claim_token,
                             checkout_claimed_at = CURRENT_TIMESTAMP(6)
                         WHERE id = :id"
                    )->execute([
                        'checkout_claim_token' => $checkoutClaimToken,
                        'id' => $order['id'],
                    ]);
                    if (($user['email_verified_at'] ?? null) === null) {
                        $verificationToken = $this->createVerificationToken(
                            $pdo,
                            (string) $user['id']
                        );
                    }
                }
                return [
                    'user_id' => (string) $user['id'],
                    'order_id' => (string) $order['id'],
                    'product_code' => (string) $order['product_code'],
                    'expected_amount_cents' =>
                        (int) $order['expected_amount_cents'],
                    'currency' => (string) $order['currency'],
                    'verification_token' => $verificationToken,
                    'existing_checkout_url' => '',
                    'owns_checkout_claim' => $ownsClaim,
                    'recover_preference' => true,
                    'may_create_preference' => $mayCreatePreference,
                ];
            }

            if (($user['email_verified_at'] ?? null) === null) {
                $verificationToken = $this->createVerificationToken(
                    $pdo,
                    (string) $user['id']
                );
            }

            $orderId = Crypto::uuid();
            $productCode = $this->config->string('product.code');
            $expectedAmountCents = $this->config->int('product.price_cents');
            $currency = $this->config->string('product.currency');
            $pdo->prepare(
                "INSERT INTO orders
                    (id, user_id, product_code, provider, idempotency_key,
                     expected_amount_cents, currency, status,
                     reserves_checkout_slot,
                     checkout_claim_token, checkout_claimed_at,
                     terms_version, privacy_version, accepted_at,
                     acceptance_ip_hash)
                 VALUES
                    (:id, :user_id, :product_code, 'mercado_pago', :idempotency_key,
                     :amount_cents, :currency, 'creating', 1,
                     :checkout_claim_token, CURRENT_TIMESTAMP(6),
                     :terms_version, :privacy_version, CURRENT_TIMESTAMP(6),
                     :acceptance_ip_hash)"
            )->execute([
                'id' => $orderId,
                'user_id' => $user['id'],
                'product_code' => $productCode,
                'idempotency_key' => $idempotencyKey,
                'amount_cents' => $expectedAmountCents,
                'currency' => $currency,
                'checkout_claim_token' => $checkoutClaimToken,
                'terms_version' => $this->config->string('legal.terms_version'),
                'privacy_version' => $this->config->string('legal.privacy_version'),
                'acceptance_ip_hash' => $this->crypto->hashIp($request->ip),
            ]);
            AuditLog::recordWithPdo(
                $pdo,
                'checkout.created',
                $request->requestId,
                (string) $user['id'],
                null,
                $this->crypto->hashIp($request->ip),
                [
                    'order_id' => $orderId,
                    'terms_version' => $this->config->string('legal.terms_version'),
                    'privacy_version' => $this->config->string('legal.privacy_version'),
                ]
            );

                return [
                    'user_id' => (string) $user['id'],
                    'order_id' => $orderId,
                    'product_code' => $productCode,
                    'expected_amount_cents' => $expectedAmountCents,
                    'currency' => $currency,
                    'verification_token' => $verificationToken,
                    'existing_checkout_url' => '',
                    'owns_checkout_claim' => true,
                    'recover_preference' => false,
                    'may_create_preference' => true,
                ];
            });
        } catch (PDOException $error) {
            if ((string) $error->getCode() === '23000' && $conflictRetry === 0) {
                return $this->prepareAccountAndOrder(
                    $email,
                    $displayName,
                    $password,
                    $idempotencyKey,
                    $checkoutClaimToken,
                    $request,
                    1
                );
            }
            throw $error;
        }
    }

    /** @param array<string, mixed> $preference */
    private function assertPreferenceMatchesOrder(
        array $preference,
        string $orderId,
        string $productCode,
        string $currency,
        int $expectedAmountCents
    ): void {
        if (!hash_equals(
            $orderId,
            (string) ($preference['external_reference'] ?? '')
        )) {
            throw new \RuntimeException(
                'Preferencia nao corresponde a compra local.'
            );
        }
        $collectorId = (string) ($preference['collector_id'] ?? '');
        $configuredCollector = $this->config->string('mercado_pago.collector_id');
        if ($configuredCollector !== ''
            && !hash_equals($configuredCollector, $collectorId)) {
            throw new \RuntimeException(
                'Recebedor da preferencia nao corresponde.'
            );
        }
        $items = $preference['items'] ?? null;
        if (!is_array($items) || count($items) !== 1 || !is_array($items[0])) {
            throw new \RuntimeException('Itens da preferencia inconsistentes.');
        }
        $item = $items[0];
        if (!hash_equals(
                $productCode,
                (string) ($item['id'] ?? '')
            )
            || (int) ($item['quantity'] ?? 0) !== 1
            || !hash_equals(
                $currency,
                strtoupper((string) ($item['currency_id'] ?? ''))
            )
            || Money::toCents($item['unit_price'] ?? '')
                !== $expectedAmountCents) {
            throw new \RuntimeException(
                'Produto ou valor da preferencia nao corresponde.'
            );
        }
    }

    private function createVerificationToken(PDO $pdo, string $userId): string
    {
        $verificationToken = Crypto::randomToken(32);
        $pdo->prepare(
            "UPDATE user_tokens SET used_at = CURRENT_TIMESTAMP(6)
             WHERE user_id = :user_id
               AND type = 'email_verification' AND used_at IS NULL"
        )->execute(['user_id' => $userId]);
        $pdo->prepare(
            "INSERT INTO user_tokens
                (id, user_id, type, token_hash, expires_at)
             VALUES
                (:id, :user_id, 'email_verification', :token_hash,
                 DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 24 HOUR))"
        )->execute([
            'id' => Crypto::uuid(),
            'user_id' => $userId,
            'token_hash' => Crypto::tokenHash($verificationToken),
        ]);
        return $verificationToken;
    }

    private static function mysqlDate(string $value): ?string
    {
        if ($value === '') {
            return null;
        }
        $timestamp = strtotime($value);
        return $timestamp === false ? null : gmdate('Y-m-d H:i:s', $timestamp);
    }
}
