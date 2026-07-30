<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class LibraryDownloadService
{
    public function __construct(
        private readonly Config $config,
        private readonly Crypto $crypto,
        private readonly RateLimiter $rateLimiter
    ) {
    }

    public function stream(Request $request, string $fileName): never
    {
        $this->rateLimiter->assertAllowed(
            'library-download-ip',
            $this->rateLimiter->ipDimension($request->ip),
            600,
            3600
        );
        if (!self::isSafeLibraryFileName($fileName)) {
            throw new ApiException(403, 'DOWNLOAD_FORBIDDEN', 'Download indisponivel.');
        }

        // Church Live acompanha o produto e pode ser pre-carregado antes da
        // compra. Isso tambem preserva 1.2/1.3, cujos instaladores segmentados
        // ainda nao enviavam Authorization. A allowlist deliberadamente cobre
        // somente as 30 partes desta biblioteca base: expansoes e packs futuros
        // continuam exigindo um token assinado com entitlement de download.
        if (!self::isPublicBaseLibraryFileName($fileName)) {
            $token = $this->bearer($request);
            $claims = $this->crypto->verifyCompact(
                $token,
                ['at+jwt', 'churchdaw-lease+jwt', 'churchdaw-setup+jwt']
            );
            if (!self::isAuthorizedDownloadToken($token, $claims, time())) {
                throw new ApiException(
                    403,
                    'DOWNLOAD_FORBIDDEN',
                    'Download indisponivel.'
                );
            }
        }

        $root = realpath($this->config->string('library.storage_path'));
        $file = $root !== false ? realpath($root . DIRECTORY_SEPARATOR . $fileName) : false;
        if ($root === false
            || $file === false
            || !str_starts_with($file, $root . DIRECTORY_SEPARATOR)
            || !is_file($file)
            || !is_readable($file)) {
            throw new ApiException(404, 'PACK_NOT_FOUND', 'Arquivo inexistente.');
        }

        $size = filesize($file);
        if (!is_int($size) || $size <= 0) {
            throw new ApiException(404, 'PACK_NOT_FOUND', 'Arquivo inexistente.');
        }
        $start = 0;
        $end = $size - 1;
        $status = 200;
        $range = $request->header('range');
        if ($range !== '') {
            if (!preg_match('/^bytes=(\d+)-(\d*)$/', $range, $matches)) {
                header('Content-Range: bytes */' . $size);
                http_response_code(416);
                exit;
            }
            $start = (int) $matches[1];
            $end = $matches[2] === '' ? $end : (int) $matches[2];
            if ($start < 0 || $end < $start || $end >= $size) {
                header('Content-Range: bytes */' . $size);
                http_response_code(416);
                exit;
            }
            $status = 206;
        }

        $handle = fopen($file, 'rb');
        if ($handle === false || fseek($handle, $start) !== 0) {
            if (is_resource($handle)) {
                fclose($handle);
            }
            throw new ApiException(500, 'PACK_READ_FAILED', 'Falha ao abrir o arquivo.');
        }

        @set_time_limit(0);
        ignore_user_abort(true);
        while (ob_get_level() > 0) {
            ob_end_clean();
        }
        http_response_code($status);
        header('Content-Type: application/zip');
        header('Content-Disposition: attachment; filename="' . $fileName . '"');
        header('Content-Length: ' . ($end - $start + 1));
        header('Accept-Ranges: bytes');
        header('Cache-Control: private, no-store, max-age=0');
        header('X-Content-Type-Options: nosniff');
        header('Referrer-Policy: no-referrer');
        if ($status === 206) {
            header("Content-Range: bytes {$start}-{$end}/{$size}");
        }

        $remaining = $end - $start + 1;
        while ($remaining > 0 && !feof($handle)) {
            $chunk = fread($handle, min(1024 * 1024, $remaining));
            if (!is_string($chunk) || $chunk === '') {
                break;
            }
            echo $chunk;
            $remaining -= strlen($chunk);
            flush();
            if (connection_aborted()) {
                break;
            }
        }
        fclose($handle);
        exit;
    }

    private static function isSafeLibraryFileName(string $fileName): bool
    {
        return preg_match(
            '/^[A-Za-z0-9][A-Za-z0-9._-]{1,150}\.zip$/',
            $fileName
        ) === 1;
    }

    private static function isPublicBaseLibraryFileName(
        string $fileName
    ): bool {
        return preg_match(
            '/^church-live-v1-segment-(?:0[1-9]|[12][0-9]|30)\.zip$/',
            $fileName
        ) === 1;
    }

    /**
     * Autoriza somente os formatos que o ChurchDAW realmente emitiu:
     *
     * - access token pago Android, curto e com scope;
     * - lease paga Android nova, com scope e expiracao;
     * - lease paga Android legada, sem scope/exp, mas ainda dentro da janela
     *   offline assinada;
     * - reserva e lease da demonstracao Android, ambas com expiracao.
     *
     * Crypto::verifyCompact() ja validou RSA, issuer, kid, nbf/iat e `exp`
     * quando presente. As regras abaixo impedem confusao entre tipos de token,
     * audience desktop, produtos desconhecidos, scopes parecidos e credenciais
     * historicas com duracao fora do formato que chegou a producao.
     *
     * @param array<string, mixed> $claims
     */
    private static function isAuthorizedDownloadToken(
        string $token,
        array $claims,
        int $now
    ): bool {
        if (($claims['aud'] ?? '') !== 'churchdaw-android') {
            return false;
        }

        return match (self::compactType($token)) {
            'at+jwt' => self::isPaidAndroidAccess($claims, $now),
            'churchdaw-lease+jwt' =>
                self::isPaidAndroidLease($claims, $now)
                || self::isTrialAndroidLease($claims, $now),
            'churchdaw-setup+jwt' =>
                self::isTrialAndroidSetup($claims, $now),
            default => false,
        };
    }

    /** @param array<string, mixed> $claims */
    private static function isPaidAndroidAccess(array $claims, int $now): bool
    {
        $issuedAt = $claims['iat'] ?? null;
        $expiresAt = $claims['exp'] ?? null;
        return self::hasScope($claims, 'android:license')
            && self::hasScope($claims, 'library:download')
            && self::hasPaidAndroidIdentity($claims)
            && is_int($issuedAt)
            && is_int($expiresAt)
            && $expiresAt > $now
            && $expiresAt > $issuedAt
            // As releases publicadas emitiram 15 minutos. Uma hora deixa folga
            // operacional sem aceitar um access token anormalmente duradouro.
            && $expiresAt <= $issuedAt + 3600;
    }

    /** @param array<string, mixed> $claims */
    private static function isPaidAndroidLease(array $claims, int $now): bool
    {
        if (!self::hasPaidAndroidIdentity($claims)
            || !self::hasValidPaidLeaseWindow($claims, $now)) {
            return false;
        }

        $hasScopeClaim = array_key_exists('scope', $claims);
        $hasExpiryClaim = array_key_exists('exp', $claims);

        if (!$hasScopeClaim && !$hasExpiryClaim) {
            // Compatibilidade exata com 2026-07-28.2/.3/.4. Esses backends
            // assinaram a lease paga sem scope e sem exp; o APK persistiu essa
            // lease como bearer do instalador.
            return true;
        }

        return $hasScopeClaim
            && $hasExpiryClaim
            && self::hasScope($claims, 'android:license')
            && self::hasScope($claims, 'library:download')
            && is_int($claims['exp'])
            && $claims['exp'] === $claims['offline_until'];
    }

    /** @param array<string, mixed> $claims */
    private static function hasPaidAndroidIdentity(array $claims): bool
    {
        foreach (['sub', 'jti', 'license_id', 'activation_id'] as $claim) {
            if (!self::isUuid($claims[$claim] ?? null)) {
                return false;
            }
        }

        return ($claims['platform'] ?? '') === 'android'
            && ($claims['mode'] ?? '') === 'licensed'
            && in_array(
                $claims['product_code'] ?? null,
                ['churchdaw-perpetual', 'churchdaw-desktop-perpetual'],
                true
            )
            && self::isInstallationId($claims['installation_id'] ?? null);
    }

    /** @param array<string, mixed> $claims */
    private static function hasValidPaidLeaseWindow(
        array $claims,
        int $now
    ): bool {
        $issuedAt = $claims['iat'] ?? null;
        $leaseExpiresAt = $claims['lease_expires_at'] ?? null;
        $offlineUntil = $claims['offline_until'] ?? null;
        return is_int($issuedAt)
            && is_int($leaseExpiresAt)
            && is_int($offlineUntil)
            && $issuedAt <= $now + 60
            && $leaseExpiresAt > $issuedAt
            // Historicamente a lease online foi de 24 h.
            && $leaseExpiresAt <= $issuedAt + 2 * 86400
            && $offlineUntil >= $leaseExpiresAt
            && $offlineUntil > $now
            // Todas as releases distribuidas usaram sete dias.
            && $offlineUntil <= $issuedAt + 8 * 86400;
    }

    /** @param array<string, mixed> $claims */
    private static function isTrialAndroidSetup(array $claims, int $now): bool
    {
        $trialId = $claims['trial_id'] ?? null;
        $issuedAt = $claims['iat'] ?? null;
        $expiresAt = $claims['exp'] ?? null;
        return self::isUuid($trialId)
            && self::isUuid($claims['jti'] ?? null)
            && ($claims['sub'] ?? '') === 'trial:' . $trialId
            && ($claims['mode'] ?? '') === 'trial_setup'
            && self::isInstallationId($claims['installation_id'] ?? null)
            && is_string($claims['public_key_fingerprint'] ?? null)
            && preg_match(
                '/^[0-9a-f]{64}$/',
                (string) $claims['public_key_fingerprint']
            ) === 1
            && self::hasScope($claims, 'android:trial-start')
            && self::hasScope($claims, 'library:download')
            && is_int($issuedAt)
            && is_int($expiresAt)
            && $expiresAt > $now
            && $expiresAt > $issuedAt
            // Config::assertRuntimeReady limita a reserva a no maximo 72 h.
            && $expiresAt <= $issuedAt + 72 * 3600;
    }

    /** @param array<string, mixed> $claims */
    private static function isTrialAndroidLease(array $claims, int $now): bool
    {
        $trialId = $claims['trial_id'] ?? null;
        $issuedAt = $claims['iat'] ?? null;
        $expiresAt = $claims['exp'] ?? null;
        $leaseExpiresAt = $claims['lease_expires_at'] ?? null;
        $offlineUntil = $claims['offline_until'] ?? null;
        $trialExpiresAt = $claims['trial_expires_at'] ?? null;

        return self::isUuid($trialId)
            && self::isUuid($claims['jti'] ?? null)
            && ($claims['sub'] ?? '') === 'trial:' . $trialId
            && ($claims['activation_id'] ?? '') === 'trial:' . $trialId
            && ($claims['product_code'] ?? '') === 'churchdaw-demo'
            && ($claims['platform'] ?? '') === 'android'
            && ($claims['mode'] ?? '') === 'trial'
            && self::isInstallationId($claims['installation_id'] ?? null)
            && self::hasScope($claims, 'android:trial')
            && self::hasScope($claims, 'library:download')
            && is_int($issuedAt)
            && is_int($expiresAt)
            && is_int($leaseExpiresAt)
            && is_int($offlineUntil)
            && is_int($trialExpiresAt)
            && $expiresAt > $now
            && $expiresAt > $issuedAt
            && $expiresAt === $leaseExpiresAt
            && $expiresAt === $offlineUntil
            && $expiresAt === $trialExpiresAt
            // Config::assertRuntimeReady limita a demonstracao a 120 min.
            && $expiresAt <= $issuedAt + 120 * 60;
    }

    /** @param array<string, mixed> $claims */
    private static function hasScope(array $claims, string $required): bool
    {
        if (!is_string($claims['scope'] ?? null)) {
            return false;
        }

        $scopes = preg_split('/\s+/', trim($claims['scope']));
        return is_array($scopes) && in_array($required, $scopes, true);
    }

    private static function isUuid(mixed $value): bool
    {
        return is_string($value)
            && preg_match(
                '/^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-'
                    . '[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i',
                $value
            ) === 1;
    }

    private static function isInstallationId(mixed $value): bool
    {
        return is_string($value)
            && preg_match('/^[A-Za-z0-9_-]{16,128}$/', $value) === 1;
    }

    private static function compactType(string $token): ?string
    {
        $parts = explode('.', $token);
        if (count($parts) !== 3) {
            return null;
        }

        $headerJson = Crypto::base64UrlDecode($parts[0]);
        if (!is_string($headerJson)) {
            return null;
        }

        try {
            $header = json_decode($headerJson, true, 8, JSON_THROW_ON_ERROR);
        } catch (\JsonException) {
            return null;
        }

        return is_array($header) && is_string($header['typ'] ?? null)
            ? $header['typ']
            : null;
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
