<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;

final class AuthService
{
    public function __construct(
        private readonly Database $database,
        private readonly RateLimiter $rateLimiter,
        private readonly LicenseService $licenses,
        private readonly Mailer $mailer,
        private readonly AuditLog $audit
    ) {
    }

    /** @return array<string, mixed> */
    public function login(Request $request): array
    {
        $email = self::normaliseEmail($request->string('email', 254));
        $password = $request->string('password', 128);
        $this->rateLimiter->assertAllowed(
            'desktop-login-ip',
            $this->rateLimiter->ipDimension($request->ip),
            12,
            900
        );
        $this->rateLimiter->assertAllowed(
            'desktop-login-email',
            hash('sha256', $email),
            10,
            900
        );

        $statement = $this->database->pdo()->prepare(
            'SELECT * FROM users WHERE email = :email LIMIT 1'
        );
        $statement->execute(['email' => $email]);
        $user = $statement->fetch();
        $hash = is_array($user)
            ? (string) $user['password_hash']
            : PasswordPolicy::DUMMY_HASH;
        $passwordValid = password_verify($password, $hash);
        $replacementHash = null;
        if ($passwordValid && is_array($user) && password_needs_rehash(
            (string) $user['password_hash'],
            PASSWORD_ARGON2ID,
            [
                'memory_cost' => 65536,
                'time_cost' => 3,
                'threads' => 1,
            ]
        )) {
            $replacementHash = PasswordPolicy::hash($password);
        }
        sodium_memzero($password);

        if (!is_array($user) || !$passwordValid || $user['status'] !== 'active') {
            $this->audit->record('auth.login.failed', $request, is_array($user) ? $user['id'] : null);
            throw new ApiException(401, 'INVALID_CREDENTIALS', 'E-mail, senha ou licenca invalidos.');
        }

        // A posse da senha definida no checkout e suficiente para entrar.
        // Mantemos o timestamp apenas como estado legado, sem bloquear o
        // cliente que acabou de pagar por falta de confirmacao de e-mail.
        if ($user['email_verified_at'] === null) {
            $this->database->pdo()->prepare(
                'UPDATE users
                 SET email_verified_at = CURRENT_TIMESTAMP(6)
                 WHERE id = :id AND email_verified_at IS NULL'
            )->execute(['id' => $user['id']]);
            $user['email_verified_at'] = gmdate('Y-m-d H:i:s.u');
        }

        if ($replacementHash !== null) {
            $this->database->pdo()->prepare(
                'UPDATE users SET password_hash = :password_hash WHERE id = :id'
            )->execute(['password_hash' => $replacementHash, 'id' => $user['id']]);
        }

        $licenseStatement = $this->database->pdo()->prepare(
            "SELECT * FROM licenses
             WHERE user_id = :user_id AND status = 'active'
             ORDER BY issued_at DESC LIMIT 1"
        );
        $licenseStatement->execute(['user_id' => $user['id']]);
        $license = $licenseStatement->fetch();
        if (!is_array($license)) {
            throw new ApiException(403, 'NO_LICENSE', 'E-mail, senha ou licenca invalidos.');
        }

        return $this->licenses->activateForLogin(
            $user,
            $license,
            $request->string('installation_id', 128),
            $request->string('device_secret', 256),
            $request->string('device_public_key', 2048),
            $request->string('device_fingerprint', 256),
            $request->string('device_label', 120),
            $request->string('platform', 24),
            $request
        );
    }

    /** @return array<string, mixed> */
    public function refresh(Request $request): array
    {
        $this->rateLimiter->assertAllowed(
            'desktop-refresh-ip',
            $this->rateLimiter->ipDimension($request->ip),
            120,
            900
        );
        return $this->licenses->refresh($request);
    }

    public function requestPasswordReset(Request $request): void
    {
        $startedAt = hrtime(true);
        $email = self::normaliseEmail($request->string('email', 254));
        $this->rateLimiter->assertAllowed(
            'forgot-password-ip',
            $this->rateLimiter->ipDimension($request->ip),
            5,
            3600
        );
        $this->rateLimiter->assertAllowed(
            'forgot-password-email',
            hash('sha256', $email),
            3,
            3600
        );

        $statement = $this->database->pdo()->prepare(
            "SELECT id, email, display_name FROM users
             WHERE email = :email AND status = 'active' LIMIT 1"
        );
        $statement->execute(['email' => $email]);
        $user = $statement->fetch();
        if (!is_array($user)) {
            self::padEnumerationSafeResponse($startedAt);
            return;
        }

        $token = Crypto::randomToken(32);
        $this->database->transaction(function (PDO $pdo) use ($user, $token): void {
            $pdo->prepare(
                "UPDATE user_tokens SET used_at = CURRENT_TIMESTAMP(6)
                 WHERE user_id = :user_id AND type = 'password_reset' AND used_at IS NULL"
            )->execute(['user_id' => $user['id']]);
            $pdo->prepare(
                "INSERT INTO user_tokens
                    (id, user_id, type, token_hash, expires_at)
                 VALUES
                    (:id, :user_id, 'password_reset', :token_hash,
                     DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 30 MINUTE))"
            )->execute([
                'id' => Crypto::uuid(),
                'user_id' => $user['id'],
                'token_hash' => Crypto::tokenHash($token),
            ]);
        });

        try {
            $this->mailer->sendPasswordReset(
                (string) $user['email'],
                (string) $user['display_name'],
                $token
            );
        } catch (\Throwable) {
            // Resposta permanece identica para evitar enumeracao. O erro fica no log do PHP.
            error_log('ChurchDAW: falha ao enviar recuperacao para user ' . $user['id']);
        }
        self::padEnumerationSafeResponse($startedAt);
    }

    public function requestEmailVerification(Request $request): void
    {
        $startedAt = hrtime(true);
        $email = self::normaliseEmail($request->string('email', 254));
        $this->rateLimiter->assertAllowed(
            'verify-email-ip',
            $this->rateLimiter->ipDimension($request->ip),
            5,
            3600
        );
        $this->rateLimiter->assertAllowed(
            'verify-email-address',
            hash('sha256', $email),
            3,
            3600
        );

        $statement = $this->database->pdo()->prepare(
            "SELECT id, email, display_name
             FROM users
             WHERE email = :email
               AND status = 'active'
               AND email_verified_at IS NULL
             LIMIT 1"
        );
        $statement->execute(['email' => $email]);
        $user = $statement->fetch();
        if (!is_array($user)) {
            self::padEnumerationSafeResponse($startedAt);
            return;
        }

        $token = Crypto::randomToken(32);
        $this->database->transaction(function (PDO $pdo) use ($user, $token): void {
            $pdo->prepare(
                "UPDATE user_tokens SET used_at = CURRENT_TIMESTAMP(6)
                 WHERE user_id = :user_id
                   AND type = 'email_verification' AND used_at IS NULL"
            )->execute(['user_id' => $user['id']]);
            $pdo->prepare(
                "INSERT INTO user_tokens
                    (id, user_id, type, token_hash, expires_at)
                 VALUES
                    (:id, :user_id, 'email_verification', :token_hash,
                     DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 24 HOUR))"
            )->execute([
                'id' => Crypto::uuid(),
                'user_id' => $user['id'],
                'token_hash' => Crypto::tokenHash($token),
            ]);
        });

        try {
            $this->mailer->sendVerification(
                (string) $user['email'],
                (string) $user['display_name'],
                $token
            );
        } catch (\Throwable) {
            error_log('ChurchDAW: falha ao reenviar verificacao para user ' . $user['id']);
        }
        self::padEnumerationSafeResponse($startedAt);
    }

    public function resetPassword(Request $request): void
    {
        $token = $request->string('token', 256);
        $password = $request->string('password', 128);
        $tokenHash = Crypto::tokenHash($token);
        $this->rateLimiter->assertAllowed(
            'password-reset-ip',
            $this->rateLimiter->ipDimension($request->ip),
            8,
            3600
        );
        $this->rateLimiter->assertAllowed(
            'password-reset-token',
            $tokenHash,
            5,
            3600
        );
        $preflight = $this->database->pdo()->prepare(
            "SELECT ut.user_id, u.email
             FROM user_tokens ut
             JOIN users u ON u.id = ut.user_id
             WHERE ut.token_hash = :token_hash
               AND ut.type = 'password_reset'
               AND ut.used_at IS NULL
               AND ut.expires_at > CURRENT_TIMESTAMP(6)
             LIMIT 1"
        );
        $preflight->execute(['token_hash' => $tokenHash]);
        $preflightRow = $preflight->fetch();
        if (!is_array($preflightRow)) {
            sodium_memzero($password);
            throw new ApiException(400, 'INVALID_RESET_TOKEN', 'Link invalido ou expirado.');
        }

        try {
            PasswordPolicy::assertValid($password, (string) $preflightRow['email']);
            $passwordHash = PasswordPolicy::hash($password);
        } finally {
            sodium_memzero($password);
        }

        $this->database->transaction(function (PDO $pdo) use (
            $tokenHash,
            $passwordHash,
            $request
        ): void {
            $select = $pdo->prepare(
                "SELECT ut.*, u.email
                 FROM user_tokens ut
                 JOIN users u ON u.id = ut.user_id
                 WHERE ut.token_hash = :token_hash
                   AND ut.type = 'password_reset'
                   AND ut.used_at IS NULL
                   AND ut.expires_at > CURRENT_TIMESTAMP(6)
                 LIMIT 1 FOR UPDATE"
            );
            $select->execute(['token_hash' => $tokenHash]);
            $row = $select->fetch();
            if (!is_array($row)) {
                throw new ApiException(400, 'INVALID_RESET_TOKEN', 'Link invalido ou expirado.');
            }

            $pdo->prepare(
                'UPDATE users
                 SET password_hash = :password_hash,
                     password_changed_at = CURRENT_TIMESTAMP(6),
                     email_verified_at = COALESCE(
                         email_verified_at, CURRENT_TIMESTAMP(6)
                     )
                 WHERE id = :user_id'
            )->execute([
                'password_hash' => $passwordHash,
                'user_id' => $row['user_id'],
            ]);
            $pdo->prepare(
                'UPDATE user_tokens
                 SET used_at = CURRENT_TIMESTAMP(6)
                 WHERE user_id = :user_id AND used_at IS NULL'
            )->execute(['user_id' => $row['user_id']]);
            $pdo->prepare(
                'UPDATE refresh_tokens
                 SET revoked_at = COALESCE(revoked_at, CURRENT_TIMESTAMP(6))
                 WHERE user_id = :user_id'
            )->execute(['user_id' => $row['user_id']]);
            $pdo->prepare(
                'UPDATE web_sessions
                 SET revoked_at = COALESCE(revoked_at, CURRENT_TIMESTAMP(6))
                 WHERE user_id = :user_id'
            )->execute(['user_id' => $row['user_id']]);

            AuditLog::recordWithPdo(
                $pdo,
                'auth.password.reset',
                $request->requestId,
                (string) $row['user_id'],
                null,
                null
            );
        });
    }

    public function verifyEmail(string $token): void
    {
        $this->database->transaction(function (PDO $pdo) use ($token): void {
            $select = $pdo->prepare(
                "SELECT * FROM user_tokens
                 WHERE token_hash = :token_hash
                   AND type = 'email_verification'
                   AND used_at IS NULL
                   AND expires_at > CURRENT_TIMESTAMP(6)
                 LIMIT 1 FOR UPDATE"
            );
            $select->execute(['token_hash' => Crypto::tokenHash($token)]);
            $row = $select->fetch();
            if (!is_array($row)) {
                throw new ApiException(400, 'INVALID_VERIFICATION_TOKEN', 'Link invalido ou expirado.');
            }
            $pdo->prepare(
                'UPDATE users
                 SET email_verified_at = COALESCE(email_verified_at, CURRENT_TIMESTAMP(6))
                 WHERE id = :user_id'
            )->execute(['user_id' => $row['user_id']]);
            $pdo->prepare(
                'UPDATE user_tokens SET used_at = CURRENT_TIMESTAMP(6) WHERE id = :id'
            )->execute(['id' => $row['id']]);
        });
    }

    private static function padEnumerationSafeResponse(int $startedAt): void
    {
        $minimumNanoseconds = 200_000_000;
        $remainingNanoseconds = $minimumNanoseconds
            - (hrtime(true) - $startedAt);
        if ($remainingNanoseconds > 0) {
            usleep((int) ceil($remainingNanoseconds / 1000));
        }
    }

    public static function normaliseEmail(string $email): string
    {
        $email = mb_strtolower(trim($email));
        if (mb_strlen($email) > 254 || !filter_var($email, FILTER_VALIDATE_EMAIL)) {
            throw new ApiException(422, 'INVALID_EMAIL', 'E-mail invalido.');
        }
        return $email;
    }
}
