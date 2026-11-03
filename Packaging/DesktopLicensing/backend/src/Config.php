<?php

declare(strict_types=1);

namespace ChurchDAW\Licensing;

use RuntimeException;

final class Config
{
    private const LICENSE_PUBLIC_KEY_SHA256 =
        '648451188c9783354fe4d34f0395fcd14bcce8965ba622b95cbedfeae13dbb98';

    /** @var array<string, mixed> */
    private array $values;

    /** @param array<string, mixed> $values */
    private function __construct(array $values)
    {
        $this->values = $values;
    }

    public static function load(string $projectRoot): self
    {
        if (!date_default_timezone_set('UTC')) {
            throw new RuntimeException('Nao foi possivel fixar o fuso horario UTC.');
        }
        $configFile = getenv('CHURCHDAW_CONFIG_FILE');
        if ($configFile === false || trim($configFile) === '') {
            $configFile = $projectRoot . '/config/local.php';
        }

        $fileValues = [];
        if (is_file($configFile)) {
            $loaded = require $configFile;
            if (!is_array($loaded)) {
                throw new RuntimeException('O arquivo de configuracao deve retornar um array.');
            }
            $fileValues = $loaded;
        }

        $defaults = [
            'app.env' => self::environment('APP_ENV', 'production'),
            'app.release' => self::environment('APP_RELEASE', 'unreleased'),
            'app.base_url' => self::environment('APP_BASE_URL', 'https://churchdaw.com'),
            'app.trusted_proxy' => self::environment('TRUSTED_PROXY', ''),
            'app.encryption_key' => self::environment('APP_ENCRYPTION_KEY', ''),
            'app.ip_hash_key' => self::environment('IP_HASH_KEY', ''),
            'db.dsn' => self::environment('DB_DSN', ''),
            'db.user' => self::environment('DB_USER', ''),
            'db.password' => self::environment('DB_PASSWORD', ''),
            'license.private_key_path' => self::environment('LICENSE_PRIVATE_KEY_PATH', ''),
            'license.key_id' => self::environment('LICENSE_KEY_ID', 'churchdaw-2026-01'),
            'license.lease_hours' => (int) self::environment('LICENSE_LEASE_HOURS', '24'),
            'license.offline_days' => (int) self::environment('LICENSE_OFFLINE_DAYS', '7'),
            'license.refresh_days' => (int) self::environment('LICENSE_REFRESH_DAYS', '30'),
            'license.max_transfers_30d' => (int) self::environment('LICENSE_MAX_TRANSFERS_30D', '2'),
            'trial.minutes' => (int) self::environment('TRIAL_MINUTES', '20'),
            'trial.setup_hours' => (int) self::environment('TRIAL_SETUP_HOURS', '24'),
            'library.storage_path' => self::environment('LIBRARY_STORAGE_PATH', ''),
            'mercado_pago.access_token' => self::environment('MP_ACCESS_TOKEN', ''),
            'mercado_pago.webhook_secret' => self::environment('MP_WEBHOOK_SECRET', ''),
            'mercado_pago.collector_id' => self::environment('MP_COLLECTOR_ID', ''),
            'mercado_pago.application_id' => self::environment('MP_APPLICATION_ID', ''),
            'mercado_pago.sandbox' => self::environment('MP_SANDBOX', '1') === '1',
            'woovi.enabled' => self::environment('WOOVI_ENABLED', '0') === '1',
            'woovi.api_key' => self::environment('WOOVI_API_KEY', ''),
            'woovi.webhook_authorization' =>
                self::environment('WOOVI_WEBHOOK_AUTHORIZATION', ''),
            'woovi.sandbox' => self::environment('WOOVI_SANDBOX', '0') === '1',
            'woovi.charge_expiration_minutes' =>
                (int) self::environment('WOOVI_CHARGE_EXPIRATION_MINUTES', '30'),
            'checkout.provider' => self::environment('CHECKOUT_PROVIDER', 'woovi'),
            'checkout.expiration_minutes' =>
                (int) self::environment('CHECKOUT_EXPIRATION_MINUTES', '4320'),
            'checkout.claim_ttl_seconds' =>
                (int) self::environment('CHECKOUT_CLAIM_TTL_SECONDS', '120'),
            'checkout.enabled' => self::environment('CHECKOUT_ENABLED', '0') === '1',
            'product.code' => self::environment('PRODUCT_CODE', 'churchdaw-perpetual'),
            'product.name' => self::environment('PRODUCT_NAME', 'ChurchDAW Desktop'),
            'product.currency' => self::environment('PRODUCT_CURRENCY', 'BRL'),
            'product.price_cents' => (int) self::environment('PRODUCT_PRICE_CENTS', '0'),
            'plan.single.price_cents' =>
                (int) self::environment('PLAN_SINGLE_PRICE_CENTS', '12999'),
            'plan.complete.price_cents' =>
                (int) self::environment('PLAN_COMPLETE_PRICE_CENTS', '19990'),
            'legal.terms_version' =>
                self::environment('TERMS_VERSION', '2026-07-28'),
            'legal.privacy_version' =>
                self::environment('PRIVACY_VERSION', '2026-07-28'),
            'mail.from' => self::environment('MAIL_FROM', 'contato@churchdaw.com'),
            'mail.reply_to' => self::environment('MAIL_REPLY_TO', 'contato@churchdaw.com'),
            'mail.enabled' => self::environment('MAIL_ENABLED', '1') === '1',
            'support.url' => self::environment('SUPPORT_URL', 'https://churchdaw.com/#duvidas'),
            'admin.enabled' => self::environment('ADMIN_ENABLED', '0') === '1',
            'admin.username' => self::environment('ADMIN_USERNAME', ''),
            'admin.password_hash' => self::environment('ADMIN_PASSWORD_HASH', ''),
        ];

        return new self(array_replace($defaults, $fileValues));
    }

    public function string(string $key): string
    {
        $value = $this->values[$key] ?? '';
        if (!is_scalar($value)) {
            throw new RuntimeException("Configuracao invalida: {$key}");
        }
        return trim((string) $value);
    }

    public function int(string $key): int
    {
        return (int) ($this->values[$key] ?? 0);
    }

    public function bool(string $key): bool
    {
        return filter_var($this->values[$key] ?? false, FILTER_VALIDATE_BOOL);
    }

    public function isProduction(): bool
    {
        return $this->string('app.env') === 'production';
    }

    public function assertRuntimeReady(): void
    {
        if (PHP_VERSION_ID < 80200) {
            throw new RuntimeException('O licenciamento requer PHP 8.2 ou superior.');
        }

        foreach (['pdo_mysql', 'curl', 'openssl', 'sodium', 'mbstring'] as $extension) {
            if (!extension_loaded($extension)) {
                throw new RuntimeException("Extensao PHP obrigatoria ausente: {$extension}");
            }
        }

        $required = [
            'app.base_url',
            'app.encryption_key',
            'app.ip_hash_key',
            'db.dsn',
            'db.user',
            'license.private_key_path',
            'library.storage_path',
            'product.code',
            'product.currency',
        ];
        if ($this->bool('checkout.enabled')) {
            if ($this->string('checkout.provider') === 'woovi') {
                $required[] = 'woovi.api_key';
                if (!$this->bool('woovi.enabled')) {
                    throw new RuntimeException(
                        'WOOVI_ENABLED precisa estar ativo para abrir o checkout Woovi.'
                    );
                }
            } elseif ($this->string('checkout.provider') === 'mercado_pago') {
                $required = array_merge($required, [
                    'mercado_pago.access_token',
                    'mercado_pago.webhook_secret',
                    'mercado_pago.collector_id',
                    'mercado_pago.application_id',
                ]);
            } else {
                throw new RuntimeException('CHECKOUT_PROVIDER invalido.');
            }
        }

        foreach ($required as $key) {
            if ($this->string($key) === '') {
                throw new RuntimeException("Configuracao obrigatoria ausente: {$key}");
            }
        }
        if ($this->bool('checkout.enabled')
            && $this->string('checkout.provider') === 'woovi'
            && strlen($this->string('woovi.webhook_authorization')) < 32) {
            throw new RuntimeException(
                'WOOVI_WEBHOOK_AUTHORIZATION deve ter ao menos 32 caracteres.'
            );
        }

        $encryptionKey = base64_decode($this->string('app.encryption_key'), true);
        if ($encryptionKey === false || strlen($encryptionKey) !== SODIUM_CRYPTO_SECRETBOX_KEYBYTES) {
            throw new RuntimeException('APP_ENCRYPTION_KEY deve conter 32 bytes em Base64.');
        }

        if (strlen($this->string('app.ip_hash_key')) < 32) {
            throw new RuntimeException('IP_HASH_KEY deve ter no minimo 32 caracteres aleatorios.');
        }
        if ($this->string('app.trusted_proxy') !== '') {
            TrustedProxy::assertSafeConfiguration(
                $this->string('app.trusted_proxy')
            );
        }

        $privateKeyPath = $this->string('license.private_key_path');
        if (!is_readable($privateKeyPath)) {
            throw new RuntimeException('A chave privada de licenca nao esta acessivel.');
        }

        if ($this->int('product.price_cents') <= 0) {
            throw new RuntimeException('PRODUCT_PRICE_CENTS deve ser maior que zero.');
        }
        if ($this->int('plan.single.price_cents') <= 0
            || $this->int('plan.complete.price_cents') <= 0) {
            throw new RuntimeException('Os precos dos planos devem ser maiores que zero.');
        }
        if ($this->int('woovi.charge_expiration_minutes') < 5
            || $this->int('woovi.charge_expiration_minutes') > 1440) {
            throw new RuntimeException(
                'WOOVI_CHARGE_EXPIRATION_MINUTES deve estar entre 5 e 1440.'
            );
        }
        if ($this->int('trial.minutes') < 20
            || $this->int('trial.minutes') > 120) {
            throw new RuntimeException('TRIAL_MINUTES deve estar entre 20 e 120.');
        }
        if ($this->int('trial.setup_hours') < 1
            || $this->int('trial.setup_hours') > 72) {
            throw new RuntimeException('TRIAL_SETUP_HOURS deve estar entre 1 e 72.');
        }
        $libraryPath = $this->string('library.storage_path');
        if (!is_dir($libraryPath) || !is_readable($libraryPath)) {
            throw new RuntimeException('LIBRARY_STORAGE_PATH deve ser um diretorio privado legivel.');
        }
        if ($this->int('checkout.expiration_minutes') < 15
            || $this->int('checkout.expiration_minutes') > 10080) {
            throw new RuntimeException(
                'CHECKOUT_EXPIRATION_MINUTES deve estar entre 15 e 10080.'
            );
        }
        if ($this->int('checkout.claim_ttl_seconds') < 30
            || $this->int('checkout.claim_ttl_seconds') > 600) {
            throw new RuntimeException(
                'CHECKOUT_CLAIM_TTL_SECONDS deve estar entre 30 e 600.'
            );
        }
        foreach (['legal.terms_version', 'legal.privacy_version'] as $legalVersion) {
            if (!preg_match('/^[A-Za-z0-9._-]{1,32}$/', $this->string($legalVersion))) {
                throw new RuntimeException("Versao juridica invalida: {$legalVersion}.");
            }
        }
        if (!preg_match(
            '/^[A-Za-z0-9._-]{1,64}$/',
            $this->string('app.release')
        )) {
            throw new RuntimeException('APP_RELEASE invalida.');
        }
        if ($this->bool('mail.enabled')) {
            foreach (['mail.from', 'mail.reply_to'] as $mailAddress) {
                $address = $this->string($mailAddress);
                if (!filter_var($address, FILTER_VALIDATE_EMAIL)
                    || str_contains($address, "\r")
                    || str_contains($address, "\n")) {
                    throw new RuntimeException(
                        "Endereco de e-mail invalido: {$mailAddress}."
                    );
                }
            }
        }
        if ($this->bool('admin.enabled')) {
            if (!preg_match(
                '/^[A-Za-z0-9._-]{4,64}$/',
                $this->string('admin.username')
            )) {
                throw new RuntimeException('ADMIN_USERNAME invalido.');
            }
            $adminHash = $this->string('admin.password_hash');
            $adminHashInfo = password_get_info($adminHash);
            if (($adminHashInfo['algoName'] ?? 'unknown') !== 'argon2id') {
                throw new RuntimeException(
                    'ADMIN_PASSWORD_HASH deve usar Argon2id.'
                );
            }
        }

        if ($this->isProduction()) {
            if ($this->string('app.release') === 'unreleased') {
                throw new RuntimeException(
                    'APP_RELEASE deve identificar a release de producao.'
                );
            }
            if ($this->string('app.base_url') !== 'https://churchdaw.com') {
                throw new RuntimeException(
                    'APP_BASE_URL de producao deve ser https://churchdaw.com.'
                );
            }
            if (!$this->bool('mail.enabled')) {
                throw new RuntimeException(
                    'MAIL_ENABLED deve ser 1 em producao.'
                );
            }
            if ($this->string('db.password') === '') {
                throw new RuntimeException(
                    'DB_PASSWORD e obrigatoria em producao.'
                );
            }
            if ($this->bool('checkout.enabled')) {
                if ($this->string('checkout.provider') === 'woovi') {
                    if ($this->bool('woovi.sandbox')) {
                        throw new RuntimeException(
                            'WOOVI_SANDBOX deve ser 0 em producao.'
                        );
                    }
                } else {
                    if ($this->bool('mercado_pago.sandbox')) {
                        throw new RuntimeException(
                            'MP_SANDBOX deve ser 0 em producao.'
                        );
                    }
                    if (strlen($this->string('mercado_pago.webhook_secret')) < 24) {
                        throw new RuntimeException(
                            'MP_WEBHOOK_SECRET de producao deve ter ao menos 24 caracteres.'
                        );
                    }
                    foreach ([
                        'mercado_pago.collector_id' => 'MP_COLLECTOR_ID',
                        'mercado_pago.application_id' => 'MP_APPLICATION_ID',
                    ] as $key => $environmentName) {
                        if (!preg_match(
                            '/^[0-9]{4,32}$/',
                            $this->string($key)
                        )) {
                            throw new RuntimeException(
                                "{$environmentName} numerico e obrigatorio em producao."
                            );
                        }
                    }
                }
            }
        }

        $privateKeyContents = file_get_contents($privateKeyPath);
        $privateKey = is_string($privateKeyContents)
            ? openssl_pkey_get_private($privateKeyContents)
            : false;
        $details = $privateKey === false
            ? false
            : openssl_pkey_get_details($privateKey);
        if (!is_array($details)
            || ($details['type'] ?? null) !== OPENSSL_KEYTYPE_RSA
            || (int) ($details['bits'] ?? 0) !== 3072
            || !is_string($details['key'] ?? null)) {
            throw new RuntimeException('A chave de licenca deve ser RSA-3072.');
        }

        $publicBody = preg_replace(
            '/-----BEGIN PUBLIC KEY-----|-----END PUBLIC KEY-----|\s+/',
            '',
            (string) $details['key']
        );
        $publicDer = is_string($publicBody)
            ? base64_decode($publicBody, true)
            : false;
        if (!is_string($publicDer)
            || !hash_equals(
                self::LICENSE_PUBLIC_KEY_SHA256,
                hash('sha256', $publicDer)
            )) {
            throw new RuntimeException(
                'A chave privada nao corresponde a chave publica embutida no ChurchDAW.'
            );
        }
    }

    private static function environment(string $name, string $fallback): string
    {
        $value = getenv($name);
        return $value === false ? $fallback : $value;
    }
}
