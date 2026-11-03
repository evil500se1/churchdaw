<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class WooviClient implements WooviProvider
{
    private const PRODUCTION_API = 'https://api.woovi.com';
    private const SANDBOX_API = 'https://api.woovi-sandbox.com';
    private const WEBHOOK_PUBLIC_KEY = <<<'PEM'
-----BEGIN PUBLIC KEY-----
MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQC/+NtIkjzevvqD+I3MMv3bLXDt
pvxBjY4BsRrSdca3rtAwMcRYYvxSnd7jagVLpctMiOxQO8ieUCKLSWHpsMAjO/zZ
WMKbqoG8MNpi/u3fp6zz0mcHCOSqYsPUUG19buW8bis5ZZ2IZgBObWSpTvJ0cnj6
HKBAA82Jln+lGwS1MwIDAQAB
-----END PUBLIC KEY-----
PEM;

    public function __construct(private readonly Config $config)
    {
    }

    public function createCharge(array $payload): array
    {
        return $this->request(
            'POST',
            '/api/v1/charge?return_existing=true',
            $payload
        );
    }

    public function charge(string $correlationId): array
    {
        if (!preg_match('/^[a-f0-9-]{36}$/i', $correlationId)) {
            throw new \InvalidArgumentException('Cobranca Woovi invalida.');
        }
        return $this->request(
            'GET',
            '/api/v1/charge/' . rawurlencode($correlationId)
        );
    }

    public function verifyWebhookSignature(string $payload, string $signature): bool
    {
        if ($payload === '' || !preg_match('/^[A-Za-z0-9+\/=]{80,1024}$/', $signature)) {
            return false;
        }
        $decoded = base64_decode($signature, true);
        if (!is_string($decoded)) {
            return false;
        }
        return openssl_verify(
            $payload,
            $decoded,
            self::WEBHOOK_PUBLIC_KEY,
            OPENSSL_ALGO_SHA256
        ) === 1;
    }

    /** @param array<string, mixed>|null $payload @return array<string, mixed> */
    private function request(
        string $method,
        string $path,
        ?array $payload = null
    ): array {
        $base = $this->config->bool('woovi.sandbox')
            ? self::SANDBOX_API : self::PRODUCTION_API;
        $handle = curl_init($base . $path);
        if ($handle === false) {
            throw new PaymentProviderException(
                'Falha ao iniciar cliente Woovi.',
                false
            );
        }
        $options = [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_CUSTOMREQUEST => $method,
            CURLOPT_HTTPHEADER => [
                'Authorization: ' . $this->config->string('woovi.api_key'),
                'Accept: application/json',
                'Content-Type: application/json',
                'User-Agent: ChurchDAW-Licensing/1.0',
            ],
            CURLOPT_CONNECTTIMEOUT => 5,
            CURLOPT_TIMEOUT => 12,
            CURLOPT_FOLLOWLOCATION => false,
            CURLOPT_SSL_VERIFYPEER => true,
            CURLOPT_SSL_VERIFYHOST => 2,
            CURLOPT_PROTOCOLS => CURLPROTO_HTTPS,
            CURLOPT_MAXREDIRS => 0,
        ];
        if ($payload !== null) {
            $options[CURLOPT_POSTFIELDS] = json_encode(
                $payload,
                JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE
                    | JSON_THROW_ON_ERROR
            );
        }
        curl_setopt_array($handle, $options);
        $raw = curl_exec($handle);
        $status = (int) curl_getinfo($handle, CURLINFO_RESPONSE_CODE);
        $curlError = curl_error($handle);
        curl_close($handle);

        $mutation = strtoupper($method) !== 'GET';
        if (!is_string($raw) || $raw === '' || $curlError !== '') {
            throw new PaymentProviderException(
                'Woovi indisponivel no momento.',
                $mutation
            );
        }
        if (strlen($raw) > 1024 * 1024) {
            throw new PaymentProviderException(
                'Resposta Woovi excedeu o limite.',
                $mutation && $status >= 200 && $status < 300
            );
        }
        try {
            $decoded = json_decode($raw, true, 64, JSON_THROW_ON_ERROR);
        } catch (\JsonException $error) {
            throw new PaymentProviderException(
                'Resposta Woovi invalida.',
                $mutation && $status >= 200 && $status < 300,
                $error
            );
        }
        if (!is_array($decoded)) {
            throw new PaymentProviderException('Resposta Woovi invalida.', false);
        }
        if ($status < 200 || $status >= 300) {
            $providerCode = mb_substr(
                (string) ($decoded['error'] ?? $decoded['message'] ?? 'unknown'),
                0,
                160
            );
            error_log("ChurchDAW Woovi HTTP {$status}: {$providerCode}");
            throw new PaymentProviderException(
                'A Woovi recusou a operacao.',
                $mutation && in_array($status, [408, 409, 425, 429], true)
                    || ($mutation && $status >= 500)
            );
        }
        return $decoded;
    }
}
