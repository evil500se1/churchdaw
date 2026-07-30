<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use OpenSSLAsymmetricKey;
use RuntimeException;

final class Crypto
{
    public function __construct(private readonly Config $config)
    {
    }

    public static function randomToken(int $bytes = 32): string
    {
        return self::base64UrlEncode(random_bytes($bytes));
    }

    public static function uuid(): string
    {
        $bytes = random_bytes(16);
        $bytes[6] = chr((ord($bytes[6]) & 0x0f) | 0x40);
        $bytes[8] = chr((ord($bytes[8]) & 0x3f) | 0x80);
        $hex = bin2hex($bytes);
        return sprintf(
            '%s-%s-%s-%s-%s',
            substr($hex, 0, 8),
            substr($hex, 8, 4),
            substr($hex, 12, 4),
            substr($hex, 16, 4),
            substr($hex, 20)
        );
    }

    public static function tokenHash(string $token): string
    {
        return hash('sha256', $token);
    }

    public function hashIp(string $ip): string
    {
        return hash_hmac('sha256', $ip, $this->config->string('app.ip_hash_key'));
    }

    public function hashDeviceFingerprint(string $fingerprint): string
    {
        return hash_hmac(
            'sha256',
            "churchdaw-device-v1\0" . $fingerprint,
            $this->config->string('app.ip_hash_key')
        );
    }

    public function encrypt(string $plaintext): string
    {
        $key = base64_decode($this->config->string('app.encryption_key'), true);
        if ($key === false || strlen($key) !== SODIUM_CRYPTO_SECRETBOX_KEYBYTES) {
            throw new RuntimeException('Chave de criptografia invalida.');
        }
        $nonce = random_bytes(SODIUM_CRYPTO_SECRETBOX_NONCEBYTES);
        return self::base64UrlEncode($nonce . sodium_crypto_secretbox($plaintext, $nonce, $key));
    }

    public function decrypt(string $ciphertext): string
    {
        $packed = self::base64UrlDecode($ciphertext);
        $key = base64_decode($this->config->string('app.encryption_key'), true);
        if ($packed === false || $key === false
            || strlen($packed) <= SODIUM_CRYPTO_SECRETBOX_NONCEBYTES) {
            throw new RuntimeException('Credencial de dispositivo corrompida.');
        }
        $nonce = substr($packed, 0, SODIUM_CRYPTO_SECRETBOX_NONCEBYTES);
        $opened = sodium_crypto_secretbox_open(
            substr($packed, SODIUM_CRYPTO_SECRETBOX_NONCEBYTES),
            $nonce,
            $key
        );
        if ($opened === false) {
            throw new RuntimeException('Credencial de dispositivo invalida.');
        }
        return $opened;
    }

    /** @param array<string, mixed> $claims */
    public function signCompact(array $claims, string $type): string
    {
        $header = [
            'alg' => 'RS256',
            'typ' => $type,
            'kid' => $this->config->string('license.key_id'),
        ];
        $encodedHeader = self::base64UrlEncode(json_encode($header, JSON_THROW_ON_ERROR));
        $encodedPayload = self::base64UrlEncode(json_encode(
            $claims,
            JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR
        ));
        $message = $encodedHeader . '.' . $encodedPayload;

        $privateKey = $this->privateKey();
        $signature = '';
        if (!openssl_sign($message, $signature, $privateKey, OPENSSL_ALGO_SHA256)) {
            throw new RuntimeException('Falha ao assinar a licenca.');
        }
        return $message . '.' . self::base64UrlEncode($signature);
    }

    /**
     * @param list<string> $allowedTypes
     * @return array<string, mixed>
     */
    public function verifyCompact(string $token, array $allowedTypes): array
    {
        if (strlen($token) < 128 || strlen($token) > 16384) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }

        $parts = explode('.', $token);
        if (count($parts) !== 3) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }

        [$encodedHeader, $encodedPayload, $encodedSignature] = $parts;
        $headerJson = self::base64UrlDecode($encodedHeader);
        $payloadJson = self::base64UrlDecode($encodedPayload);
        $signature = self::base64UrlDecode($encodedSignature);
        if (!is_string($headerJson)
            || !is_string($payloadJson)
            || !is_string($signature)) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }

        try {
            $header = json_decode($headerJson, true, 32, JSON_THROW_ON_ERROR);
            $claims = json_decode($payloadJson, true, 64, JSON_THROW_ON_ERROR);
        } catch (\JsonException) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }
        if (!is_array($header)
            || !is_array($claims)
            || ($header['alg'] ?? '') !== 'RS256'
            || !in_array((string) ($header['typ'] ?? ''), $allowedTypes, true)
            || !hash_equals(
                $this->config->string('license.key_id'),
                (string) ($header['kid'] ?? '')
            )) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }

        $privateKey = $this->privateKey();
        $details = openssl_pkey_get_details($privateKey);
        $publicKey = is_array($details)
            ? openssl_pkey_get_public((string) ($details['key'] ?? ''))
            : false;
        if ($publicKey === false
            || openssl_verify(
                $encodedHeader . '.' . $encodedPayload,
                $signature,
                $publicKey,
                OPENSSL_ALGO_SHA256
            ) !== 1) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }

        $now = time();
        if (!is_int($claims['nbf'] ?? null)
            || !is_int($claims['iat'] ?? null)
            || (int) $claims['nbf'] > $now + 60
            || (int) $claims['iat'] > $now + 60
            || !hash_equals(
                $this->config->string('app.base_url'),
                (string) ($claims['iss'] ?? '')
            )) {
            throw new ApiException(401, 'INVALID_TOKEN', 'Token invalido.');
        }
        if (array_key_exists('exp', $claims)
            && (!is_int($claims['exp']) || (int) $claims['exp'] <= $now)) {
            throw new ApiException(401, 'TOKEN_EXPIRED', 'Token expirado.');
        }
        return $claims;
    }

    public static function base64UrlEncode(string $value): string
    {
        return rtrim(strtr(base64_encode($value), '+/', '-_'), '=');
    }

    public static function base64UrlDecode(string $value): string|false
    {
        if (!preg_match('/^[A-Za-z0-9_-]*$/', $value)) {
            return false;
        }
        $padding = (4 - strlen($value) % 4) % 4;
        return base64_decode(strtr($value, '-_', '+/') . str_repeat('=', $padding), true);
    }

    private function privateKey(): OpenSSLAsymmetricKey
    {
        $contents = file_get_contents($this->config->string('license.private_key_path'));
        if ($contents === false) {
            throw new RuntimeException('Nao foi possivel ler a chave privada.');
        }
        $key = openssl_pkey_get_private($contents);
        if ($key === false) {
            throw new RuntimeException('Chave privada RSA invalida.');
        }
        return $key;
    }
}
