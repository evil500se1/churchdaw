<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;

final class AdminService
{
    private const COOKIE_NAME = '__Host-churchdaw_admin';

    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto,
        private readonly RateLimiter $rateLimiter
    ) {
    }

    public function enabled(): bool
    {
        return $this->config->bool('admin.enabled');
    }

    public function login(Request $request): void
    {
        $this->assertEnabled();
        $username = strtolower($request->string('username', 64));
        $password = $request->string('password', 256);
        $this->rateLimiter->assertAllowed(
            'admin-login-ip',
            $this->rateLimiter->ipDimension($request->ip),
            5,
            900
        );
        $this->rateLimiter->assertAllowed(
            'admin-login-user',
            hash('sha256', $username),
            5,
            900
        );

        $expectedUser = strtolower($this->config->string('admin.username'));
        $userValid = hash_equals($expectedUser, $username);
        $passwordValid = password_verify(
            $password,
            $this->config->string('admin.password_hash')
        );
        sodium_memzero($password);
        if (!$userValid || !$passwordValid) {
            throw new ApiException(
                401,
                'INVALID_ADMIN_CREDENTIALS',
                'Usuario ou senha invalidos.'
            );
        }

        $token = Crypto::randomToken(32);
        $csrf = Crypto::randomToken(24);
        $userAgent = mb_substr($request->header('user-agent'), 0, 512);
        $this->database->pdo()->prepare(
            "INSERT INTO admin_sessions
                (id, token_hash, csrf_hash, csrf_ciphertext, ip_hash,
                 user_agent_hash, expires_at)
             VALUES
                (:id, :token_hash, :csrf_hash, :csrf_ciphertext, :ip_hash,
                 :user_agent_hash,
                 DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 8 HOUR))"
        )->execute([
            'id' => Crypto::uuid(),
            'token_hash' => Crypto::tokenHash($token),
            'csrf_hash' => Crypto::tokenHash($csrf),
            'csrf_ciphertext' => $this->crypto->encrypt($csrf),
            'ip_hash' => $this->crypto->hashIp($request->ip),
            'user_agent_hash' => hash('sha256', $userAgent),
        ]);
        setcookie(self::COOKIE_NAME, $token, [
            'expires' => time() + 28800,
            'path' => '/',
            'secure' => true,
            'httponly' => true,
            'samesite' => 'Strict',
        ]);
    }

    /** @return array<string, mixed> */
    public function dashboard(Request $request): array
    {
        $admin = $this->requireAdmin($request);
        $pdo = $this->database->pdo();
        $counts = $pdo->query(
            "SELECT
                (SELECT COUNT(*) FROM licenses WHERE status = 'active')
                    AS active_licenses,
                (SELECT COUNT(*) FROM licenses WHERE status = 'revoked')
                    AS revoked_licenses,
                (SELECT COUNT(*) FROM activations WHERE status = 'active')
                    AS active_devices,
                (SELECT COUNT(*) FROM android_trials WHERE status = 'active')
                    AS active_trials"
        )->fetch();
        $licenses = $pdo->query(
            "SELECT l.id AS license_id, l.status AS license_status,
                    l.issued_at, l.revoked_at, l.revoke_reason,
                    u.id AS user_id, u.email, u.display_name,
                    d.id AS device_id, d.label AS device_label,
                    d.platform, d.installation_id, d.first_seen_at,
                    d.last_seen_at, a.activated_at, a.last_validated_at
             FROM licenses l
             JOIN users u ON u.id = l.user_id
             LEFT JOIN activations a
               ON a.license_id = l.id AND a.status = 'active'
             LEFT JOIN devices d ON d.id = a.device_id
             ORDER BY l.issued_at DESC
             LIMIT 100"
        )->fetchAll();

        return [
            'csrf_token' => $admin['csrf_token'],
            'counts' => is_array($counts) ? $counts : [],
            'licenses' => is_array($licenses) ? $licenses : [],
            'flash' => $admin['flash'],
        ];
    }

    /** @return array<string, mixed> */
    public function issueLicense(Request $request): array
    {
        $admin = $this->requireAdmin($request, true);
        $email = AuthService::normaliseEmail(
            $request->string('email', 254)
        );
        $displayName = trim($request->string('display_name', 96));
        if (!filter_var($email, FILTER_VALIDATE_EMAIL)
            || mb_strlen($displayName) < 2) {
            throw new ApiException(
                422,
                'INVALID_CUSTOMER',
                'Informe nome e e-mail validos.'
            );
        }

        $temporaryCode = Crypto::randomToken(18);
        $result = $this->database->transaction(function (PDO $pdo) use (
            $email,
            $displayName,
            $temporaryCode,
            $request
        ): array {
            $findUser = $pdo->prepare(
                'SELECT id, status FROM users WHERE email = :email
                 LIMIT 1 FOR UPDATE'
            );
            $findUser->execute(['email' => $email]);
            $user = $findUser->fetch();
            $newUser = !is_array($user);
            if ($newUser) {
                $userId = Crypto::uuid();
                $passwordHash = password_hash(
                    $temporaryCode,
                    PASSWORD_ARGON2ID
                );
                if (!is_string($passwordHash)) {
                    throw new \RuntimeException(
                        'Nao foi possivel proteger o codigo de acesso.'
                    );
                }
                $pdo->prepare(
                    "INSERT INTO users
                        (id, email, display_name, password_hash, status,
                         email_verified_at)
                     VALUES
                        (:id, :email, :display_name, :password_hash,
                         'active', CURRENT_TIMESTAMP(6))"
                )->execute([
                    'id' => $userId,
                    'email' => $email,
                    'display_name' => $displayName,
                    'password_hash' => $passwordHash,
                ]);
            } else {
                $userId = (string) $user['id'];
                if ((string) $user['status'] !== 'active') {
                    throw new ApiException(
                        409,
                        'CUSTOMER_BLOCKED',
                        'A conta do cliente nao esta ativa.'
                    );
                }
                $pdo->prepare(
                    'UPDATE users SET display_name = :display_name
                     WHERE id = :id'
                )->execute([
                    'display_name' => $displayName,
                    'id' => $userId,
                ]);
            }

            $existing = $pdo->prepare(
                "SELECT id FROM licenses
                 WHERE user_id = :user_id
                   AND product_code = :product_code
                   AND status = 'active'
                 LIMIT 1 FOR UPDATE"
            );
            $existing->execute([
                'user_id' => $userId,
                'product_code' => $this->config->string('product.code'),
            ]);
            $existingId = $existing->fetchColumn();
            if (is_string($existingId) && $existingId !== '') {
                return [
                    'license_id' => $existingId,
                    'email' => $email,
                    'temporary_code' => $newUser ? $temporaryCode : null,
                    'already_active' => true,
                ];
            }

            $orderId = Crypto::uuid();
            $licenseId = Crypto::uuid();
            $pdo->prepare(
                "INSERT INTO orders
                    (id, user_id, product_code, provider, idempotency_key,
                     expected_amount_cents, currency, status,
                     terms_version, privacy_version, accepted_at,
                     acceptance_ip_hash, provider_status,
                     provider_reconciled_at, paid_at)
                 VALUES
                    (:id, :user_id, :product_code, 'manual',
                     :idempotency_key, :amount, :currency,
                     'manual_granted', :terms_version, :privacy_version,
                     CURRENT_TIMESTAMP(6), :ip_hash, 'manual',
                     CURRENT_TIMESTAMP(6), CURRENT_TIMESTAMP(6))"
            )->execute([
                'id' => $orderId,
                'user_id' => $userId,
                'product_code' => $this->config->string('product.code'),
                'idempotency_key' => 'manual:' . $orderId,
                'amount' => $this->config->int('product.price_cents'),
                'currency' => $this->config->string('product.currency'),
                'terms_version' =>
                    $this->config->string('legal.terms_version'),
                'privacy_version' =>
                    $this->config->string('legal.privacy_version'),
                'ip_hash' => $this->crypto->hashIp($request->ip),
            ]);
            $pdo->prepare(
                "INSERT INTO licenses
                    (id, user_id, product_code, order_id, status,
                     max_active_devices)
                 VALUES
                    (:id, :user_id, :product_code, :order_id, 'active', 1)"
            )->execute([
                'id' => $licenseId,
                'user_id' => $userId,
                'product_code' => $this->config->string('product.code'),
                'order_id' => $orderId,
            ]);
            $this->audit(
                $pdo,
                $request,
                $licenseId,
                'admin.license_issued',
                ['email' => $email, 'new_user' => $newUser]
            );
            return [
                'license_id' => $licenseId,
                'email' => $email,
                'temporary_code' => $newUser ? $temporaryCode : null,
                'already_active' => false,
            ];
        });

        $this->setFlash((string) $admin['session_id'], [
            'type' => 'success',
            'message' => $result['already_active']
                ? 'O cliente ja possuia uma licenca ativa.'
                : 'Licenca manual liberada com sucesso.',
            'email' => $result['email'],
            'temporary_code' => $result['temporary_code'],
            'license_id' => $result['license_id'],
        ]);
        return $result;
    }

    public function revokeLicense(Request $request, string $licenseId): void
    {
        $admin = $this->requireAdmin($request, true);
        $reason = trim($request->string('reason', 160));
        if ($reason === '') {
            $reason = 'Revogada manualmente pelo administrador';
        }
        $this->database->transaction(function (PDO $pdo) use (
            $request,
            $licenseId,
            $reason
        ): void {
            $select = $pdo->prepare(
                'SELECT id, status FROM licenses WHERE id = :id
                 LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $licenseId]);
            $license = $select->fetch();
            if (!is_array($license)) {
                throw new ApiException(
                    404,
                    'LICENSE_NOT_FOUND',
                    'Licenca inexistente.'
                );
            }
            if ((string) $license['status'] !== 'revoked') {
                $pdo->prepare(
                    "UPDATE refresh_tokens
                     SET revoked_at = COALESCE(
                         revoked_at, CURRENT_TIMESTAMP(6)
                     )
                     WHERE license_id = :license_id"
                )->execute(['license_id' => $licenseId]);
                $pdo->prepare(
                    "UPDATE activations
                     SET status = 'deactivated',
                         deactivated_at = COALESCE(
                             deactivated_at, CURRENT_TIMESTAMP(6)
                         ),
                         deactivation_reason = 'admin_revoke'
                     WHERE license_id = :license_id
                       AND status = 'active'"
                )->execute(['license_id' => $licenseId]);
                $pdo->prepare(
                    "UPDATE devices SET status = 'revoked'
                     WHERE license_id = :license_id
                       AND status = 'active'"
                )->execute(['license_id' => $licenseId]);
                $pdo->prepare(
                    "UPDATE licenses
                     SET status = 'revoked',
                         revoked_at = CURRENT_TIMESTAMP(6),
                         revoke_reason = :reason
                     WHERE id = :id"
                )->execute(['reason' => $reason, 'id' => $licenseId]);
                $this->audit(
                    $pdo,
                    $request,
                    $licenseId,
                    'admin.license_revoked',
                    ['reason' => $reason]
                );
            }
        });
        $this->setFlash((string) $admin['session_id'], [
            'type' => 'success',
            'message' => 'Licenca revogada e sessoes do aparelho encerradas.',
        ]);
    }

    public function deactivateDevice(Request $request, string $deviceId): void
    {
        $admin = $this->requireAdmin($request, true);
        $this->database->transaction(function (PDO $pdo) use (
            $request,
            $deviceId
        ): void {
            $select = $pdo->prepare(
                'SELECT id, license_id FROM devices WHERE id = :id
                 LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $deviceId]);
            $device = $select->fetch();
            if (!is_array($device)) {
                throw new ApiException(
                    404,
                    'DEVICE_NOT_FOUND',
                    'Aparelho inexistente.'
                );
            }
            $pdo->prepare(
                "UPDATE activations
                 SET status = 'deactivated',
                     deactivated_at = COALESCE(
                         deactivated_at, CURRENT_TIMESTAMP(6)
                     ),
                     deactivation_reason = 'admin_device_revoke'
                 WHERE device_id = :device_id AND status = 'active'"
            )->execute(['device_id' => $deviceId]);
            $pdo->prepare(
                "UPDATE refresh_tokens
                 SET revoked_at = COALESCE(
                     revoked_at, CURRENT_TIMESTAMP(6)
                 )
                 WHERE device_id = :device_id"
            )->execute(['device_id' => $deviceId]);
            $pdo->prepare(
                "UPDATE devices SET status = 'revoked' WHERE id = :id"
            )->execute(['id' => $deviceId]);
            $this->audit(
                $pdo,
                $request,
                (string) $device['license_id'],
                'admin.device_deactivated',
                ['device_id' => $deviceId]
            );
        });
        $this->setFlash((string) $admin['session_id'], [
            'type' => 'success',
            'message' => 'Aparelho desativado. A licenca pode entrar em outro computador.',
        ]);
    }

    public function resetCustomerAccess(Request $request, string $userId): void
    {
        $admin = $this->requireAdmin($request, true);
        $temporaryCode = Crypto::randomToken(18);
        $passwordHash = password_hash($temporaryCode, PASSWORD_ARGON2ID);
        if (!is_string($passwordHash)) {
            throw new \RuntimeException('Nao foi possivel gerar novo acesso.');
        }
        $result = $this->database->transaction(function (PDO $pdo) use (
            $request,
            $userId,
            $passwordHash
        ): array {
            $select = $pdo->prepare(
                'SELECT email FROM users WHERE id = :id
                 LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => $userId]);
            $email = $select->fetchColumn();
            if (!is_string($email)) {
                throw new ApiException(
                    404,
                    'CUSTOMER_NOT_FOUND',
                    'Cliente inexistente.'
                );
            }
            $pdo->prepare(
                'UPDATE users
                 SET password_hash = :password_hash,
                     password_changed_at = CURRENT_TIMESTAMP(6)
                 WHERE id = :id'
            )->execute([
                'password_hash' => $passwordHash,
                'id' => $userId,
            ]);
            $pdo->prepare(
                "UPDATE refresh_tokens
                 SET revoked_at = COALESCE(
                     revoked_at, CURRENT_TIMESTAMP(6)
                 )
                 WHERE user_id = :user_id"
            )->execute(['user_id' => $userId]);
            $pdo->prepare(
                "UPDATE web_sessions
                 SET revoked_at = COALESCE(
                     revoked_at, CURRENT_TIMESTAMP(6)
                 )
                 WHERE user_id = :user_id"
            )->execute(['user_id' => $userId]);
            $this->audit(
                $pdo,
                $request,
                null,
                'admin.customer_access_reset',
                ['user_id' => $userId]
            );
            return ['email' => $email];
        });
        $this->setFlash((string) $admin['session_id'], [
            'type' => 'success',
            'message' => 'Novo codigo de acesso gerado. O anterior foi invalidado.',
            'email' => $result['email'],
            'temporary_code' => $temporaryCode,
        ]);
    }

    public function logout(Request $request): void
    {
        $admin = $this->requireAdmin($request, true);
        $this->database->pdo()->prepare(
            'UPDATE admin_sessions
             SET revoked_at = CURRENT_TIMESTAMP(6)
             WHERE id = :id'
        )->execute(['id' => $admin['session_id']]);
        $this->clearCookie();
    }

    /** @return array<string, mixed> */
    private function requireAdmin(
        Request $request,
        bool $requireCsrf = false
    ): array {
        $this->assertEnabled();
        $token = (string) ($_COOKIE[self::COOKIE_NAME] ?? '');
        if ($token === '') {
            throw new ApiException(
                401,
                'ADMIN_AUTH_REQUIRED',
                'Entre no painel administrativo.'
            );
        }
        $statement = $this->database->pdo()->prepare(
            "SELECT id, csrf_hash, csrf_ciphertext, flash_ciphertext,
                    ip_hash, user_agent_hash
             FROM admin_sessions
             WHERE token_hash = :token_hash
               AND revoked_at IS NULL
               AND expires_at > CURRENT_TIMESTAMP(6)
               AND last_seen_at >
                   DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 30 MINUTE)
             LIMIT 1"
        );
        $statement->execute([
            'token_hash' => Crypto::tokenHash($token),
        ]);
        $session = $statement->fetch();
        $userAgentHash = hash(
            'sha256',
            mb_substr($request->header('user-agent'), 0, 512)
        );
        if (!is_array($session)
            || !hash_equals(
                (string) $session['ip_hash'],
                $this->crypto->hashIp($request->ip)
            )
            || !hash_equals(
                (string) $session['user_agent_hash'],
                $userAgentHash
            )) {
            $this->clearCookie();
            throw new ApiException(
                401,
                'ADMIN_AUTH_REQUIRED',
                'Entre novamente no painel.'
            );
        }
        $csrf = $this->crypto->decrypt(
            (string) $session['csrf_ciphertext']
        );
        if (!hash_equals(
            (string) $session['csrf_hash'],
            Crypto::tokenHash($csrf)
        )) {
            throw new ApiException(
                401,
                'ADMIN_AUTH_REQUIRED',
                'Entre novamente no painel.'
            );
        }
        if ($requireCsrf) {
            $provided = $request->string('csrf_token', 256);
            if ($provided === ''
                || !hash_equals(
                    (string) $session['csrf_hash'],
                    Crypto::tokenHash($provided)
                )) {
                throw new ApiException(
                    403,
                    'INVALID_CSRF',
                    'Sessao administrativa invalida.'
                );
            }
        }

        $flash = null;
        if (is_string($session['flash_ciphertext'])
            && $session['flash_ciphertext'] !== '') {
            try {
                $decoded = json_decode(
                    $this->crypto->decrypt(
                        (string) $session['flash_ciphertext']
                    ),
                    true,
                    16,
                    JSON_THROW_ON_ERROR
                );
                if (is_array($decoded)) {
                    $flash = $decoded;
                }
            } catch (\Throwable) {
                $flash = null;
            }
        }
        $this->database->pdo()->prepare(
            'UPDATE admin_sessions
             SET last_seen_at = CURRENT_TIMESTAMP(6),
                 flash_ciphertext = NULL
             WHERE id = :id'
        )->execute(['id' => $session['id']]);
        return [
            'session_id' => $session['id'],
            'csrf_token' => $csrf,
            'flash' => $flash,
        ];
    }

    /** @param array<string, mixed> $flash */
    private function setFlash(string $sessionId, array $flash): void
    {
        $encoded = json_encode(
            $flash,
            JSON_UNESCAPED_SLASHES
                | JSON_UNESCAPED_UNICODE
                | JSON_THROW_ON_ERROR
        );
        $this->database->pdo()->prepare(
            'UPDATE admin_sessions SET flash_ciphertext = :flash
             WHERE id = :id'
        )->execute([
            'flash' => $this->crypto->encrypt($encoded),
            'id' => $sessionId,
        ]);
    }

    /**
     * @param array<string, mixed> $metadata
     */
    private function audit(
        PDO $pdo,
        Request $request,
        ?string $licenseId,
        string $type,
        array $metadata
    ): void {
        $statement = $pdo->prepare(
            'INSERT INTO audit_events
                (request_id, actor_user_id, license_id, event_type,
                 ip_hash, metadata_json)
             VALUES
                (:request_id, NULL, :license_id, :event_type,
                 :ip_hash, :metadata_json)'
        );
        $statement->execute([
            'request_id' => $request->requestId,
            'license_id' => $licenseId,
            'event_type' => $type,
            'ip_hash' => $this->crypto->hashIp($request->ip),
            'metadata_json' => json_encode(
                $metadata,
                JSON_UNESCAPED_SLASHES
                    | JSON_UNESCAPED_UNICODE
                    | JSON_THROW_ON_ERROR
            ),
        ]);
    }

    private function assertEnabled(): void
    {
        if (!$this->enabled()) {
            throw new ApiException(
                404,
                'NOT_FOUND',
                'Rota inexistente.'
            );
        }
    }

    private function clearCookie(): void
    {
        setcookie(self::COOKIE_NAME, '', [
            'expires' => time() - 3600,
            'path' => '/',
            'secure' => true,
            'httponly' => true,
            'samesite' => 'Strict',
        ]);
    }
}
