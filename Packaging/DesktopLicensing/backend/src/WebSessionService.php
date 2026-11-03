<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;

final class WebSessionService
{
    private const COOKIE_NAME = 'churchdaw_session';

    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto,
        private readonly RateLimiter $rateLimiter
    ) {
    }

    /** @return array{csrf_token: string, user: array<string, mixed>} */
    public function login(Request $request): array
    {
        $email = AuthService::normaliseEmail($request->string('email', 254));
        $password = $request->string('password', 128);
        $this->rateLimiter->assertAllowed(
            'web-login-ip',
            $this->rateLimiter->ipDimension($request->ip),
            10,
            900
        );
        $this->rateLimiter->assertAllowed(
            'web-login-email',
            hash('sha256', $email),
            10,
            900
        );
        $statement = $this->database->pdo()->prepare(
            "SELECT id, email, display_name, password_hash, status, email_verified_at
             FROM users WHERE email = :email LIMIT 1"
        );
        $statement->execute(['email' => $email]);
        $user = $statement->fetch();
        $passwordHash = is_array($user)
            ? (string) $user['password_hash']
            : PasswordPolicy::DUMMY_HASH;
        $passwordValid = password_verify($password, $passwordHash);
        sodium_memzero($password);
        if (!is_array($user)
            || $user['status'] !== 'active'
            || !$passwordValid) {
            throw new ApiException(401, 'INVALID_CREDENTIALS', 'E-mail ou senha invalidos.');
        }
        // O cliente pode entrar assim que a senha estiver correta. A antiga
        // confirmacao de e-mail nao e mais uma etapa obrigatoria da compra.
        if ($user['email_verified_at'] === null) {
            $this->database->pdo()->prepare(
                'UPDATE users
                 SET email_verified_at = CURRENT_TIMESTAMP(6)
                 WHERE id = :id AND email_verified_at IS NULL'
            )->execute(['id' => $user['id']]);
            $user['email_verified_at'] = gmdate('Y-m-d H:i:s.u');
        }

        $token = Crypto::randomToken(32);
        $csrf = Crypto::randomToken(24);
        $sessionId = Crypto::uuid();
        $userAgent = mb_substr($request->header('user-agent'), 0, 512);
        $this->database->pdo()->prepare(
            "INSERT INTO web_sessions
                (id, user_id, token_hash, csrf_hash, csrf_ciphertext,
                 ip_hash, user_agent_hash, expires_at)
             VALUES
                (:id, :user_id, :token_hash, :csrf_hash, :csrf_ciphertext, :ip_hash,
                 :user_agent_hash, DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 12 HOUR))"
        )->execute([
            'id' => $sessionId,
            'user_id' => $user['id'],
            'token_hash' => Crypto::tokenHash($token),
            'csrf_hash' => Crypto::tokenHash($csrf),
            'csrf_ciphertext' => $this->crypto->encrypt($csrf),
            'ip_hash' => $this->crypto->hashIp($request->ip),
            'user_agent_hash' => hash('sha256', $userAgent),
        ]);
        $this->setCookie($token, 43200);

        unset($user['password_hash']);
        return ['csrf_token' => $csrf, 'user' => $user];
    }

    /** @return array<string, mixed> */
    public function requireUser(Request $request, bool $requireCsrf = false): array
    {
        $token = (string) ($_COOKIE[self::COOKIE_NAME] ?? '');
        if ($token === '') {
            throw new ApiException(401, 'AUTH_REQUIRED', 'Entre na sua conta.');
        }
        $statement = $this->database->pdo()->prepare(
            "SELECT ws.id AS session_id, ws.csrf_hash, ws.csrf_ciphertext,
                    ws.user_agent_hash,
                    u.id, u.email, u.display_name, u.status, u.email_verified_at
             FROM web_sessions ws
             JOIN users u ON u.id = ws.user_id
             WHERE ws.token_hash = :token_hash
               AND ws.revoked_at IS NULL
               AND ws.expires_at > CURRENT_TIMESTAMP(6)
               AND u.status = 'active'
             LIMIT 1"
        );
        $statement->execute(['token_hash' => Crypto::tokenHash($token)]);
        $user = $statement->fetch();
        if (!is_array($user)
            || !hash_equals(
                (string) $user['user_agent_hash'],
                hash('sha256', mb_substr($request->header('user-agent'), 0, 512))
            )) {
            $this->clearCookie();
            throw new ApiException(401, 'AUTH_REQUIRED', 'Entre novamente.');
        }
        if ($requireCsrf) {
            $csrf = $request->header('x-csrf-token');
            if ($csrf === '' || !hash_equals(
                (string) $user['csrf_hash'],
                Crypto::tokenHash($csrf)
            )) {
                throw new ApiException(403, 'INVALID_CSRF', 'Sessao de pagina invalida.');
            }
        }
        $this->database->pdo()->prepare(
            'UPDATE web_sessions SET last_seen_at = CURRENT_TIMESTAMP(6) WHERE id = :id'
        )->execute(['id' => $user['session_id']]);
        return $user;
    }

    /** @return array<string, mixed> */
    public function account(Request $request): array
    {
        $user = $this->requireUser($request);
        $statement = $this->database->pdo()->prepare(
            "SELECT l.id, l.product_code, l.status, l.issued_at,
                    l.activation_policy, l.max_active_devices
             FROM licenses l
             WHERE l.user_id = :user_id
             ORDER BY l.issued_at DESC LIMIT 1"
        );
        $statement->execute(['user_id' => $user['id']]);
        $license = $statement->fetch();

        $transfersUsed = 0;
        if (is_array($license)) {
            $devices = $this->database->pdo()->prepare(
                "SELECT d.id AS device_id, d.label AS device_label,
                        d.platform, d.last_seen_at, a.activated_at
                 FROM activations a
                 JOIN devices d ON d.id = a.device_id
                 WHERE a.license_id = :license_id AND a.status = 'active'
                 ORDER BY FIELD(d.platform, 'windows', 'macos', 'android')"
            );
            $devices->execute(['license_id' => $license['id']]);
            $activeDevices = $devices->fetchAll();
            $license['devices'] = is_array($activeDevices)
                ? $activeDevices : [];
            $count = $this->database->pdo()->prepare(
                'SELECT COUNT(*) FROM license_transfers
                 WHERE license_id = :license_id
                   AND created_at >= DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 30 DAY)'
            );
            $count->execute(['license_id' => $license['id']]);
            $transfersUsed = (int) $count->fetchColumn();
        }

        $csrf = $this->crypto->decrypt((string) $user['csrf_ciphertext']);
        if (!hash_equals((string) $user['csrf_hash'], Crypto::tokenHash($csrf))) {
            throw new ApiException(401, 'AUTH_REQUIRED', 'Entre novamente.');
        }

        return [
            'user' => [
                'email' => $user['email'],
                'display_name' => $user['display_name'],
                'email_verified' => $user['email_verified_at'] !== null,
            ],
            'license' => is_array($license) ? $license : null,
            'transfers_remaining_30d' => max(
                0,
                $this->config->int('license.max_transfers_30d') - $transfersUsed
            ),
            'csrf_token' => $csrf,
        ];
    }

    public function logout(Request $request): void
    {
        $user = $this->requireUser($request, true);
        $this->database->pdo()->prepare(
            'UPDATE web_sessions SET revoked_at = CURRENT_TIMESTAMP(6)
             WHERE id = :id'
        )->execute(['id' => $user['session_id']]);
        $this->clearCookie();
    }

    private function setCookie(string $value, int $maxAge): void
    {
        setcookie(self::COOKIE_NAME, $value, [
            'expires' => time() + $maxAge,
            'path' => '/',
            'secure' => true,
            'httponly' => true,
            'samesite' => 'Strict',
        ]);
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
