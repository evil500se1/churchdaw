<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;
use PDOException;

final class WooviPaymentService
{
    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto,
        private readonly RateLimiter $rateLimiter,
        private readonly WooviProvider $woovi,
        private readonly Mailer $mailer
    ) {
    }

    /** @return array<string, mixed> */
    public function startCheckout(Request $request): array
    {
        $this->rateLimiter->assertAllowed(
            'woovi-checkout-ip',
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

        $plan = $this->plan($request->string('plan_code', 32));
        $email = AuthService::normaliseEmail($request->string('email', 254));
        $displayName = preg_replace(
            '/\s+/',
            ' ',
            $request->string('display_name', 96)
        ) ?? '';
        if (mb_strlen($displayName) < 2) {
            throw new ApiException(422, 'INVALID_NAME', 'Informe seu nome.');
        }
        $this->rateLimiter->assertAllowed(
            'woovi-checkout-email',
            hash('sha256', $email),
            10,
            900
        );

        $password = $request->string('password', 128);
        $checkoutToken = Crypto::randomToken(32);
        try {
            PasswordPolicy::assertValid($password, $email);
            $prepared = $this->prepareOrder(
                $email,
                $displayName,
                $password,
                $idempotencyKey,
                $checkoutToken,
                $plan,
                $request
            );
        } finally {
            sodium_memzero($password);
        }

        if (($prepared['existing'] ?? false) === true) {
            return $this->checkoutPayload($prepared, $checkoutToken);
        }

        $orderId = (string) $prepared['order_id'];
        if (($prepared['resume_existing'] ?? false) === true
            && ($prepared['retry_create'] ?? false) !== true) {
            try {
                $this->reconcileOrder($orderId);
            } catch (\Throwable $error) {
                error_log(
                    'ChurchDAW Woovi retomada: ' . $error->getMessage()
                );
                throw new ApiException(
                    503,
                    'PAYMENT_RECONCILIATION_PENDING',
                    'A cobranca anterior ainda esta sendo confirmada. Tente novamente em instantes.'
                );
            }
            $recovered = $this->authorisedOrder($orderId, $checkoutToken);
            if ((string) $recovered['status'] === 'expired') {
                throw new ApiException(
                    409,
                    'PIX_EXPIRED',
                    'O Pix anterior expirou. Inicie uma nova compra.'
                );
            }
            if ((string) $recovered['status'] === 'paid'
                || (string) ($recovered['checkout_code'] ?? '') !== '') {
                $recovered['order_id'] = (string) $recovered['id'];
                return $this->checkoutPayload($recovered, $checkoutToken);
            }
            throw new ApiException(
                503,
                'PAYMENT_RECONCILIATION_PENDING',
                'A cobranca anterior ainda esta sendo confirmada. Tente novamente em instantes.'
            );
        }
        if (($prepared['retry_create'] ?? false) === true) {
            $claimed = $this->database->pdo()->prepare(
                "UPDATE orders SET status = 'creating'
                 WHERE id = :id AND provider = 'woovi'
                   AND status = 'provider_error'"
            );
            $claimed->execute(['id' => $orderId]);
            if ($claimed->rowCount() !== 1) {
                throw new ApiException(
                    409,
                    'CHECKOUT_STATE_CHANGED',
                    'A compra mudou de estado. Atualize a pagina.'
                );
            }
        }
        try {
            $response = $this->woovi->createCharge([
                'correlationID' => $orderId,
                'value' => (int) $plan['price_cents'],
                'type' => 'DYNAMIC',
                'expiresIn' =>
                    $this->config->int('woovi.charge_expiration_minutes') * 60,
                'comment' => 'ChurchDAW - ' . (string) $plan['name'],
                'customer' => [
                    'name' => $displayName,
                    'email' => $email,
                    'correlationID' => (string) $prepared['user_id'],
                ],
                'additionalInfo' => [[
                    'key' => 'plan_code',
                    'value' => (string) $plan['code'],
                ]],
            ]);
            $charge = $this->normaliseCharge($response);
            $this->assertChargeMatches(
                $charge,
                $orderId,
                (int) $plan['price_cents'],
                false
            );
            $brCode = (string) (
                $charge['brCode'] ?? $response['brCode'] ?? ''
            );
            $checkoutUrl = (string) ($charge['paymentLinkUrl'] ?? '');
            $imageUrl = (string) ($charge['qrCodeImage'] ?? '');
            if (strlen($brCode) < 40
                || !$this->safeWooviUrl($checkoutUrl)
                || ($imageUrl !== '' && !$this->safeWooviUrl($imageUrl))) {
                throw new \RuntimeException('Cobranca Woovi incompleta.');
            }
            $providerId = (string) (
                $charge['globalID'] ?? $charge['identifier'] ?? $orderId
            );
            if (mb_strlen($providerId) > 128) {
                throw new \RuntimeException('Identificador Woovi invalido.');
            }
            $expiresAt = self::mysqlDate((string) ($charge['expiresDate'] ?? ''))
                ?? gmdate(
                    'Y-m-d H:i:s',
                    time() + $this->config->int(
                        'woovi.charge_expiration_minutes'
                    ) * 60
                );
            $update = $this->database->pdo()->prepare(
                "UPDATE orders
                 SET provider_preference_id = :provider_id,
                     provider_status = :provider_status,
                     checkout_url = :checkout_url,
                     checkout_code = :checkout_code,
                     checkout_image_url = :checkout_image_url,
                     checkout_expires_at = :checkout_expires_at,
                     status = 'pending',
                     checkout_claim_token = NULL,
                     checkout_claimed_at = NULL
                 WHERE id = :id AND provider = 'woovi'
                   AND status = 'creating'"
            );
            $update->execute([
                'provider_id' => $providerId,
                'provider_status' => strtoupper(
                    (string) ($charge['status'] ?? 'ACTIVE')
                ),
                'checkout_url' => $checkoutUrl,
                'checkout_code' => $brCode,
                'checkout_image_url' => $imageUrl,
                'checkout_expires_at' => $expiresAt,
                'id' => $orderId,
            ]);
            if ($update->rowCount() !== 1) {
                throw new \RuntimeException('A cobranca perdeu seu estado local.');
            }
        } catch (\Throwable $error) {
            $ambiguous = !($error instanceof PaymentProviderException)
                || $error->operationMayHaveSucceeded;
            $this->database->pdo()->prepare(
                "UPDATE orders
                 SET status = :status, checkout_claim_token = NULL,
                     checkout_claimed_at = NULL
                 WHERE id = :id AND provider = 'woovi'
                   AND status = 'creating'"
            )->execute([
                'status' => $ambiguous ? 'provider_ambiguous' : 'provider_error',
                'id' => $orderId,
            ]);
            throw $error;
        }

        if (($prepared['verification_token'] ?? '') !== '') {
            try {
                $this->mailer->sendVerification(
                    $email,
                    $displayName,
                    (string) $prepared['verification_token']
                );
            } catch (\Throwable) {
                error_log('ChurchDAW: falha ao enfileirar verificacao Woovi.');
            }
        }

        $prepared['checkout_url'] = $checkoutUrl;
        $prepared['checkout_code'] = $brCode;
        $prepared['checkout_image_url'] = $imageUrl;
        $prepared['checkout_expires_at'] = $expiresAt;
        $prepared['status'] = 'pending';
        return $this->checkoutPayload($prepared, $checkoutToken);
    }

    /** @return array<string, mixed> */
    public function status(Request $request): array
    {
        $orderId = $request->string('order_id', 36);
        $token = $request->string('checkout_token', 128);
        if (!preg_match('/^[a-f0-9-]{36}$/i', $orderId)
            || !preg_match('/^[A-Za-z0-9_-]{32,128}$/', $token)) {
            throw new ApiException(404, 'ORDER_NOT_FOUND', 'Compra nao encontrada.');
        }
        $this->rateLimiter->assertAllowed(
            'woovi-status-order',
            hash('sha256', $orderId . '|' . $request->ip),
            30,
            60
        );
        $order = $this->authorisedOrder($orderId, $token);
        $lastCheck = strtotime((string) ($order['provider_reconciled_at'] ?? ''));
        if (in_array(
            (string) $order['status'],
            ['creating', 'pending', 'provider_ambiguous'],
            true
        ) && ($lastCheck === false || $lastCheck <= time() - 3)) {
            $this->reconcileOrder($orderId);
            $order = $this->authorisedOrder($orderId, $token);
        }
        return $this->publicStatus($order);
    }

    /** @return string|null correlationId gravado, para liberacao imediata */
    public function receiveWebhook(Request $request): ?string
    {
        // CHECKOUT_ENABLED controla somente a criacao de novas cobrancas.
        // Uma cobranca aberta antes de uma pausa operacional ainda pode ser
        // paga; descartar seu webhook atrasaria a licenca ate a conciliacao e
        // poderia fazer o cliente pensar que o Pix confirmado foi perdido.
        $signature = $request->header('x-webhook-signature');
        if ($signature === '') {
            $expectedAuthorization = $this->config->string(
                'woovi.webhook_authorization'
            );
            $providedAuthorization = $request->header('authorization');
            if ($providedAuthorization === '') {
                $providedAuthorization = $request->header(
                    'x-openpix-authorization'
                );
            }
            if ($expectedAuthorization !== ''
                && $providedAuthorization !== ''
                && hash_equals(
                    $expectedAuthorization,
                    $providedAuthorization
                )) {
                // Handshake autenticado usado pela Woovi ao cadastrar o
                // endpoint. Ele nunca e salvo ou processado como pagamento.
                return null;
            }
        }
        if (!$this->woovi->verifyWebhookSignature(
            $request->rawBody,
            $signature
        )) {
            throw new ApiException(
                401,
                'INVALID_WEBHOOK_SIGNATURE',
                'Assinatura invalida.'
            );
        }
        $event = strtoupper((string) ($request->body['event'] ?? ''));
        $acceptedEvents = [
            'OPENPIX:CHARGE_COMPLETED',
            'WOOVI:CHARGE_COMPLETED',
            'OPENPIX:CHARGE_EXPIRED',
            'WOOVI:CHARGE_EXPIRED',
        ];
        $charge = $request->body['charge'] ?? null;
        // Ao cadastrar um webhook, a Woovi envia um payload assinado contendo
        // event + data_criacao, mas sem charge. Esse probe precisa responder
        // 200 e nunca pode entrar na fila financeira.
        if (!is_array($charge)
            && in_array($event, $acceptedEvents, true)
            && $this->isWebhookRegistrationProbe($request->body)) {
            return null;
        }
        $correlationId = is_array($charge)
            ? (string) ($charge['correlationID'] ?? '') : '';
        if (!in_array($event, $acceptedEvents, true)
            || !preg_match('/^[a-f0-9-]{36}$/i', $correlationId)) {
            throw new ApiException(400, 'INVALID_WEBHOOK', 'Evento invalido.');
        }
        $transactionId = is_array($charge)
            ? (string) ($charge['transactionID'] ?? '') : '';
        $deliveryId = hash(
            'sha256',
            $event . '|' . $correlationId . '|' . $transactionId
                . '|' . $request->rawBody
        );
        try {
            $this->database->pdo()->prepare(
                "INSERT INTO webhook_events
                    (id, provider, delivery_id, resource_id,
                     event_type, payload_json)
                 VALUES
                    (:id, 'woovi', :delivery_id, :resource_id,
                     :event_type, :payload_json)"
            )->execute([
                'id' => Crypto::uuid(),
                'delivery_id' => $deliveryId,
                'resource_id' => $correlationId,
                'event_type' => $event,
                'payload_json' => $request->rawBody,
            ]);
        } catch (PDOException $error) {
            if ((string) $error->getCode() !== '23000') {
                throw $error;
            }
        }

        return $correlationId;
    }

    /**
     * Liberacao IMEDIATA do pedido que acabou de ser pago, chamada depois que
     * a resposta 200 ja foi enviada a Woovi. Sem isto o cliente esperava o
     * cron (a HostGator compartilhada so permite 15 em 15 minutos) para ver a
     * licenca — foi o que aconteceu em 19/08/2026. A fila continua sendo a
     * rede de seguranca: qualquer falha aqui e reprocessada pelo worker.
     */
    public function settleWebhookNow(string $correlationId): void
    {
        if (!preg_match('/^[a-f0-9-]{36}$/i', $correlationId)) {
            return;
        }

        $this->reconcileOrder($correlationId);

        // O evento correspondente ja pode ser marcado como processado: o
        // reconcileOrder acima consultou a Woovi e aplicou o estado canonico.
        $this->database->pdo()->prepare(
            "UPDATE webhook_events SET status = 'processed',
                 processed_at = CURRENT_TIMESTAMP(6),
                 processing_started_at = NULL, last_error = NULL
             WHERE provider = 'woovi' AND resource_id = :resource_id
               AND status = 'received'"
        )->execute(['resource_id' => $correlationId]);
    }

    /** @param array<string, mixed> $body */
    private function isWebhookRegistrationProbe(array $body): bool
    {
        $fields = array_keys($body);
        if (count($fields) < 2 || count($fields) > 3
            || array_diff($fields, ['data_criacao', 'evento', 'event']) !== []) {
            return false;
        }
        $createdAt = $body['data_criacao'] ?? null;
        if (!is_string($createdAt) || strlen($createdAt) < 10
            || strlen($createdAt) > 64 || strtotime($createdAt) === false) {
            return false;
        }
        if (array_key_exists('evento', $body)
            && (!is_string($body['evento'])
                || strtolower(trim($body['evento'])) !== 'teste_webhook')) {
            return false;
        }
        return true;
    }

    public function processPendingWebhooks(int $limit = 25): int
    {
        $limit = max(1, min(100, $limit));
        $ids = $this->database->pdo()->query(
            "SELECT id FROM webhook_events
             WHERE provider = 'woovi' AND attempts < 10
               AND (status = 'received'
                    OR (status = 'failed' AND
                        (next_attempt_at IS NULL OR
                         next_attempt_at <= CURRENT_TIMESTAMP(6)))
                    OR (status = 'processing' AND processing_started_at <
                        DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 5 MINUTE)))
             ORDER BY received_at ASC LIMIT {$limit}"
        )->fetchAll(PDO::FETCH_COLUMN);
        $processed = 0;
        foreach ($ids as $id) {
            $event = null;
            try {
                $event = $this->claimWebhook((string) $id);
                if ($event === null) {
                    continue;
                }
                $this->reconcileOrder((string) $event['resource_id']);
                $this->database->pdo()->prepare(
                    "UPDATE webhook_events SET status = 'processed',
                         processed_at = CURRENT_TIMESTAMP(6),
                         processing_started_at = NULL, last_error = NULL
                     WHERE id = :id"
                )->execute(['id' => $id]);
                ++$processed;
            } catch (\Throwable $error) {
                $attempts = isset($event) && is_array($event)
                    ? (int) $event['attempts'] : 1;
                $delay = min(21600, 60 * (2 ** min(8, $attempts)));
                $this->database->pdo()->prepare(
                    "UPDATE webhook_events SET status = 'failed',
                         processing_started_at = NULL,
                         next_attempt_at = :next_attempt_at,
                         last_error = :last_error WHERE id = :id"
                )->execute([
                    'next_attempt_at' => gmdate('Y-m-d H:i:s', time() + $delay),
                    'last_error' => mb_substr($error->getMessage(), 0, 500),
                    'id' => $id,
                ]);
                error_log('ChurchDAW Woovi webhook: ' . $error->getMessage());
            }
        }
        return $processed;
    }

    /** @return array{checked:int,reconciled:int,failed:int} */
    public function reconcilePending(int $limit = 100): array
    {
        $limit = max(1, min(500, $limit));
        $ids = $this->database->pdo()->query(
            "SELECT id FROM orders WHERE provider = 'woovi'
               AND status IN ('creating', 'pending', 'provider_ambiguous')
             ORDER BY COALESCE(provider_reconciled_at, '1970-01-01') ASC
             LIMIT {$limit}"
        )->fetchAll(PDO::FETCH_COLUMN);
        $stats = ['checked' => 0, 'reconciled' => 0, 'failed' => 0];
        foreach ($ids as $id) {
            ++$stats['checked'];
            try {
                if ($this->reconcileOrder((string) $id)) {
                    ++$stats['reconciled'];
                }
            } catch (\Throwable $error) {
                ++$stats['failed'];
                error_log('ChurchDAW Woovi reconcile: ' . $error->getMessage());
            }
        }
        return $stats;
    }

    public function reconcileOrder(string $orderId): bool
    {
        if (!preg_match('/^[a-f0-9-]{36}$/i', $orderId)) {
            throw new \InvalidArgumentException('Pedido Woovi invalido.');
        }
        $select = $this->database->pdo()->prepare(
            "SELECT * FROM orders WHERE id = :id AND provider = 'woovi' LIMIT 1"
        );
        $select->execute(['id' => $orderId]);
        $order = $select->fetch();
        if (!is_array($order)) {
            throw new \RuntimeException('Pedido Woovi inexistente.');
        }
        $response = $this->woovi->charge($orderId);
        $charge = $this->normaliseCharge($response);
        $this->assertChargeMatches(
            $charge,
            $orderId,
            (int) $order['expected_amount_cents'],
            true
        );
        $status = strtoupper((string) ($charge['status'] ?? ''));
        if ($status === 'COMPLETED') {
            $this->settleCompleted($order, $charge, $response);
            return true;
        }
        if ($status === 'ACTIVE') {
            $brCode = (string) (
                $charge['brCode'] ?? $response['brCode'] ?? ''
            );
            $checkoutUrl = (string) ($charge['paymentLinkUrl'] ?? '');
            $imageUrl = (string) ($charge['qrCodeImage'] ?? '');
            if (strlen($brCode) < 40
                || !$this->safeWooviUrl($checkoutUrl)
                || ($imageUrl !== '' && !$this->safeWooviUrl($imageUrl))) {
                throw new \RuntimeException('Cobranca Woovi ativa incompleta.');
            }
            $providerId = (string) (
                $charge['globalID'] ?? $charge['identifier'] ?? $orderId
            );
            if (mb_strlen($providerId) > 128) {
                throw new \RuntimeException('Identificador Woovi invalido.');
            }
            $expiresAt = self::mysqlDate((string) ($charge['expiresDate'] ?? ''))
                ?? gmdate(
                    'Y-m-d H:i:s',
                    time() + $this->config->int(
                        'woovi.charge_expiration_minutes'
                    ) * 60
                );
            $this->database->pdo()->prepare(
                "UPDATE orders SET status = 'pending',
                     provider_preference_id = :provider_id,
                     provider_status = 'ACTIVE', checkout_url = :checkout_url,
                     checkout_code = :checkout_code,
                     checkout_image_url = :checkout_image_url,
                     checkout_expires_at = :checkout_expires_at,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6),
                     provider_reconcile_failures = 0,
                     provider_reconcile_error = NULL
                 WHERE id = :id AND provider = 'woovi'
                   AND status IN ('creating', 'pending',
                                  'provider_ambiguous', 'provider_error')"
            )->execute([
                'provider_id' => $providerId,
                'checkout_url' => $checkoutUrl,
                'checkout_code' => $brCode,
                'checkout_image_url' => $imageUrl,
                'checkout_expires_at' => $expiresAt,
                'id' => $orderId,
            ]);
            return true;
        }
        if ($status === 'EXPIRED') {
            $this->database->pdo()->prepare(
                "UPDATE orders SET status = 'expired',
                     reserves_checkout_slot = 0,
                     provider_status = 'EXPIRED',
                     provider_reconciled_at = CURRENT_TIMESTAMP(6)
                 WHERE id = :id AND provider = 'woovi'
                   AND status <> 'paid'"
            )->execute(['id' => $orderId]);
            return true;
        }
        $this->database->pdo()->prepare(
            "UPDATE orders SET provider_status = :provider_status,
                 provider_reconciled_at = CURRENT_TIMESTAMP(6),
                 provider_reconcile_error = NULL
             WHERE id = :id AND provider = 'woovi'"
        )->execute(['provider_status' => $status, 'id' => $orderId]);
        return false;
    }

    /** @param array<string,mixed> $plan @return array<string,mixed> */
    private function prepareOrder(
        string $email,
        string $displayName,
        string $password,
        string $idempotencyKey,
        string $checkoutToken,
        array $plan,
        Request $request
    ): array {
        return $this->database->transaction(function (PDO $pdo) use (
            $email,
            $displayName,
            $password,
            $idempotencyKey,
            $checkoutToken,
            $plan,
            $request
        ): array {
            $findUser = $pdo->prepare(
                'SELECT * FROM users WHERE email = :email LIMIT 1 FOR UPDATE'
            );
            $findUser->execute(['email' => $email]);
            $user = $findUser->fetch();
            if (is_array($user)) {
                if ($user['status'] !== 'active'
                    || !password_verify($password, (string) $user['password_hash'])) {
                    throw new ApiException(
                        401,
                        'INVALID_CREDENTIALS',
                        'E-mail ou senha invalidos.'
                    );
                }
                if ($user['email_verified_at'] === null) {
                    $pdo->prepare(
                        'UPDATE users
                         SET email_verified_at = CURRENT_TIMESTAMP(6)
                         WHERE id = :id AND email_verified_at IS NULL'
                    )->execute(['id' => $user['id']]);
                    $user['email_verified_at'] = gmdate('Y-m-d H:i:s.u');
                }
            } else {
                $user = [
                    'id' => Crypto::uuid(),
                    'email' => $email,
                    'display_name' => $displayName,
                    'email_verified_at' => gmdate('Y-m-d H:i:s.u'),
                ];
                $pdo->prepare(
                    "INSERT INTO users
                        (id, email, display_name, password_hash, status,
                         email_verified_at)
                     VALUES
                        (:id, :email, :display_name, :password_hash, 'active',
                         CURRENT_TIMESTAMP(6))"
                )->execute([
                    'id' => $user['id'],
                    'email' => $email,
                    'display_name' => $displayName,
                    'password_hash' => PasswordPolicy::hash($password),
                ]);
            }

            $licensed = $pdo->prepare(
                "SELECT id FROM licenses WHERE user_id = :user_id
                   AND product_code = :product_code AND status = 'active'
                 LIMIT 1 FOR UPDATE"
            );
            $licensed->execute([
                'user_id' => $user['id'],
                'product_code' => $this->config->string('product.code'),
            ]);
            if (is_string($licensed->fetchColumn())) {
                throw new ApiException(
                    409,
                    'ALREADY_LICENSED',
                    'Esta conta ja possui uma licenca ativa.'
                );
            }

            $findOrder = $pdo->prepare(
                "SELECT * FROM orders WHERE user_id = :user_id
                   AND product_code = :product_code
                   AND (idempotency_key = :idempotency_key OR open_slot = 1)
                 ORDER BY (idempotency_key = :idempotency_match) DESC,
                          created_at DESC LIMIT 1 FOR UPDATE"
            );
            $findOrder->execute([
                'user_id' => $user['id'],
                'product_code' => $this->config->string('product.code'),
                'idempotency_key' => $idempotencyKey,
                'idempotency_match' => $idempotencyKey,
            ]);
            $order = $findOrder->fetch();
            $verificationToken = '';
            if (is_array($order)) {
                if ((string) $order['provider'] !== 'woovi'
                    || (string) ($order['plan_code'] ?? '') !== $plan['code']
                    || !in_array(
                        (string) $order['status'],
                        [
                            'creating',
                            'pending',
                            'provider_ambiguous',
                            'provider_error',
                        ],
                        true
                    )) {
                    throw new ApiException(
                        409,
                        'OPEN_ORDER_EXISTS',
                        'Existe uma compra anterior. Aguarde ou atualize a pagina.'
                    );
                }
                $pdo->prepare(
                    'UPDATE orders SET checkout_access_token_hash = :token_hash
                     WHERE id = :id'
                )->execute([
                    'token_hash' => Crypto::tokenHash($checkoutToken),
                    'id' => $order['id'],
                ]);
                return [
                    'order_id' => (string) $order['id'],
                    'user_id' => (string) $user['id'],
                    'plan_code' => (string) $order['plan_code'],
                    'checkout_url' => (string) ($order['checkout_url'] ?? ''),
                    'checkout_code' => (string) ($order['checkout_code'] ?? ''),
                    'checkout_image_url' =>
                        (string) ($order['checkout_image_url'] ?? ''),
                    'checkout_expires_at' =>
                        (string) ($order['checkout_expires_at'] ?? ''),
                    'status' => (string) $order['status'],
                    'existing' => (string) $order['status'] === 'pending'
                        && (string) ($order['checkout_code'] ?? '') !== '',
                    'resume_existing' => in_array(
                        (string) $order['status'],
                        ['creating', 'pending', 'provider_ambiguous'],
                        true
                    ),
                    'retry_create' =>
                        (string) $order['status'] === 'provider_error',
                    'verification_token' => '',
                ];
            }

            if (($user['email_verified_at'] ?? null) === null) {
                $verificationToken = $this->createVerificationToken(
                    $pdo,
                    (string) $user['id']
                );
            }
            $orderId = Crypto::uuid();
            $pdo->prepare(
                "INSERT INTO orders
                    (id, user_id, product_code, plan_code, activation_policy,
                     provider, idempotency_key, expected_amount_cents,
                     currency, status, reserves_checkout_slot,
                     checkout_access_token_hash, terms_version,
                     privacy_version, accepted_at, acceptance_ip_hash)
                 VALUES
                    (:id, :user_id, :product_code, :plan_code,
                     :activation_policy, 'woovi', :idempotency_key,
                     :amount, :currency, 'creating', 1, :token_hash,
                     :terms_version, :privacy_version,
                     CURRENT_TIMESTAMP(6), :ip_hash)"
            )->execute([
                'id' => $orderId,
                'user_id' => $user['id'],
                'product_code' => $this->config->string('product.code'),
                'plan_code' => $plan['code'],
                'activation_policy' => $plan['activation_policy'],
                'idempotency_key' => $idempotencyKey,
                'amount' => $plan['price_cents'],
                'currency' => $this->config->string('product.currency'),
                'token_hash' => Crypto::tokenHash($checkoutToken),
                'terms_version' => $this->config->string('legal.terms_version'),
                'privacy_version' =>
                    $this->config->string('legal.privacy_version'),
                'ip_hash' => $this->crypto->hashIp($request->ip),
            ]);
            AuditLog::recordWithPdo(
                $pdo,
                'checkout.woovi_created',
                $request->requestId,
                (string) $user['id'],
                null,
                $this->crypto->hashIp($request->ip),
                ['order_id' => $orderId, 'plan_code' => $plan['code']]
            );
            return [
                'order_id' => $orderId,
                'user_id' => (string) $user['id'],
                'plan_code' => $plan['code'],
                'status' => 'creating',
                'existing' => false,
                'resume_existing' => false,
                'retry_create' => false,
                'verification_token' => $verificationToken,
            ];
        });
    }

    /** @param array<string,mixed> $order @param array<string,mixed> $charge @param array<string,mixed> $response */
    private function settleCompleted(
        array $order,
        array $charge,
        array $response
    ): void {
        $transactionId = (string) ($charge['transactionID'] ?? '');
        if (!preg_match('/^[A-Za-z0-9._:-]{8,128}$/', $transactionId)) {
            throw new \RuntimeException('Transacao Woovi invalida.');
        }
        $paidAt = self::mysqlDate((string) ($charge['paidAt'] ?? ''))
            ?? gmdate('Y-m-d H:i:s');
        $fingerprint = hash(
            'sha256',
            json_encode($response, JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR)
        );
        $this->database->transaction(function (PDO $pdo) use (
            $order,
            $transactionId,
            $paidAt,
            $fingerprint
        ): void {
            $lock = $pdo->prepare(
                "SELECT * FROM orders WHERE id = :id AND provider = 'woovi'
                 LIMIT 1 FOR UPDATE"
            );
            $lock->execute(['id' => $order['id']]);
            $current = $lock->fetch();
            if (!is_array($current)) {
                throw new \RuntimeException('Pedido Woovi desapareceu.');
            }
            if ((string) $current['status'] === 'paid') {
                return;
            }
            $existingPayment = $pdo->prepare(
                "SELECT order_id FROM payments WHERE provider = 'woovi'
                   AND provider_payment_id = :payment_id LIMIT 1 FOR UPDATE"
            );
            $existingPayment->execute(['payment_id' => $transactionId]);
            $paymentOrder = $existingPayment->fetchColumn();
            if (is_string($paymentOrder)
                && !hash_equals((string) $current['id'], $paymentOrder)) {
                throw new \RuntimeException('Transacao Woovi ja conciliada.');
            }
            if (!is_string($paymentOrder)) {
                $pdo->prepare(
                    "INSERT INTO payments
                        (id, order_id, provider, provider_payment_id,
                         amount_cents, currency, status, raw_fingerprint,
                         approved_at)
                     VALUES
                        (:id, :order_id, 'woovi', :payment_id, :amount,
                         :currency, 'approved', :fingerprint, :approved_at)"
                )->execute([
                    'id' => Crypto::uuid(),
                    'order_id' => $current['id'],
                    'payment_id' => $transactionId,
                    'amount' => $current['expected_amount_cents'],
                    'currency' => $current['currency'],
                    'fingerprint' => $fingerprint,
                    'approved_at' => $paidAt,
                ]);
            }
            $license = $pdo->prepare(
                'SELECT id FROM licenses WHERE order_id = :order_id LIMIT 1'
            );
            $license->execute(['order_id' => $current['id']]);
            $licenseId = $license->fetchColumn();
            if (!is_string($licenseId)) {
                $active = $pdo->prepare(
                    "SELECT id FROM licenses WHERE user_id = :user_id
                       AND product_code = :product_code AND status = 'active'
                     LIMIT 1 FOR UPDATE"
                );
                $active->execute([
                    'user_id' => $current['user_id'],
                    'product_code' => $current['product_code'],
                ]);
                if (is_string($active->fetchColumn())) {
                    $pdo->prepare(
                        "UPDATE orders SET status = 'paid_review',
                             reserves_checkout_slot = 1,
                             provider_status = 'COMPLETED',
                             provider_payment_id = :payment_id,
                             provider_reconciled_at = CURRENT_TIMESTAMP(6),
                             paid_at = :paid_at WHERE id = :id"
                    )->execute([
                        'payment_id' => $transactionId,
                        'paid_at' => $paidAt,
                        'id' => $current['id'],
                    ]);
                    return;
                }
                $licenseId = Crypto::uuid();
                $policy = (string) ($current['activation_policy'] ?? 'single_device');
                $pdo->prepare(
                    "INSERT INTO licenses
                        (id, user_id, product_code, order_id, status,
                         max_active_devices, activation_policy)
                     VALUES
                        (:id, :user_id, :product_code, :order_id, 'active',
                         :max_devices, :activation_policy)"
                )->execute([
                    'id' => $licenseId,
                    'user_id' => $current['user_id'],
                    'product_code' => $current['product_code'],
                    'order_id' => $current['id'],
                    'max_devices' => $policy === 'platform_bundle' ? 3 : 1,
                    'activation_policy' => $policy,
                ]);
            }
            $pdo->prepare(
                "UPDATE orders SET status = 'paid', reserves_checkout_slot = 0,
                     provider_status = 'COMPLETED',
                     provider_payment_id = :payment_id,
                     provider_reconciled_at = CURRENT_TIMESTAMP(6),
                     provider_reconcile_failures = 0,
                     provider_reconcile_error = NULL, paid_at = :paid_at
                 WHERE id = :id"
            )->execute([
                'payment_id' => $transactionId,
                'paid_at' => $paidAt,
                'id' => $current['id'],
            ]);
            AuditLog::recordWithPdo(
                $pdo,
                'payment.woovi_approved',
                null,
                (string) $current['user_id'],
                $licenseId,
                null,
                [
                    'order_id' => $current['id'],
                    'plan_code' => $current['plan_code'],
                    'amount_cents' => (int) $current['expected_amount_cents'],
                ]
            );
        });
    }

    /** @return array<string,mixed>|null */
    private function claimWebhook(string $id): ?array
    {
        return $this->database->transaction(function (PDO $pdo) use ($id): ?array {
            $select = $pdo->prepare(
                "SELECT * FROM webhook_events WHERE id = :id
                   AND provider = 'woovi' LIMIT 1 FOR UPDATE"
            );
            $select->execute(['id' => $id]);
            $event = $select->fetch();
            if (!is_array($event) || $event['status'] === 'processed') {
                return null;
            }
            $pdo->prepare(
                "UPDATE webhook_events SET status = 'processing',
                     attempts = attempts + 1,
                     processing_started_at = CURRENT_TIMESTAMP(6),
                     last_error = NULL WHERE id = :id"
            )->execute(['id' => $id]);
            $event['attempts'] = (int) $event['attempts'] + 1;
            return $event;
        });
    }

    /** @return array<string,mixed> */
    private function authorisedOrder(string $orderId, string $token): array
    {
        $select = $this->database->pdo()->prepare(
            "SELECT o.*, l.id AS license_id, u.email_verified_at
             FROM orders o
             JOIN users u ON u.id = o.user_id
             LEFT JOIN licenses l ON l.order_id = o.id
             WHERE o.id = :id AND o.provider = 'woovi' LIMIT 1"
        );
        $select->execute(['id' => $orderId]);
        $order = $select->fetch();
        if (!is_array($order)
            || !hash_equals(
                (string) ($order['checkout_access_token_hash'] ?? ''),
                Crypto::tokenHash($token)
            )) {
            throw new ApiException(404, 'ORDER_NOT_FOUND', 'Compra nao encontrada.');
        }
        return $order;
    }

    /** @param array<string,mixed> $order @return array<string,mixed> */
    private function publicStatus(array $order): array
    {
        $status = (string) $order['status'];
        return [
            'order_id' => (string) $order['id'],
            'status' => $status,
            'paid' => $status === 'paid',
            'license_issued' => (string) ($order['license_id'] ?? '') !== '',
            'email_verified' => $order['email_verified_at'] !== null,
            'message' => match ($status) {
                'paid' => 'Pagamento confirmado e licenca liberada.',
                'expired' => 'Este Pix expirou. Inicie uma nova compra.',
                'paid_review' => 'Pagamento recebido e em conferencia.',
                default => 'Aguardando o pagamento Pix.',
            },
        ];
    }

    /** @param array<string,mixed> $prepared @return array<string,mixed> */
    private function checkoutPayload(array $prepared, string $token): array
    {
        return [
            'order_id' => (string) $prepared['order_id'],
            'checkout_token' => $token,
            'plan_code' => (string) $prepared['plan_code'],
            'checkout_url' => (string) ($prepared['checkout_url'] ?? ''),
            'pix_copy_paste' => (string) ($prepared['checkout_code'] ?? ''),
            'qr_code_image' => (string) ($prepared['checkout_image_url'] ?? ''),
            'expires_at' => (string) ($prepared['checkout_expires_at'] ?? ''),
            'status' => (string) ($prepared['status'] ?? 'pending'),
            'email_verification_sent' =>
                (string) ($prepared['verification_token'] ?? '') !== '',
        ];
    }

    /** @return array{code:string,name:string,price_cents:int,activation_policy:string} */
    private function plan(string $code): array
    {
        return match ($code) {
            'single' => [
                'code' => 'single',
                'name' => 'Licenca Individual',
                'price_cents' => $this->config->int('plan.single.price_cents'),
                'activation_policy' => 'single_device',
            ],
            'complete' => [
                'code' => 'complete',
                'name' => 'Pacote Completo',
                'price_cents' => $this->config->int('plan.complete.price_cents'),
                'activation_policy' => 'platform_bundle',
            ],
            default => throw new ApiException(
                422,
                'INVALID_PLAN',
                'Escolha um plano valido.'
            ),
        };
    }

    /** @param array<string,mixed> $response @return array<string,mixed> */
    private function normaliseCharge(array $response): array
    {
        $charge = $response['charge'] ?? $response;
        if (!is_array($charge)) {
            throw new \RuntimeException('Resposta Woovi sem cobranca.');
        }
        return $charge;
    }

    /** @param array<string,mixed> $charge */
    private function assertChargeMatches(
        array $charge,
        string $orderId,
        int $amount,
        bool $allowCompleted
    ): void {
        $status = strtoupper((string) ($charge['status'] ?? ''));
        $allowed = $allowCompleted
            ? ['ACTIVE', 'COMPLETED', 'EXPIRED'] : ['ACTIVE', 'COMPLETED'];
        if (!hash_equals($orderId, (string) ($charge['correlationID'] ?? ''))
            || (int) ($charge['value'] ?? -1) !== $amount
            || !in_array($status, $allowed, true)) {
            throw new \RuntimeException(
                'Cobranca Woovi nao corresponde ao pedido local.'
            );
        }
    }

    private function safeWooviUrl(string $url): bool
    {
        $parts = parse_url($url);
        if (!is_array($parts)
            || strtolower((string) ($parts['scheme'] ?? '')) !== 'https') {
            return false;
        }
        return in_array(strtolower((string) ($parts['host'] ?? '')), [
            'woovi.com',
            'www.woovi.com',
            'app.woovi.com',
            'api.woovi.com',
            'woovi-sandbox.com',
            'www.woovi-sandbox.com',
            'app.woovi-sandbox.com',
            'api.woovi-sandbox.com',
        ], true);
    }

    private function createVerificationToken(PDO $pdo, string $userId): string
    {
        $token = Crypto::randomToken(32);
        $pdo->prepare(
            "UPDATE user_tokens SET used_at = CURRENT_TIMESTAMP(6)
             WHERE user_id = :user_id AND type = 'email_verification'
               AND used_at IS NULL"
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
            'token_hash' => Crypto::tokenHash($token),
        ]);
        return $token;
    }

    private static function mysqlDate(string $value): ?string
    {
        $timestamp = $value === '' ? false : strtotime($value);
        return $timestamp === false ? null : gmdate('Y-m-d H:i:s', $timestamp);
    }
}
