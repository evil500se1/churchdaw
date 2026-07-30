<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;
use PDOException;

final class TrialService
{
    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto,
        private readonly RateLimiter $rateLimiter
    ) {
    }

    /** @return array<string, mixed> */
    public function reserve(Request $request): array
    {
        $identity = $this->identity($request);
        if ($this->isExistingExactIdentity($identity)) {
            // Reservar novamente no mesmo aparelho e uma operacao idempotente:
            // reabrir o app ou repetir depois de uma falha de rede nao pode
            // consumir o limite de criacao de demonstracoes.
            $this->rateLimiter->assertAllowed(
                'android-trial-reserve-idempotent-ip',
                $this->rateLimiter->ipDimension($request->ip),
                120,
                3600
            );
        } else {
            // O limite por IP precisa comportar igrejas, lojas e residencias
            // onde varios aparelhos compartilham o mesmo NAT.
            $this->rateLimiter->assertAllowed(
                'android-trial-reserve-ip',
                $this->rateLimiter->ipDimension($request->ip),
                64,
                86400
            );
            $this->rateLimiter->assertAllowed(
                'android-trial-reserve-device',
                (string) $identity['device_fingerprint_hash'],
                4,
                86400
            );
        }

        $trial = $this->database->transaction(function (PDO $pdo) use (
            $identity,
            $request
        ): array {
            $select = $pdo->prepare(
                'SELECT * FROM android_trials
                 WHERE device_fingerprint_hash = :fingerprint
                    OR installation_id = :installation_id
                 LIMIT 1 FOR UPDATE'
            );
            $select->execute([
                'fingerprint' => $identity['device_fingerprint_hash'],
                'installation_id' => $identity['installation_id'],
            ]);
            $existing = $select->fetch();
            if (is_array($existing)) {
                $sameDevice = hash_equals(
                    (string) $existing['device_fingerprint_hash'],
                    (string) $identity['device_fingerprint_hash']
                ) && hash_equals(
                    (string) $existing['public_key_fingerprint'],
                    (string) $identity['public_key_fingerprint']
                ) && hash_equals(
                    (string) $existing['installation_id'],
                    (string) $identity['installation_id']
                );
                if (!$sameDevice) {
                    throw new ApiException(
                        403,
                        'TRIAL_ALREADY_USED',
                        'A demonstracao deste aparelho ja foi utilizada.'
                    );
                }
                if ($existing['expires_at'] !== null
                    && strtotime((string) $existing['expires_at']) <= time()) {
                    $pdo->prepare(
                        "UPDATE android_trials SET status = 'expired'
                         WHERE id = :id"
                    )->execute(['id' => $existing['id']]);
                    throw new ApiException(
                        403,
                        'TRIAL_EXPIRED',
                        'A demonstracao de 20 minutos terminou.'
                    );
                }

                if ((string) $existing['status'] === 'reserved') {
                    $pdo->prepare(
                        'UPDATE android_trials
                         SET reservation_expires_at = FROM_UNIXTIME(:expires_at),
                             last_seen_at = CURRENT_TIMESTAMP(6),
                             last_ip_hash = :ip_hash
                         WHERE id = :id'
                    )->execute([
                        'expires_at' => time()
                            + 3600 * $this->config->int('trial.setup_hours'),
                        'ip_hash' => $this->crypto->hashIp($request->ip),
                        'id' => $existing['id'],
                    ]);
                    $existing['reservation_expires_at'] = gmdate(
                        'Y-m-d H:i:s',
                        time() + 3600 * $this->config->int('trial.setup_hours')
                    );
                }
                return $existing;
            }

            $id = Crypto::uuid();
            $reservationExpires = time()
                + 3600 * $this->config->int('trial.setup_hours');
            $pdo->prepare(
                'INSERT INTO android_trials
                    (id, installation_id, device_fingerprint_hash,
                     public_key_pem, public_key_fingerprint, device_label,
                     status, reservation_expires_at, last_ip_hash)
                 VALUES
                    (:id, :installation_id, :device_fingerprint_hash,
                     :public_key_pem, :public_key_fingerprint, :device_label,
                     \'reserved\', FROM_UNIXTIME(:reservation_expires_at),
                     :last_ip_hash)'
            )->execute([
                'id' => $id,
                'installation_id' => $identity['installation_id'],
                'device_fingerprint_hash' => $identity['device_fingerprint_hash'],
                'public_key_pem' => $identity['public_key_pem'],
                'public_key_fingerprint' => $identity['public_key_fingerprint'],
                'device_label' => $identity['device_label'],
                'reservation_expires_at' => $reservationExpires,
                'last_ip_hash' => $this->crypto->hashIp($request->ip),
            ]);
            return [
                'id' => $id,
                'installation_id' => $identity['installation_id'],
                'device_label' => $identity['device_label'],
                'public_key_fingerprint' => $identity['public_key_fingerprint'],
                'status' => 'reserved',
                'reservation_expires_at' =>
                    gmdate('Y-m-d H:i:s', $reservationExpires),
                'expires_at' => null,
            ];
        });

        if ((string) $trial['status'] === 'active') {
            return $this->activePayload($trial);
        }
        return $this->reservationPayload($trial);
    }

    /** @return array<string, mixed> */
    public function start(Request $request): array
    {
        $this->rateLimiter->assertAllowed(
            'android-trial-start-ip',
            $this->rateLimiter->ipDimension($request->ip),
            20,
            3600
        );
        $setupToken = $this->bearer($request);
        $claims = $this->crypto->verifyCompact(
            $setupToken,
            ['churchdaw-setup+jwt']
        );
        if (($claims['aud'] ?? '') !== 'churchdaw-android'
            || ($claims['mode'] ?? '') !== 'trial_setup'
            || !str_contains((string) ($claims['scope'] ?? ''), 'android:trial-start')) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }

        $installationId = $request->string('installation_id', 128);
        $nonce = $request->string('nonce', 128);
        $proof = $request->string('proof', 256);
        $timestamp = (int) ($request->body['timestamp'] ?? 0);
        if (!hash_equals((string) ($claims['installation_id'] ?? ''), $installationId)
            || !preg_match('/^[A-Za-z0-9+\/_=-]{16,128}$/', $nonce)
            || !preg_match('/^[A-Za-z0-9+\/=]{80,128}$/', $proof)
            || abs(time() - $timestamp) > 300) {
            throw new ApiException(401, 'INVALID_SESSION', 'Sessao invalida.');
        }

        $trial = $this->database->transaction(function (PDO $pdo) use (
            $claims,
            $installationId,
            $nonce,
            $proof,
            $timestamp,
            $setupToken,
            $request
        ): array {
            $select = $pdo->prepare(
                'SELECT * FROM android_trials WHERE id = :id LIMIT 1 FOR UPDATE'
            );
            $select->execute(['id' => (string) ($claims['trial_id'] ?? '')]);
            $row = $select->fetch();
            if (!is_array($row)
                || !hash_equals((string) $row['installation_id'], $installationId)
                || !hash_equals(
                    (string) $row['public_key_fingerprint'],
                    (string) ($claims['public_key_fingerprint'] ?? '')
                )) {
                throw new ApiException(401, 'INVALID_SESSION', 'Sessao invalida.');
            }
            if ($row['expires_at'] !== null
                && strtotime((string) $row['expires_at']) <= time()) {
                $pdo->prepare(
                    "UPDATE android_trials SET status = 'expired' WHERE id = :id"
                )->execute(['id' => $row['id']]);
                throw new ApiException(
                    403,
                    'TRIAL_EXPIRED',
                    'A demonstracao de 20 minutos terminou.'
                );
            }
            if ((string) $row['status'] === 'reserved'
                && strtotime((string) $row['reservation_expires_at']) <= time()) {
                throw new ApiException(
                    401,
                    'SETUP_EXPIRED',
                    'Prepare novamente a demonstracao.'
                );
            }

            try {
                $pdo->prepare(
                    'INSERT INTO android_trial_nonces
                        (trial_id, nonce_hash, expires_at)
                     VALUES
                        (:trial_id, :nonce_hash,
                         DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 10 MINUTE))'
                )->execute([
                    'trial_id' => $row['id'],
                    'nonce_hash' => hash('sha256', $nonce),
                ]);
            } catch (PDOException $error) {
                if ((string) $error->getCode() === '23000') {
                    throw new ApiException(
                        401,
                        'REPLAY_DETECTED',
                        'Sessao invalida.'
                    );
                }
                throw $error;
            }

            $message = $installationId . '|' . $timestamp . '|'
                . $nonce . '|' . $setupToken;
            $signature = base64_decode($proof, true);
            if (!is_string($signature)
                || strlen($signature) < 64
                || strlen($signature) > 80
                || openssl_verify(
                    $message,
                    $signature,
                    (string) $row['public_key_pem'],
                    OPENSSL_ALGO_SHA256
                ) !== 1) {
                throw new ApiException(
                    401,
                    'INVALID_DEVICE_PROOF',
                    'Sessao invalida.'
                );
            }

            if ((string) $row['status'] === 'reserved') {
                $expiresAt = time() + 60 * $this->config->int('trial.minutes');
                $pdo->prepare(
                    "UPDATE android_trials
                     SET status = 'active',
                         started_at = CURRENT_TIMESTAMP(6),
                         expires_at = FROM_UNIXTIME(:expires_at),
                         last_seen_at = CURRENT_TIMESTAMP(6),
                         last_ip_hash = :last_ip_hash
                     WHERE id = :id"
                )->execute([
                    'expires_at' => $expiresAt,
                    'last_ip_hash' => $this->crypto->hashIp($request->ip),
                    'id' => $row['id'],
                ]);
                $row['status'] = 'active';
                $row['started_at'] = gmdate('Y-m-d H:i:s');
                $row['expires_at'] = gmdate('Y-m-d H:i:s', $expiresAt);
            }
            return $row;
        });

        return $this->activePayload($trial);
    }

    /** @return array<string, mixed> */
    private function reservationPayload(array $trial): array
    {
        $now = time();
        $expiresAt = strtotime((string) $trial['reservation_expires_at']);
        $claims = [
            'iss' => $this->config->string('app.base_url'),
            'aud' => 'churchdaw-android',
            'sub' => 'trial:' . (string) $trial['id'],
            'jti' => Crypto::uuid(),
            'trial_id' => (string) $trial['id'],
            'installation_id' => (string) $trial['installation_id'],
            'public_key_fingerprint' =>
                (string) ($trial['public_key_fingerprint'] ?? ''),
            'mode' => 'trial_setup',
            'scope' => 'android:trial-start library:download',
            'iat' => $now,
            'nbf' => $now - 30,
            'exp' => $expiresAt,
        ];
        return [
            'mode' => 'trial_setup',
            'setup_token' => $this->crypto->signCompact(
                $claims,
                'churchdaw-setup+jwt'
            ),
            'setup_expires_at' => $expiresAt,
            'trial_seconds' => 60 * $this->config->int('trial.minutes'),
            'server_time' => $now,
        ];
    }

    /** @return array<string, mixed> */
    private function activePayload(array $trial): array
    {
        $now = time();
        $expiresAt = strtotime((string) $trial['expires_at']);
        if (!is_int($expiresAt) || $expiresAt <= $now) {
            throw new ApiException(
                403,
                'TRIAL_EXPIRED',
                'A demonstracao de 20 minutos terminou.'
            );
        }
        $claims = [
            'iss' => $this->config->string('app.base_url'),
            'aud' => 'churchdaw-android',
            'sub' => 'trial:' . (string) $trial['id'],
            'jti' => Crypto::uuid(),
            'trial_id' => (string) $trial['id'],
            'product_code' => 'churchdaw-demo',
            'activation_id' => 'trial:' . (string) $trial['id'],
            'activation_label' => (string) $trial['device_label'],
            'installation_id' => (string) $trial['installation_id'],
            'platform' => 'android',
            'mode' => 'trial',
            'scope' => 'android:trial library:download',
            'iat' => $now,
            'nbf' => $now - 30,
            'exp' => $expiresAt,
            'lease_expires_at' => $expiresAt,
            'offline_until' => $expiresAt,
            'trial_expires_at' => $expiresAt,
        ];
        $lease = $this->crypto->signCompact(
            $claims,
            'churchdaw-lease+jwt'
        );
        return [
            'mode' => 'trial',
            'lease_token' => $lease,
            'access_token' => $lease,
            'token_type' => 'Bearer',
            'expires_in' => $expiresAt - $now,
            'lease_expires_at' => $expiresAt,
            'offline_until' => $expiresAt,
            'trial_expires_at' => $expiresAt,
            'server_time' => $now,
        ];
    }

    /** @param array<string, string> $identity */
    private function isExistingExactIdentity(array $identity): bool
    {
        $select = $this->database->pdo()->prepare(
            'SELECT installation_id, device_fingerprint_hash,
                    public_key_fingerprint
             FROM android_trials
             WHERE device_fingerprint_hash = :fingerprint
                OR installation_id = :installation_id
             LIMIT 1'
        );
        $select->execute([
            'fingerprint' => $identity['device_fingerprint_hash'],
            'installation_id' => $identity['installation_id'],
        ]);
        $existing = $select->fetch();
        return is_array($existing)
            && hash_equals(
                (string) $existing['device_fingerprint_hash'],
                (string) $identity['device_fingerprint_hash']
            )
            && hash_equals(
                (string) $existing['public_key_fingerprint'],
                (string) $identity['public_key_fingerprint']
            )
            && hash_equals(
                (string) $existing['installation_id'],
                (string) $identity['installation_id']
            );
    }

    /** @return array<string, string> */
    private function identity(Request $request): array
    {
        $installationId = $request->string('installation_id', 128);
        $publicKeyEncoded = $request->string('device_public_key', 2048);
        $fingerprint = $request->string('device_fingerprint', 256);
        $label = $request->string('device_label', 120);
        $der = Crypto::base64UrlDecode($publicKeyEncoded);
        if (!preg_match('/^[A-Za-z0-9_-]{16,128}$/', $installationId)
            || !is_string($der)
            || strlen($der) < 80
            || strlen($der) > 512
            || !preg_match('/^[A-Za-z0-9._:-]{8,256}$/', $fingerprint)
            || $label === ''
            || mb_strlen($label) > 120) {
            throw new ApiException(422, 'INVALID_DEVICE', 'Dados do aparelho invalidos.');
        }
        $pem = "-----BEGIN PUBLIC KEY-----\n"
            . chunk_split(base64_encode($der), 64, "\n")
            . "-----END PUBLIC KEY-----\n";
        $key = openssl_pkey_get_public($pem);
        $details = $key !== false ? openssl_pkey_get_details($key) : false;
        $curve = is_array($details)
            ? (string) ($details['ec']['curve_name'] ?? '')
            : '';
        if (!is_array($details)
            || (int) ($details['type'] ?? -1) !== OPENSSL_KEYTYPE_EC
            || (int) ($details['bits'] ?? 0) !== 256
            || !in_array($curve, ['prime256v1', 'secp256r1'], true)) {
            throw new ApiException(422, 'INVALID_DEVICE_KEY', 'Chave do aparelho invalida.');
        }
        return [
            'installation_id' => $installationId,
            'device_fingerprint_hash' =>
                $this->crypto->hashDeviceFingerprint($fingerprint),
            'public_key_pem' => $pem,
            'public_key_fingerprint' => hash('sha256', $der),
            'device_label' => $label,
        ];
    }

    private function bearer(Request $request): string
    {
        $authorization = $request->header('authorization');
        if (!preg_match('/^Bearer ([A-Za-z0-9._-]{128,16384})$/', $authorization, $match)) {
            throw new ApiException(401, 'AUTH_REQUIRED', 'Autenticacao obrigatoria.');
        }
        return $match[1];
    }
}
