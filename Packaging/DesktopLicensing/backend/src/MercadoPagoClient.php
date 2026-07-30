<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

final class MercadoPagoClient implements PaymentProvider
{
    private const API_BASE = 'https://api.mercadopago.com';

    public function __construct(private readonly Config $config)
    {
    }

    /**
     * @param array<string, mixed> $payload
     * @return array<string, mixed>
     */
    public function createPreference(array $payload): array
    {
        return $this->request(
            'POST',
            '/checkout/preferences',
            $payload
        );
    }

    public function findPreferenceByExternalReference(string $externalReference): ?array
    {
        if (!preg_match('/^[a-f0-9-]{36}$/i', $externalReference)) {
            throw new \InvalidArgumentException('Referencia externa invalida.');
        }
        $response = $this->request(
            'GET',
            '/checkout/preferences/search?external_reference='
                . rawurlencode($externalReference)
        );
        $match = null;
        foreach (($response['elements'] ?? []) as $preference) {
            if (is_array($preference)
                && hash_equals(
                    $externalReference,
                    (string) ($preference['external_reference'] ?? '')
                )) {
                if ($match !== null) {
                    throw new \RuntimeException(
                        'Mais de uma preferencia encontrada para a mesma compra.'
                    );
                }
                $match = $preference;
            }
        }
        if ($match === null) {
            return null;
        }
        $preferenceId = (string) ($match['id'] ?? '');
        if (!preg_match('/^[A-Za-z0-9._:-]{1,128}$/', $preferenceId)) {
            throw new \RuntimeException(
                'Preferencia recuperada possui identificador invalido.'
            );
        }
        return $this->request(
            'GET',
            '/checkout/preferences/' . rawurlencode($preferenceId)
        );
    }

    public function preference(string $preferenceId): array
    {
        if (!preg_match('/^[A-Za-z0-9._:-]{1,128}$/', $preferenceId)) {
            throw new \InvalidArgumentException(
                'Identificador de preferencia invalido.'
            );
        }
        return $this->request(
            'GET',
            '/checkout/preferences/' . rawurlencode($preferenceId)
        );
    }

    /** @return array<string, mixed> */
    public function payment(string $paymentId): array
    {
        if (!preg_match('/^[0-9]{1,32}$/', $paymentId)) {
            throw new ApiException(400, 'INVALID_PAYMENT_ID', 'Pagamento invalido.');
        }
        return $this->request('GET', '/v1/payments/' . rawurlencode($paymentId));
    }

    public function merchantOrder(string $merchantOrderId): array
    {
        if (!preg_match('/^[0-9]{1,32}$/', $merchantOrderId)) {
            throw new ApiException(
                400,
                'INVALID_MERCHANT_ORDER_ID',
                'Pedido do provedor invalido.'
            );
        }
        return $this->request(
            'GET',
            '/merchant_orders/' . rawurlencode($merchantOrderId)
        );
    }

    public function merchantOrdersByExternalReference(
        string $externalReference
    ): array {
        if (!preg_match('/^[a-f0-9-]{36}$/i', $externalReference)) {
            throw new \InvalidArgumentException('Referencia externa invalida.');
        }
        $response = $this->request(
            'GET',
            '/merchant_orders/search?external_reference='
                . rawurlencode($externalReference)
                . '&limit=100&offset=0'
        );
        if ((int) ($response['total'] ?? 0) > 100) {
            throw new \RuntimeException(
                'Mais de cem pedidos comerciais exigem revisao manual.'
            );
        }
        $matches = [];
        foreach (($response['elements'] ?? []) as $merchantOrder) {
            if (!is_array($merchantOrder)
                || !hash_equals(
                    $externalReference,
                    (string) ($merchantOrder['external_reference'] ?? '')
                )) {
                continue;
            }
            $merchantOrderId = (string) ($merchantOrder['id'] ?? '');
            if (!preg_match('/^[0-9]{1,32}$/', $merchantOrderId)) {
                throw new \RuntimeException(
                    'Busca retornou pedido comercial invalido.'
                );
            }
            $matches[] = $merchantOrder;
        }
        return $matches;
    }

    public function chargeback(string $chargebackId): array
    {
        if (!preg_match('/^[A-Za-z0-9._:-]{1,128}$/', $chargebackId)) {
            throw new ApiException(
                400,
                'INVALID_CHARGEBACK_ID',
                'Contestacao invalida.'
            );
        }
        return $this->request(
            'GET',
            '/v1/chargebacks/' . rawurlencode($chargebackId)
        );
    }

    public function verifyWebhookSignature(
        string $signatureHeader,
        string $requestId,
        string $dataId
    ): bool {
        if ($signatureHeader === '' || $requestId === '' || $dataId === '') {
            return false;
        }

        $parts = [];
        foreach (explode(',', $signatureHeader) as $part) {
            $pair = explode('=', trim($part), 2);
            if (count($pair) === 2) {
                $parts[trim($pair[0])] = trim($pair[1]);
            }
        }

        $timestamp = $parts['ts'] ?? '';
        $signature = $parts['v1'] ?? '';
        if (!preg_match('/^[0-9]{9,13}$/', $timestamp)
            || !preg_match('/^[a-f0-9]{64}$/i', $signature)
            || !preg_match('/^[A-Za-z0-9._:-]{1,160}$/', $requestId)
            || !preg_match('/^[A-Za-z0-9._:-]{1,160}$/', $dataId)) {
            return false;
        }

        $manifest = 'id:' . strtolower($dataId)
                  . ';request-id:' . $requestId
                  . ';ts:' . $timestamp . ';';
        $expected = hash_hmac(
            'sha256',
            $manifest,
            $this->config->string('mercado_pago.webhook_secret')
        );
        return hash_equals($expected, strtolower($signature));
    }

    /**
     * @param array<string, mixed>|null $payload
     * @param list<string> $extraHeaders
     * @return array<string, mixed>
     */
    private function request(
        string $method,
        string $path,
        ?array $payload = null,
        array $extraHeaders = []
    ): array {
        $handle = curl_init(self::API_BASE . $path);
        if ($handle === false) {
            throw new PaymentProviderException(
                'Falha ao iniciar cliente de pagamento.',
                false
            );
        }

        $headers = array_merge([
            'Authorization: Bearer ' . $this->config->string('mercado_pago.access_token'),
            'Accept: application/json',
            'Content-Type: application/json',
            'User-Agent: ChurchDAW-Licensing/1.0',
        ], $extraHeaders);
        $options = [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_CUSTOMREQUEST => $method,
            CURLOPT_HTTPHEADER => $headers,
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
                JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR
            );
        }
        curl_setopt_array($handle, $options);
        $raw = curl_exec($handle);
        $status = (int) curl_getinfo($handle, CURLINFO_RESPONSE_CODE);
        $curlError = curl_error($handle);
        curl_close($handle);

        $isMutation = strtoupper($method) !== 'GET';
        if (!is_string($raw) || $raw === '' || $curlError !== '') {
            throw new PaymentProviderException(
                'Mercado Pago indisponivel: ' . $curlError,
                $isMutation
            );
        }
        if (strlen($raw) > 1024 * 1024) {
            throw new PaymentProviderException(
                'Resposta do provedor excedeu o limite.',
                $isMutation && $status >= 200 && $status < 300
            );
        }

        try {
            $decoded = json_decode($raw, true, 64, JSON_THROW_ON_ERROR);
        } catch (\JsonException $error) {
            throw new PaymentProviderException(
                'Resposta invalida do provedor.',
                $isMutation && $status >= 200 && $status < 300,
                $error
            );
        }
        if (!is_array($decoded)) {
            throw new PaymentProviderException(
                'Resposta invalida do provedor.',
                $isMutation && $status >= 200 && $status < 300
            );
        }
        if ($status < 200 || $status >= 300) {
            $providerCode = (string) ($decoded['error'] ?? $decoded['message'] ?? 'unknown');
            error_log("ChurchDAW Mercado Pago HTTP {$status}: {$providerCode}");
            $ambiguousMutation = $isMutation
                && ($status === 408
                    || $status === 409
                    || $status === 425
                    || $status === 429
                    || $status >= 500);
            throw new PaymentProviderException(
                'O provedor de pagamento recusou a operacao.',
                $ambiguousMutation
            );
        }
        return $decoded;
    }
}
