<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use JsonException;

final class ApiException extends \RuntimeException
{
    public function __construct(
        public readonly int $status,
        public readonly string $apiCode,
        string $message
    ) {
        parent::__construct($message);
    }
}

final class Request
{
    private const MAX_BODY_BYTES = 65536;

    /** @param array<string, string> $headers @param array<string, mixed> $body */
    private function __construct(
        public readonly string $method,
        public readonly string $path,
        public readonly array $headers,
        public readonly array $body,
        public readonly string $rawBody,
        public readonly string $ip,
        public readonly string $requestId
    ) {
    }

    public static function fromGlobals(Config $config): self
    {
        $method = strtoupper($_SERVER['REQUEST_METHOD'] ?? 'GET');
        $uri = (string) ($_SERVER['REQUEST_URI'] ?? '/');
        $path = '/' . ltrim((string) parse_url($uri, PHP_URL_PATH), '/');
        $headers = [];
        foreach (getallheaders() ?: [] as $name => $value) {
            $headers[strtolower((string) $name)] = trim((string) $value);
        }

        $contentType = strtolower(trim(explode(
            ';',
            $headers['content-type'] ?? ''
        )[0]));
        if (str_starts_with($path, '/api/')
            && in_array($method, ['POST', 'PUT', 'PATCH', 'DELETE'], true)
            && $contentType !== 'application/json') {
            throw new ApiException(
                415,
                'JSON_REQUIRED',
                'Use Content-Type application/json.'
            );
        }

        $declaredLength = (int) ($_SERVER['CONTENT_LENGTH'] ?? 0);
        if ($declaredLength > self::MAX_BODY_BYTES) {
            throw new ApiException(413, 'BODY_TOO_LARGE', 'Corpo da requisicao muito grande.');
        }
        $raw = (string) file_get_contents(
            'php://input',
            false,
            null,
            0,
            self::MAX_BODY_BYTES + 1
        );
        if (strlen($raw) > self::MAX_BODY_BYTES) {
            throw new ApiException(413, 'BODY_TOO_LARGE', 'Corpo da requisicao muito grande.');
        }
        $body = [];
        if ($raw !== '' && $contentType === 'application/json') {
            try {
                $decoded = json_decode($raw, true, 64, JSON_THROW_ON_ERROR);
                if (!is_array($decoded)) {
                    throw new JsonException('JSON must be an object.');
                }
                $body = $decoded;
            } catch (JsonException) {
                throw new ApiException(400, 'INVALID_JSON', 'Corpo JSON invalido.');
            }
        } elseif ($raw !== ''
            && $contentType === 'application/x-www-form-urlencoded') {
            parse_str($raw, $decodedForm);
            if (!is_array($decodedForm) || count($decodedForm) > 64) {
                throw new ApiException(
                    400,
                    'INVALID_FORM',
                    'Formulario invalido.'
                );
            }
            foreach ($decodedForm as $key => $value) {
                if (!is_string($key)
                    || !is_string($value)
                    || mb_strlen($key) > 80
                    || mb_strlen($value) > 4096) {
                    throw new ApiException(
                        400,
                        'INVALID_FORM',
                        'Formulario invalido.'
                    );
                }
            }
            $body = $decodedForm;
        } elseif ($raw !== '') {
            throw new ApiException(
                415,
                'CONTENT_TYPE_UNSUPPORTED',
                'Tipo de formulario nao suportado.'
            );
        }

        $ip = (string) ($_SERVER['REMOTE_ADDR'] ?? '0.0.0.0');
        $trustedProxy = $config->string('app.trusted_proxy');
        if ($trustedProxy !== '' && TrustedProxy::contains($trustedProxy, $ip)) {
            $forwarded = trim($headers['cf-connecting-ip'] ?? '');
            if (!filter_var($forwarded, FILTER_VALIDATE_IP)) {
                $forwarded = trim(explode(
                    ',',
                    $headers['x-forwarded-for'] ?? ''
                )[0] ?? '');
            }
            if (filter_var($forwarded, FILTER_VALIDATE_IP)) {
                $ip = $forwarded;
            }
        }

        $requestId = $headers['x-request-id'] ?? Crypto::randomToken(16);
        if (!preg_match('/^[A-Za-z0-9._:-]{8,128}$/', $requestId)) {
            $requestId = Crypto::randomToken(16);
        }

        return new self($method, $path, $headers, $body, $raw, $ip, $requestId);
    }

    /**
     * Creates an in-process request for integration tests and trusted CLI workers.
     *
     * @param array<string, string> $headers
     * @param array<string, mixed> $body
     */
    public static function fromValues(
        string $method,
        string $path,
        array $headers = [],
        array $body = [],
        string $ip = '127.0.0.1',
        ?string $requestId = null
    ): self {
        $normalisedHeaders = [];
        foreach ($headers as $name => $value) {
            $normalisedHeaders[strtolower(trim($name))] = trim($value);
        }
        $rawBody = $body === []
            ? ''
            : json_encode(
                $body,
                JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR
            );
        $resolvedRequestId = $requestId ?? Crypto::randomToken(16);
        if (!preg_match('/^[A-Za-z0-9._:-]{8,128}$/', $resolvedRequestId)) {
            throw new \InvalidArgumentException('Request ID invalido.');
        }

        return new self(
            strtoupper($method),
            '/' . ltrim($path, '/'),
            $normalisedHeaders,
            $body,
            $rawBody,
            $ip,
            $resolvedRequestId
        );
    }

    public function header(string $name): string
    {
        return $this->headers[strtolower($name)] ?? '';
    }

    public function string(string $key, int $maxLength = 512): string
    {
        $value = $this->body[$key] ?? '';
        if (!is_string($value)) {
            throw new ApiException(422, 'INVALID_INPUT', "Campo invalido: {$key}");
        }
        $value = trim($value);
        if (mb_strlen($value) > $maxLength) {
            throw new ApiException(422, 'INVALID_INPUT', "Campo muito longo: {$key}");
        }
        return $value;
    }

    public function bool(string $key): bool
    {
        return filter_var($this->body[$key] ?? false, FILTER_VALIDATE_BOOL);
    }
}

final class Response
{
    /** @param array<string, mixed> $payload */
    public static function json(array $payload, int $status = 200, ?string $requestId = null): never
    {
        self::securityHeaders($requestId);
        http_response_code($status);
        header('Content-Type: application/json; charset=utf-8');
        header('Cache-Control: no-store, max-age=0');
        echo json_encode($payload, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR);
        exit;
    }

    public static function html(string $html, int $status = 200, ?string $requestId = null): never
    {
        self::securityHeaders($requestId);
        http_response_code($status);
        header('Content-Type: text/html; charset=utf-8');
        header('Cache-Control: no-store, max-age=0');
        echo $html;
        exit;
    }

    public static function empty(int $status = 204, ?string $requestId = null): never
    {
        self::securityHeaders($requestId);
        http_response_code($status);
        header('Cache-Control: no-store, max-age=0');
        exit;
    }

    public static function redirect(
        string $location,
        int $status = 303,
        ?string $requestId = null
    ): never {
        if (!str_starts_with($location, '/')
            || str_starts_with($location, '//')
            || str_contains($location, "\r")
            || str_contains($location, "\n")) {
            throw new \InvalidArgumentException('Redirect local invalido.');
        }
        self::securityHeaders($requestId);
        http_response_code($status);
        header('Location: ' . $location);
        header('Cache-Control: no-store, max-age=0');
        exit;
    }

    private static function securityHeaders(?string $requestId): void
    {
        header('X-Content-Type-Options: nosniff');
        header('X-Frame-Options: DENY');
        header('Referrer-Policy: no-referrer');
        header("Permissions-Policy: camera=(), microphone=(), geolocation=(), payment=()");
        header("Content-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; base-uri 'none'; form-action 'self' https://www.mercadopago.com.br https://mercadopago.com.br; frame-ancestors 'none'");
        if ($requestId !== null) {
            header('X-Request-ID: ' . $requestId);
        }
    }
}
