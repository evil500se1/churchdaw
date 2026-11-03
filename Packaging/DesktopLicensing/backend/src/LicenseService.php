<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use PDO;
use PDOException;

final class LicenseService
{
    public function __construct(
        private readonly Config $config,
        private readonly Database $database,
        private readonly Crypto $crypto
    ) {
    }

    /**
     * @param array<string, mixed> $user
     * @param array<string, mixed> $license
     * @return array<string, mixed>
     */
    public function activateForLogin(
        array $user,
        array $license,
        string $installationId,
        string $deviceSecret,
        string $devicePublicKey,
        string $deviceFingerprint,
        string $deviceLabel,
        string $platform,
        Request $request
    ): array {
        $identity = $this->validateDeviceIdentity(
            $installationId,
            $deviceSecret,
            $devicePublicKey,
            $deviceFingerprint,
            $deviceLabel,
            $platform
        );

        return $this->database->transaction(function (PDO $pdo) use (
            $user,
            $license,
            $installationId,
            $deviceLabel,
            $platform,
            $identity,
            $request
        ): array {
            $lockedLicense = $this->lockActiveLicense($pdo, (string) $license['id']);
            $activationPolicy = (string) (
                $lockedLicense['activation_policy'] ?? 'single_device'
            );
            $active = $this->activeActivation(
                $pdo,
                (string) $lockedLicense['id'],
                $activationPolicy === 'platform_bundle' ? $platform : null
            );
            $replacedAndroidDeviceId = null;

            if ($active !== null && !hash_equals(
                (string) $active['installation_id'],
                $installationId
            )) {
                // Uma desinstalacao apaga SharedPreferences e Android Keystore.
                // Alem disso, o ANDROID_ID e vinculado ao certificado usado para
                // assinar o APK em Android 8+, portanto uma troca de certificado
                // pode produzir outro installation_id no mesmo aparelho.
                //
                // Este metodo so e chamado depois de e-mail e senha terem sido
                // validados. Para Android -> Android, fazemos uma transferencia
                // atomica da unica vaga: a ativacao e todos os refresh tokens
                // antigos sao encerrados antes de criar a nova. Desktop continua
                // exigindo a transferencia explicita existente.
                if ($platform !== 'android'
                    || (string) ($active['platform'] ?? '') !== 'android') {
                    throw new ApiException(
                        409,
                        'ACTIVATION_LIMIT',
                        'Esta licenca ja esta ativa em outro aparelho.'
                    );
                }

                $replacedAndroidDeviceId = (string) $active['device_id'];
                $this->retireAndroidActivationForReinstall(
                    $pdo,
                    (string) $active['id'],
                    $replacedAndroidDeviceId
                );
                $active = null;
            }

            if ($active === null) {
                $activeCount = $this->activeActivationCount(
                    $pdo,
                    (string) $lockedLicense['id']
                );
                $maxActiveDevices = max(
                    1,
                    (int) ($lockedLicense['max_active_devices'] ?? 1)
                );
                if ($activeCount >= $maxActiveDevices) {
                    throw new ApiException(
                        409,
                        'ACTIVATION_LIMIT',
                        'Todas as vagas desta licenca ja estao em uso.'
                    );
                }
            }

            $fingerprint = (string) $identity['identity_fingerprint'];
            $device = $this->deviceByInstallation(
                $pdo,
                (string) $lockedLicense['id'],
                $installationId,
                $platform
            );

            if ($device !== null) {
                // ROTACAO DO SEGREDO EM REINSTALACAO.
                //
                // O installation_id passou a ser derivado do hardware
                // (31/07/2026), entao a MESMA maquina volta com o MESMO id
                // depois de formatar. Mas o segredo do aparelho continua
                // sorteado - e tem de continuar, senao bastaria conhecer o
                // UUID da placa para se passar por ela.
                //
                // Recusar aqui era o que fazia o cliente formatar e bater
                // em "acesso negado" (DEVICE_IDENTITY_MISMATCH): mesmo id,
                // segredo novo. Como este caminho e o activateForLogin -
                // so se chega nele APOS validar e-mail e senha -, quem
                // chegou aqui ja provou ser o dono da conta, e registrar o
                // segredo novo e o comportamento certo.
                //
                // A protecao que importa continua de pe: sem a senha nao
                // se chega aqui, e o /auth/refresh (que usa so o token)
                // segue exigindo o segredo exato.
                $mesmoSegredo = hash_equals(
                    (string) $device['secret_fingerprint'],
                    $fingerprint
                ) && hash_equals(
                    (string) ($device['identity_type'] ?? 'shared-secret-v1'),
                    (string) $identity['identity_type']
                );

                if (!$mesmoSegredo) {
                    $rotate = $pdo->prepare(
                        "UPDATE devices
                            SET secret_fingerprint = :secret_fingerprint,
                                identity_type = :identity_type,
                                secret_ciphertext = :secret_ciphertext,
                                public_key_pem = :public_key_pem
                          WHERE id = :id"
                    );
                    $rotate->execute([
                        'secret_fingerprint' => $fingerprint,
                        'identity_type' => (string) $identity['identity_type'],
                        'secret_ciphertext' => $identity['secret_ciphertext'],
                        'public_key_pem' => $identity['public_key_pem'],
                        'id' => $device['id'],
                    ]);

                    // A chave privada anterior nao existe mais depois de uma
                    // reinstalacao. Revogar seus tokens impede que uma copia da
                    // sessao antiga continue concorrendo com a identidade nova.
                    $this->revokeDeviceSessions($pdo, (string) $device['id']);
                }

                $update = $pdo->prepare(
                    "UPDATE devices
                     SET label = :label, platform = :platform, status = 'active',
                         device_fingerprint_hash = :device_fingerprint_hash,
                         last_seen_at = CURRENT_TIMESTAMP(6)
                      WHERE id = :id"
                );
                $update->execute([
                    'label' => $deviceLabel,
                    'platform' => $platform,
                    'device_fingerprint_hash' => $identity['device_fingerprint_hash'],
                    'id' => $device['id'],
                ]);
            } else {
                $device = [
                    'id' => Crypto::uuid(),
                    'installation_id' => $installationId,
                    'label' => $deviceLabel,
                    'platform' => $platform,
                    'identity_type' => $identity['identity_type'],
                    'secret_ciphertext' => $identity['secret_ciphertext'],
                    'public_key_pem' => $identity['public_key_pem'],
                    'secret_fingerprint' => $fingerprint,
                    'device_fingerprint_hash' => $identity['device_fingerprint_hash'],
                ];
                $insert = $pdo->prepare(
                    "INSERT INTO devices
                        (id, license_id, installation_id, label, platform,
                         identity_type, secret_ciphertext, public_key_pem,
                         secret_fingerprint, device_fingerprint_hash, status)
                     VALUES
                        (:id, :license_id, :installation_id, :label, :platform,
                         :identity_type, :secret_ciphertext, :public_key_pem,
                         :secret_fingerprint, :device_fingerprint_hash, 'active')"
                );
                $insert->execute([
                    'id' => $device['id'],
                    'license_id' => $lockedLicense['id'],
                    'installation_id' => $installationId,
                    'label' => $deviceLabel,
                    'platform' => $platform,
                    'identity_type' => $device['identity_type'],
                    'secret_ciphertext' => $device['secret_ciphertext'],
                    'public_key_pem' => $device['public_key_pem'],
                    'secret_fingerprint' => $fingerprint,
                    'device_fingerprint_hash' => $device['device_fingerprint_hash'],
                ]);
            }

            if ($active === null) {
                $active = [
                    'id' => Crypto::uuid(),
                    'device_id' => $device['id'],
                    'installation_id' => $installationId,
                    'label' => $deviceLabel,
                ];
                $insertActivation = $pdo->prepare(
                    "INSERT INTO activations
                        (id, license_id, device_id, platform, status)
                     VALUES
                        (:id, :license_id, :device_id, :platform, 'active')"
                );
                try {
                    $insertActivation->execute([
                        'id' => $active['id'],
                        'license_id' => $lockedLicense['id'],
                        'device_id' => $device['id'],
                        'platform' => $platform,
                    ]);
                } catch (PDOException $error) {
                    if ((string) $error->getCode() === '23000') {
                        throw new ApiException(
                            409,
                            'ACTIVATION_LIMIT',
                            'Esta licenca ja possui um aparelho ativo neste sistema.'
                        );
                    }
                    throw $error;
                }
            } else {
                $touch = $pdo->prepare(
                    'UPDATE activations
                     SET last_validated_at = CURRENT_TIMESTAMP(6)
                     WHERE id = :id'
                );
                $touch->execute(['id' => $active['id']]);
            }

            $familyId = Crypto::uuid();
            $session = $this->createRefreshToken(
                $pdo,
                $familyId,
                (string) $user['id'],
                (string) $lockedLicense['id'],
                (string) $device['id']
            );

            AuditLog::recordWithPdo(
                $pdo,
                'desktop.activation.validated',
                $request->requestId,
                (string) $user['id'],
                (string) $lockedLicense['id'],
                $this->crypto->hashIp($request->ip),
                [
                    'platform' => $platform,
                    'new_activation' => $active['id'],
                    'android_reinstall' => $replacedAndroidDeviceId !== null,
                    'previous_device_id' => $replacedAndroidDeviceId,
                ]
            );

            return $this->authorizationPayload(
                $user,
                $lockedLicense,
                [
                    'id' => $active['id'],
                    'installation_id' => $installationId,
                    'label' => $deviceLabel,
                    'platform' => $platform,
                ],
                $session['token']
            );
        });
    }

    /** @return array<string, mixed> */
    public function refresh(Request $request): array
    {
        $refreshToken = $request->string('refresh_token', 256);
        $installationId = $request->string('installation_id', 128);
        $nonce = $request->string('nonce', 128);
        $proof = $request->string('proof', 256);
        $timestamp = (int) ($request->body['timestamp'] ?? 0);

        if ($refreshToken === '' || $installationId === ''
            || !preg_match('/^[A-Za-z0-9+\/_=-]{16,128}$/', $nonce)
            || !preg_match('/^[A-Za-z0-9+\/=]{40,128}$/', $proof)
            || abs(time() - $timestamp) > 300) {
            throw new ApiException(401, 'INVALID_SESSION', 'Sessao invalida.');
        }

        $result = $this->database->transaction(function (PDO $pdo) use (
            $refreshToken,
            $installationId,
            $nonce,
            $proof,
            $timestamp,
            $request
        ): array {
            $select = $pdo->prepare(
                "SELECT rt.*, u.email, u.display_name, u.status AS user_status,
                        u.email_verified_at,
                        l.status AS license_status, l.product_code,
                        d.installation_id, d.label, d.platform,
                        d.identity_type, d.secret_ciphertext, d.public_key_pem,
                        d.status AS device_status,
                        a.id AS activation_id, a.status AS activation_status
                 FROM refresh_tokens rt
                 JOIN users u ON u.id = rt.user_id
                 JOIN licenses l ON l.id = rt.license_id
                 JOIN devices d ON d.id = rt.device_id
                 JOIN activations a
                   ON a.license_id = l.id AND a.device_id = d.id
                  AND a.status = 'active'
                 WHERE rt.token_hash = :token_hash
                 LIMIT 1 FOR UPDATE"
            );
            $select->execute(['token_hash' => Crypto::tokenHash($refreshToken)]);
            $row = $select->fetch();

            if (!is_array($row)) {
                throw new ApiException(401, 'INVALID_SESSION', 'Sessao invalida.');
            }

            if ($row['revoked_at'] !== null) {
                throw new ApiException(401, 'INVALID_SESSION', 'Sessao invalida.');
            }

            if (strtotime((string) $row['expires_at']) <= time()
                || $row['user_status'] !== 'active'
                || $row['license_status'] !== 'active'
                || $row['device_status'] !== 'active'
                || $row['activation_status'] !== 'active'
                || $row['email_verified_at'] === null
                || !hash_equals((string) $row['installation_id'], $installationId)) {
                throw new ApiException(401, 'LICENSE_REVOKED', 'Licenca indisponivel.');
            }

            $message = $installationId . '|' . $timestamp . '|' . $nonce . '|' . $refreshToken;
            $validProof = false;
            if ((string) $row['identity_type'] === 'android-keystore-ec-p256') {
                $signature = base64_decode($proof, true);
                $validProof = is_string($signature)
                    && strlen($signature) >= 64
                    && strlen($signature) <= 80
                    && openssl_verify(
                        $message,
                        $signature,
                        (string) $row['public_key_pem'],
                        OPENSSL_ALGO_SHA256
                    ) === 1;
            } else {
                $deviceSecret = $this->crypto->decrypt(
                    (string) $row['secret_ciphertext']
                );
                $expectedProof = base64_encode(
                    hash_hmac('sha256', $message, $deviceSecret, true)
                );
                sodium_memzero($deviceSecret);
                $validProof = hash_equals($expectedProof, $proof);
            }
            if (!$validProof) {
                throw new ApiException(401, 'INVALID_DEVICE_PROOF', 'Sessao invalida.');
            }

            $nonceInsert = $pdo->prepare(
                'INSERT INTO request_nonces (device_id, nonce_hash, expires_at)
                 VALUES (:device_id, :nonce_hash, DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 10 MINUTE))'
            );
            try {
                $nonceInsert->execute([
                    'device_id' => $row['device_id'],
                    'nonce_hash' => hash('sha256', $nonce),
                ]);
            } catch (PDOException $error) {
                if ((string) $error->getCode() === '23000') {
                    throw new ApiException(401, 'REPLAY_DETECTED', 'Sessao invalida.');
                }
                throw $error;
            }

            if ($row['consumed_at'] !== null) {
                $recoveredToken = $this->recoverReplacementRefreshToken($pdo, $row);
                if ($recoveredToken !== null) {
                    $this->touchActivation($pdo, (string) $row['activation_id']);
                    AuditLog::recordWithPdo(
                        $pdo,
                        'desktop.refresh.retry_recovered',
                        $request->requestId,
                        (string) $row['user_id'],
                        (string) $row['license_id'],
                        $this->crypto->hashIp($request->ip)
                    );
                    return $this->authorizationPayloadForRefreshRow($row, $recoveredToken);
                }

                $revoke = $pdo->prepare(
                    'UPDATE refresh_tokens
                     SET revoked_at = COALESCE(revoked_at, CURRENT_TIMESTAMP(6))
                     WHERE family_id = :family_id'
                );
                $revoke->execute(['family_id' => $row['family_id']]);
                AuditLog::recordWithPdo(
                    $pdo,
                    'desktop.refresh.reuse_detected',
                    $request->requestId,
                    (string) $row['user_id'],
                    (string) $row['license_id'],
                    $this->crypto->hashIp($request->ip)
                );
                return ['_committed_error' => 'SESSION_REUSED'];
            }

            $replacement = $this->createRefreshToken(
                $pdo,
                (string) $row['family_id'],
                (string) $row['user_id'],
                (string) $row['license_id'],
                (string) $row['device_id']
            );
            $consume = $pdo->prepare(
                'UPDATE refresh_tokens
                 SET consumed_at = CURRENT_TIMESTAMP(6), replaced_by_id = :replacement_id
                 WHERE id = :id'
            );
            $consume->execute([
                'replacement_id' => $replacement['id'],
                'id' => $row['id'],
            ]);

            $this->touchActivation($pdo, (string) $row['activation_id']);

            return $this->authorizationPayloadForRefreshRow($row, $replacement['token']);
        });

        if (($result['_committed_error'] ?? '') === 'SESSION_REUSED') {
            throw new ApiException(401, 'SESSION_REUSED', 'Sessao invalida.');
        }
        return $result;
    }

    public function deactivateCurrentDevice(string $userId, Request $request): void
    {
        $this->database->transaction(function (PDO $pdo) use ($userId, $request): void {
            $deviceId = trim($request->string('device_id', 36));
            $select = $pdo->prepare(
                "SELECT l.id AS license_id, a.id AS activation_id, a.device_id
                 FROM licenses l
                 JOIN activations a
                   ON a.license_id = l.id AND a.status = 'active'
                 WHERE l.user_id = :user_id AND l.status = 'active'
                   AND (:device_id = '' OR a.device_id = :device_id_match)
                 ORDER BY l.issued_at DESC LIMIT 1 FOR UPDATE"
            );
            $select->execute([
                'user_id' => $userId,
                'device_id' => $deviceId,
                'device_id_match' => $deviceId,
            ]);
            $active = $select->fetch();
            if (!is_array($active)) {
                return;
            }

            if ($deviceId === '') {
                $activeCount = $pdo->prepare(
                    "SELECT COUNT(*)
                     FROM licenses l
                     JOIN activations a
                       ON a.license_id = l.id AND a.status = 'active'
                     WHERE l.user_id = :user_id AND l.status = 'active'"
                );
                $activeCount->execute(['user_id' => $userId]);
                if ((int) $activeCount->fetchColumn() > 1) {
                    throw new ApiException(
                        422,
                        'DEVICE_REQUIRED',
                        'Escolha qual aparelho deseja desativar.'
                    );
                }
            }

            $count = $pdo->prepare(
                'SELECT COUNT(*) FROM license_transfers
                 WHERE license_id = :license_id
                   AND created_at >= DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 30 DAY)'
            );
            $count->execute(['license_id' => $active['license_id']]);
            if ((int) $count->fetchColumn() >= $this->config->int('license.max_transfers_30d')) {
                throw new ApiException(
                    429,
                    'TRANSFER_LIMIT',
                    'Limite de trocas atingido. Fale com o suporte.'
                );
            }

            $deactivate = $pdo->prepare(
                "UPDATE activations
                 SET status = 'deactivated',
                     deactivated_at = CURRENT_TIMESTAMP(6),
                     deactivation_reason = 'user_transfer'
                 WHERE id = :activation_id"
            );
            $deactivate->execute(['activation_id' => $active['activation_id']]);
            $pdo->prepare("UPDATE devices SET status = 'inactive' WHERE id = :id")
                ->execute(['id' => $active['device_id']]);
            $pdo->prepare(
                'UPDATE refresh_tokens
                 SET revoked_at = COALESCE(revoked_at, CURRENT_TIMESTAMP(6))
                 WHERE device_id = :device_id'
            )->execute(['device_id' => $active['device_id']]);
            $pdo->prepare(
                'INSERT INTO license_transfers
                    (id, license_id, from_device_id, requested_by_user_id)
                 VALUES (:id, :license_id, :device_id, :user_id)'
            )->execute([
                'id' => Crypto::uuid(),
                'license_id' => $active['license_id'],
                'device_id' => $active['device_id'],
                'user_id' => $userId,
            ]);

            AuditLog::recordWithPdo(
                $pdo,
                'desktop.activation.deactivated',
                $request->requestId,
                $userId,
                (string) $active['license_id'],
                $this->crypto->hashIp($request->ip)
            );
        });
    }

    /** @return array<string, mixed> */
    private function lockActiveLicense(PDO $pdo, string $licenseId): array
    {
        $statement = $pdo->prepare(
            "SELECT * FROM licenses
             WHERE id = :id AND status = 'active'
             LIMIT 1 FOR UPDATE"
        );
        $statement->execute(['id' => $licenseId]);
        $license = $statement->fetch();
        if (!is_array($license)) {
            throw new ApiException(403, 'LICENSE_REVOKED', 'Licenca indisponivel.');
        }
        return $license;
    }

    /** @return array<string, mixed>|null */
    private function activeActivation(
        PDO $pdo,
        string $licenseId,
        ?string $platform = null
    ): ?array
    {
        $statement = $pdo->prepare(
            "SELECT a.*, d.installation_id, d.label, d.platform,
                    d.device_fingerprint_hash
             FROM activations a
             JOIN devices d ON d.id = a.device_id
             WHERE a.license_id = :license_id
               AND a.status = 'active'
               AND (:platform = '' OR a.platform = :platform_match)
             LIMIT 1 FOR UPDATE"
        );
        $statement->execute([
            'license_id' => $licenseId,
            'platform' => $platform ?? '',
            'platform_match' => $platform ?? '',
        ]);
        $row = $statement->fetch();
        return is_array($row) ? $row : null;
    }

    private function activeActivationCount(PDO $pdo, string $licenseId): int
    {
        $statement = $pdo->prepare(
            "SELECT COUNT(*) FROM activations
             WHERE license_id = :license_id AND status = 'active'"
        );
        $statement->execute(['license_id' => $licenseId]);
        return (int) $statement->fetchColumn();
    }

    private function retireAndroidActivationForReinstall(
        PDO $pdo,
        string $activationId,
        string $deviceId
    ): void {
        $pdo->prepare(
            "UPDATE activations
             SET status = 'deactivated',
                 deactivated_at = CURRENT_TIMESTAMP(6),
                 deactivation_reason = 'android_reinstall'
             WHERE id = :id AND status = 'active'"
        )->execute(['id' => $activationId]);
        $pdo->prepare(
            "UPDATE devices SET status = 'inactive' WHERE id = :id"
        )->execute(['id' => $deviceId]);
        $this->revokeDeviceSessions($pdo, $deviceId);
    }

    private function revokeDeviceSessions(PDO $pdo, string $deviceId): void
    {
        $pdo->prepare(
            'UPDATE refresh_tokens
             SET revoked_at = COALESCE(revoked_at, CURRENT_TIMESTAMP(6))
             WHERE device_id = :device_id'
        )->execute(['device_id' => $deviceId]);
        $pdo->prepare(
            'DELETE FROM request_nonces WHERE device_id = :device_id'
        )->execute(['device_id' => $deviceId]);
    }

    /** @return array<string, mixed>|null */
    private function deviceByInstallation(
        PDO $pdo,
        string $licenseId,
        string $installationId,
        string $platform
    ): ?array {
        $statement = $pdo->prepare(
            'SELECT * FROM devices
             WHERE license_id = :license_id
               AND installation_id = :installation_id
               AND platform = :platform
             LIMIT 1 FOR UPDATE'
        );
        $statement->execute([
            'license_id' => $licenseId,
            'installation_id' => $installationId,
            'platform' => $platform,
        ]);
        $row = $statement->fetch();
        return is_array($row) ? $row : null;
    }

    /** @return array{id: string, token: string} */
    private function createRefreshToken(
        PDO $pdo,
        string $familyId,
        string $userId,
        string $licenseId,
        string $deviceId
    ): array {
        $id = Crypto::uuid();
        $token = $this->crypto->deriveRefreshToken(
            $id,
            $familyId,
            $userId,
            $licenseId,
            $deviceId
        );
        $expires = time() + 86400 * $this->config->int('license.refresh_days');
        $statement = $pdo->prepare(
            'INSERT INTO refresh_tokens
                (id, family_id, user_id, license_id, device_id, token_hash, expires_at)
             VALUES
                (:id, :family_id, :user_id, :license_id, :device_id,
                 :token_hash, FROM_UNIXTIME(:expires_at))'
        );
        $statement->execute([
            'id' => $id,
            'family_id' => $familyId,
            'user_id' => $userId,
            'license_id' => $licenseId,
            'device_id' => $deviceId,
            'token_hash' => Crypto::tokenHash($token),
            'expires_at' => $expires,
        ]);
        return ['id' => $id, 'token' => $token];
    }

    /**
     * @param array<string, mixed> $row
     */
    private function recoverReplacementRefreshToken(PDO $pdo, array $row): ?string
    {
        $replacementId = (string) ($row['replaced_by_id'] ?? '');
        if ($replacementId === '') {
            return null;
        }

        $select = $pdo->prepare(
            'SELECT id, family_id, user_id, license_id, device_id, token_hash
             FROM refresh_tokens
             WHERE id = :id AND family_id = :family_id
               AND user_id = :user_id AND license_id = :license_id
               AND device_id = :device_id
               AND consumed_at IS NULL AND revoked_at IS NULL
               AND expires_at > CURRENT_TIMESTAMP(6)
             LIMIT 1 FOR UPDATE'
        );
        $select->execute([
            'id' => $replacementId,
            'family_id' => $row['family_id'],
            'user_id' => $row['user_id'],
            'license_id' => $row['license_id'],
            'device_id' => $row['device_id'],
        ]);
        $replacement = $select->fetch();
        if (!is_array($replacement)) {
            return null;
        }

        $token = $this->crypto->deriveRefreshToken(
            (string) $replacement['id'],
            (string) $replacement['family_id'],
            (string) $replacement['user_id'],
            (string) $replacement['license_id'],
            (string) $replacement['device_id']
        );
        if (!hash_equals((string) $replacement['token_hash'], Crypto::tokenHash($token))) {
            sodium_memzero($token);
            return null;
        }
        return $token;
    }

    private function touchActivation(PDO $pdo, string $activationId): void
    {
        $touch = $pdo->prepare(
            'UPDATE activations
             SET last_validated_at = CURRENT_TIMESTAMP(6)
             WHERE id = :activation_id'
        );
        $touch->execute(['activation_id' => $activationId]);
    }

    /**
     * @param array<string, mixed> $row
     * @return array<string, mixed>
     */
    private function authorizationPayloadForRefreshRow(array $row, string $refreshToken): array
    {
        return $this->authorizationPayload(
            [
                'id' => $row['user_id'],
                'email' => $row['email'],
                'display_name' => $row['display_name'],
            ],
            [
                'id' => $row['license_id'],
                'product_code' => $row['product_code'],
            ],
            [
                'id' => $row['activation_id'],
                'installation_id' => $row['installation_id'],
                'label' => $row['label'],
                'platform' => $row['platform'],
            ],
            $refreshToken
        );
    }

    /**
     * @param array<string, mixed> $user
     * @param array<string, mixed> $license
     * @param array<string, mixed> $activation
     * @return array<string, mixed>
     */
    private function authorizationPayload(
        array $user,
        array $license,
        array $activation,
        string $refreshToken
    ): array {
        $now = time();
        $leaseExpiresAt = $now + 3600 * $this->config->int('license.lease_hours');
        $offlineUntil = $now + 86400 * $this->config->int('license.offline_days');
        $platform = (string) ($activation['platform'] ?? 'windows');
        $isAndroid = $platform === 'android';
        $baseClaims = [
            'iss' => $this->config->string('app.base_url'),
            'aud' => $isAndroid ? 'churchdaw-android' : 'churchdaw-desktop',
            'sub' => (string) $user['id'],
            'email' => (string) $user['email'],
            'display_name' => (string) $user['display_name'],
            'license_id' => (string) $license['id'],
            'product_code' => (string) $license['product_code'],
            'activation_id' => (string) $activation['id'],
            'activation_label' => (string) $activation['label'],
            'installation_id' => (string) $activation['installation_id'],
            'platform' => $platform,
            'mode' => 'licensed',
            'iat' => $now,
            'nbf' => $now - 30,
        ];

        $leaseClaims = $baseClaims + [
            'jti' => Crypto::uuid(),
            // O Android usa a lease assinada tanto para o gate offline quanto
            // como bearer do instalador de bibliotecas. O access_token de 15
            // minutos nao e persistido pelo app e, portanto, nao chega ao
            // download segmentado/resumivel.
            //
            // A autorizacao de download precisa expirar: sem `exp`, adicionar
            // apenas o scope transformaria a lease em credencial de download
            // indefinida. A mesma janela offline ja aceita pelo cliente limita
            // esse acesso e ainda comporta o pack grande em conexoes lentas.
            'scope' => $isAndroid
                ? 'android:license library:download'
                : 'desktop:license',
            'exp' => $offlineUntil,
            'lease_expires_at' => $leaseExpiresAt,
            'offline_until' => $offlineUntil,
        ];
        $accessClaims = $baseClaims + [
            'jti' => Crypto::uuid(),
            'scope' => $isAndroid
                ? 'android:license library:download'
                : 'desktop:license library:download',
            'exp' => $now + 900,
        ];

        return [
            'access_token' => $this->crypto->signCompact($accessClaims, 'at+jwt'),
            'token_type' => 'Bearer',
            'expires_in' => 900,
            'refresh_token' => $refreshToken,
            'lease_token' => $this->crypto->signCompact($leaseClaims, 'churchdaw-lease+jwt'),
            'email' => (string) $user['email'],
            'display_name' => (string) $user['display_name'],
            'activation_label' => (string) $activation['label'],
            'lease_expires_at' => $leaseExpiresAt,
            'offline_until' => $offlineUntil,
            'server_time' => $now,
        ];
    }

    /**
     * @return array{
     *   identity_type: string,
     *   identity_fingerprint: string,
     *   secret_ciphertext: ?string,
     *   public_key_pem: ?string,
     *   device_fingerprint_hash: ?string
     * }
     */
    private function validateDeviceIdentity(
        string $installationId,
        string $deviceSecret,
        string $devicePublicKey,
        string $deviceFingerprint,
        string $deviceLabel,
        string $platform
    ): array {
        if (!preg_match('/^[A-Za-z0-9_-]{16,128}$/', $installationId)
            || $deviceLabel === ''
            || mb_strlen($deviceLabel) > 120
            || !in_array($platform, ['windows', 'macos', 'android'], true)) {
            throw new ApiException(422, 'INVALID_DEVICE', 'Dados do aparelho invalidos.');
        }

        if ($platform === 'android') {
            $der = Crypto::base64UrlDecode($devicePublicKey);
            if (!is_string($der) || strlen($der) < 80 || strlen($der) > 512
                || !preg_match('/^[A-Za-z0-9._:-]{8,256}$/', $deviceFingerprint)) {
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
                'identity_type' => 'android-keystore-ec-p256',
                'identity_fingerprint' => hash('sha256', $der),
                'secret_ciphertext' => null,
                'public_key_pem' => $pem,
                'device_fingerprint_hash' =>
                    $this->crypto->hashDeviceFingerprint($deviceFingerprint),
            ];
        }

        $decodedSecret = Crypto::base64UrlDecode($deviceSecret);
        if (!is_string($decodedSecret)
            || strlen($decodedSecret) < 32
            || strlen($decodedSecret) > 64) {
            throw new ApiException(422, 'INVALID_DEVICE', 'Dados do aparelho invalidos.');
        }
        return [
            'identity_type' => 'shared-secret-v1',
            'identity_fingerprint' => hash('sha256', $deviceSecret),
            'secret_ciphertext' => $this->crypto->encrypt($deviceSecret),
            'public_key_pem' => null,
            'device_fingerprint_hash' => null,
        ];
    }
}
