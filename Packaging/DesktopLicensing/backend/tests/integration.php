<?php

declare(strict_types=1);

use ChurchDAW\Licensing\ApiException;
use ChurchDAW\Licensing\AdminService;
use ChurchDAW\Licensing\AuditLog;
use ChurchDAW\Licensing\AuthService;
use ChurchDAW\Licensing\Config;
use ChurchDAW\Licensing\Crypto;
use ChurchDAW\Licensing\Database;
use ChurchDAW\Licensing\LicenseService;
use ChurchDAW\Licensing\Mailer;
use ChurchDAW\Licensing\MigrationState;
use ChurchDAW\Licensing\PasswordPolicy;
use ChurchDAW\Licensing\PaymentProvider;
use ChurchDAW\Licensing\PaymentProviderException;
use ChurchDAW\Licensing\PaymentService;
use ChurchDAW\Licensing\RateLimiter;
use ChurchDAW\Licensing\Request;
use ChurchDAW\Licensing\TrialService;
use ChurchDAW\Licensing\WebSessionService;
use ChurchDAW\Licensing\WooviPaymentService;
use ChurchDAW\Licensing\WooviProvider;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

function expect(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException('FAIL: ' . $message);
    }
}

function expectApi(callable $operation, string $expectedCode): ApiException
{
    try {
        $operation();
    } catch (ApiException $error) {
        expect(
            $error->apiCode === $expectedCode,
            "esperava {$expectedCode}, recebeu {$error->apiCode}"
        );
        return $error;
    }
    throw new RuntimeException("FAIL: esperava erro {$expectedCode}");
}

/** @return array<string, mixed> */
function compactClaims(string $token): array
{
    $parts = explode('.', $token);
    expect(count($parts) === 3, 'token compacto com tres partes');
    $json = Crypto::base64UrlDecode($parts[1]);
    expect(is_string($json), 'payload Base64URL valido');
    $claims = json_decode($json, true, 64, JSON_THROW_ON_ERROR);
    expect(is_array($claims), 'claims JSON validas');
    return $claims;
}

function loginRequest(
    string $email,
    string $password,
    string $installationId,
    string $deviceSecret,
    string $label,
    string $platform = 'windows'
): Request {
    return Request::fromValues('POST', '/api/v1/auth/login', [], [
        'email' => $email,
        'password' => $password,
        'installation_id' => $installationId,
        'device_secret' => $deviceSecret,
        'device_label' => $label,
        'platform' => $platform,
    ], '127.0.0.11');
}

function refreshRequest(
    string $installationId,
    string $deviceSecret,
    string $refreshToken
): Request {
    $timestamp = time();
    $nonce = Crypto::randomToken(24);
    $message = $installationId . '|' . $timestamp . '|' . $nonce . '|' . $refreshToken;
    return Request::fromValues('POST', '/api/v1/auth/refresh', [], [
        'refresh_token' => $refreshToken,
        'installation_id' => $installationId,
        'nonce' => $nonce,
        'timestamp' => $timestamp,
        'proof' => base64_encode(hash_hmac('sha256', $message, $deviceSecret, true)),
    ], '127.0.0.12');
}

/** @return array{private: OpenSSLAsymmetricKey, public: string, encoded: string} */
function androidKeyPair(): array
{
    $private = openssl_pkey_new([
        'private_key_type' => OPENSSL_KEYTYPE_EC,
        'curve_name' => 'prime256v1',
    ]);
    expect($private instanceof OpenSSLAsymmetricKey, 'chave EC P-256 criada');
    $details = openssl_pkey_get_details($private);
    expect(is_array($details) && is_string($details['key'] ?? null), 'publica EC criada');
    $public = (string) $details['key'];
    $encoded = preg_replace(
        '/-----BEGIN PUBLIC KEY-----|-----END PUBLIC KEY-----|\s+/',
        '',
        $public
    );
    $der = is_string($encoded) ? base64_decode($encoded, true) : false;
    expect(is_string($der), 'DER EC criado');
    return [
        'private' => $private,
        'public' => $public,
        'encoded' => Crypto::base64UrlEncode($der),
    ];
}

function androidLoginRequest(
    string $email,
    string $password,
    string $installationId,
    string $publicKey,
    string $fingerprint,
    string $label,
    string $ip = '127.0.0.31'
): Request {
    return Request::fromValues('POST', '/api/v1/auth/login', [], [
        'email' => $email,
        'password' => $password,
        'installation_id' => $installationId,
        'device_secret' => '',
        'device_public_key' => $publicKey,
        'device_fingerprint' => $fingerprint,
        'device_label' => $label,
        'platform' => 'android',
    ], $ip);
}

function androidProofRequest(
    string $path,
    string $installationId,
    OpenSSLAsymmetricKey $privateKey,
    string $token,
    ?string $nonce = null,
    string $ip = '127.0.0.32'
): Request {
    $timestamp = time();
    $nonce ??= Crypto::randomToken(24);
    $message = $installationId . '|' . $timestamp . '|' . $nonce . '|' . $token;
    $signature = '';
    expect(
        openssl_sign($message, $signature, $privateKey, OPENSSL_ALGO_SHA256),
        'prova ECDSA criada'
    );
    return Request::fromValues('POST', $path, [
        'authorization' => 'Bearer ' . $token,
    ], [
        'refresh_token' => $path === '/api/v1/auth/refresh' ? $token : '',
        'installation_id' => $installationId,
        'nonce' => $nonce,
        'timestamp' => $timestamp,
        'proof' => base64_encode($signature),
    ], $ip);
}

function wooviCheckoutRequest(
    string $email,
    string $password,
    string $idempotencyKey,
    string $planCode = 'complete'
): Request {
    return Request::fromValues('POST', '/api/v1/checkout/start', [
        'idempotency-key' => $idempotencyKey,
    ], [
        'display_name' => 'Cliente Woovi Teste',
        'email' => $email,
        'password' => $password,
        'plan_code' => $planCode,
        'accept_terms' => true,
    ], '127.0.0.61');
}

final class FakePaymentProvider implements PaymentProvider
{
    public int $preferenceCalls = 0;
    public int $preferenceSearchCalls = 0;
    public int $preferenceGetCalls = 0;
    public int $paymentCalls = 0;
    public int $merchantOrderCalls = 0;
    public int $merchantOrderSearchCalls = 0;
    public int $chargebackCalls = 0;
    public bool $failNextPreference = false;
    public bool $timeoutAfterCreatingPreference = false;
    public bool $delayNextPreferenceVisibility = false;
    public ?Closure $onCreatePreference = null;

    /** @var array<string, array<string, mixed>> */
    public array $payments = [];

    /** @var array<string, array<string, mixed>> */
    public array $merchantOrders = [];

    /** @var array<string, bool> */
    public array $hiddenMerchantOrderIdsFromSearch = [];

    /** @var array<string, array<string, mixed>> */
    public array $chargebacks = [];

    /** @var array<string, array<string, mixed>> */
    public array $preferences = [];

    /** @var array<string, array<string, mixed>> */
    public array $delayedPreferences = [];

    public function __construct(private readonly Config $config)
    {
    }

    public function createPreference(array $payload): array
    {
        ++$this->preferenceCalls;
        if ($this->onCreatePreference !== null) {
            $callback = $this->onCreatePreference;
            $this->onCreatePreference = null;
            $callback();
        }
        if ($this->failNextPreference) {
            $this->failNextPreference = false;
            throw new \ChurchDAW\Licensing\PaymentProviderException(
                'provider indisponivel no teste',
                false
            );
        }
        $externalReference = (string) ($payload['external_reference'] ?? '');
        $preference = [
            'id' => 'pref-' . hash('sha256', $externalReference),
            'external_reference' => $externalReference,
            'collector_id' => $this->config->string('mercado_pago.collector_id'),
            'items' => $payload['items'] ?? [],
            'expires' => $payload['expires'] ?? false,
            'preference_expired' => false,
            'expiration_date_from' => $payload['expiration_date_from'] ?? null,
            'expiration_date_to' => $payload['expiration_date_to'] ?? null,
            'init_point' => 'https://www.mercadopago.com.br/checkout/'
                . $externalReference,
            'sandbox_init_point' => 'https://sandbox.mercadopago.com/checkout/'
                . $externalReference,
        ];
        $this->preferences[$externalReference] = $preference;
        if ($this->delayNextPreferenceVisibility) {
            $this->delayNextPreferenceVisibility = false;
            unset($this->preferences[$externalReference]);
            $this->delayedPreferences[$externalReference] = $preference;
            throw new \ChurchDAW\Licensing\PaymentProviderException(
                'timeout ambiguo com consistencia atrasada',
                true
            );
        }
        if ($this->timeoutAfterCreatingPreference) {
            $this->timeoutAfterCreatingPreference = false;
            throw new RuntimeException('timeout ambiguo no teste');
        }
        return $preference;
    }

    public function findPreferenceByExternalReference(
        string $externalReference
    ): ?array {
        ++$this->preferenceSearchCalls;
        if (isset($this->delayedPreferences[$externalReference])) {
            $this->preferences[$externalReference] =
                $this->delayedPreferences[$externalReference];
            unset($this->delayedPreferences[$externalReference]);
            return null;
        }
        return $this->preferences[$externalReference] ?? null;
    }

    public function preference(string $preferenceId): array
    {
        ++$this->preferenceGetCalls;
        foreach ($this->preferences as $preference) {
            if ((string) ($preference['id'] ?? '') === $preferenceId) {
                return $preference;
            }
        }
        throw new RuntimeException('preferencia fake ausente');
    }

    public function payment(string $paymentId): array
    {
        ++$this->paymentCalls;
        if (!isset($this->payments[$paymentId])) {
            throw new RuntimeException('pagamento fake ausente');
        }
        return $this->payments[$paymentId];
    }

    public function merchantOrder(string $merchantOrderId): array
    {
        ++$this->merchantOrderCalls;
        if (!isset($this->merchantOrders[$merchantOrderId])) {
            throw new RuntimeException('pedido comercial fake ausente');
        }
        return $this->merchantOrders[$merchantOrderId];
    }

    public function merchantOrdersByExternalReference(
        string $externalReference
    ): array {
        ++$this->merchantOrderSearchCalls;
        return array_values(array_filter(
            $this->merchantOrders,
            fn (array $merchantOrder): bool =>
                (string) ($merchantOrder['external_reference'] ?? '')
                    === $externalReference
                && !isset($this->hiddenMerchantOrderIdsFromSearch[
                    (string) ($merchantOrder['id'] ?? '')
                ])
        ));
    }

    public function chargeback(string $chargebackId): array
    {
        ++$this->chargebackCalls;
        if (!isset($this->chargebacks[$chargebackId])) {
            throw new RuntimeException('contestacao fake ausente');
        }
        return $this->chargebacks[$chargebackId];
    }

    public function verifyWebhookSignature(
        string $signatureHeader,
        string $requestId,
        string $dataId
    ): bool {
        return $signatureHeader === 'valid-test-signature'
            && $requestId !== ''
            && $dataId !== '';
    }
}

final class FakeWooviProvider implements WooviProvider
{
    public int $createCalls = 0;
    public int $chargeCalls = 0;
    public bool $timeoutAfterCreate = false;
    public string $paymentBaseUrl = 'https://app.woovi.com/pay/';
    public string $imageBaseUrl =
        'https://api.woovi.com/openpix/charge/brcode/image/';

    /** @var array<string, array<string, mixed>> */
    public array $charges = [];

    public function createCharge(array $payload): array
    {
        ++$this->createCalls;
        $orderId = (string) ($payload['correlationID'] ?? '');
        if (isset($this->charges[$orderId])) {
            return ['charge' => $this->charges[$orderId]];
        }
        $charge = [
            'correlationID' => $orderId,
            'value' => (int) ($payload['value'] ?? 0),
            'status' => 'ACTIVE',
            'identifier' => 'woovi-' . str_replace('-', '', $orderId),
            'brCode' => str_repeat('0', 64) . $orderId,
            'paymentLinkUrl' =>
                $this->paymentBaseUrl . rawurlencode($orderId),
            'qrCodeImage' =>
                $this->imageBaseUrl . rawurlencode($orderId) . '.png',
            'expiresDate' => gmdate(DATE_ATOM, time() + 1800),
        ];
        $this->charges[$orderId] = $charge;
        if ($this->timeoutAfterCreate) {
            $this->timeoutAfterCreate = false;
            throw new PaymentProviderException(
                'timeout ambiguo Woovi no teste',
                true
            );
        }
        return ['charge' => $charge];
    }

    public function charge(string $correlationId): array
    {
        ++$this->chargeCalls;
        if (!isset($this->charges[$correlationId])) {
            throw new PaymentProviderException(
                'cobranca Woovi fake ausente',
                false
            );
        }
        return ['charge' => $this->charges[$correlationId]];
    }

    public function verifyWebhookSignature(string $payload, string $signature): bool
    {
        return $payload !== '' && $signature === 'valid-woovi-signature';
    }
}

putenv('ADMIN_ENABLED=1');
putenv('ADMIN_USERNAME=integration-admin');
putenv('ADMIN_PASSWORD_HASH=' . PasswordPolicy::hash(
    'senha administrativa exclusiva de integracao'
));
$config = Config::load($root);
$config->assertRuntimeReady();
$database = new Database($config);
$pdo = $database->pdo();
$databaseName = (string) $pdo->query('SELECT DATABASE()')->fetchColumn();
expect(
    preg_match('/^churchdaw_(?:test|integration)[A-Za-z0-9_]*$/', $databaseName) === 1,
    'o teste so pode usar um banco churchdaw_test* ou churchdaw_integration*'
);
$migrationState = MigrationState::inspect($pdo, $root);
expect(
    $migrationState['ready'],
    'lista completa de migrations nao corresponde ao codigo'
);
$generatedSlots = (int) $pdo->query(
    "SELECT COUNT(*)
     FROM information_schema.columns
     WHERE table_schema = DATABASE()
       AND (
           (table_name = 'orders' AND column_name = 'open_slot')
           OR (table_name = 'licenses' AND column_name = 'active_slot')
           OR (table_name = 'activations' AND column_name = 'active_slot')
           OR (table_name = 'activations' AND column_name = 'active_platform')
       )
       AND UPPER(extra) LIKE '%STORED GENERATED%'"
)->fetchColumn();
expect(
    $generatedSlots === 4,
    'slots comerciais nao sao colunas STORED GENERATED'
);
$slotTriggers = (int) $pdo->query(
    "SELECT COUNT(*)
     FROM information_schema.triggers
     WHERE trigger_schema = DATABASE()
       AND trigger_name LIKE 'churchdaw\\_%\\_slot\\_%'"
)->fetchColumn();
expect(
    $slotTriggers === 0,
    'invariantes ainda dependem de privilegio TRIGGER'
);
$invalidStorage = (int) $pdo->query(
    "SELECT COUNT(*)
     FROM information_schema.tables
     WHERE table_schema = DATABASE()
       AND table_type = 'BASE TABLE'
       AND (
           engine <> 'InnoDB'
           OR row_format <> 'Dynamic'
           OR table_collation <> 'utf8mb4_unicode_ci'
       )"
)->fetchColumn();
expect(
    $invalidStorage === 0,
    'storage nao esta integralmente em InnoDB/DYNAMIC/utf8mb4'
);
$sessionState = $pdo->query(
    'SELECT @@SESSION.time_zone AS time_zone,
            @@SESSION.sql_mode AS sql_mode'
)->fetch();
expect(
    is_array($sessionState)
        && $sessionState['time_zone'] === '+00:00'
        && str_contains((string) $sessionState['sql_mode'], 'STRICT_TRANS_TABLES')
        && str_contains((string) $sessionState['sql_mode'], 'ONLY_FULL_GROUP_BY'),
    'sessao SQL nao esta fixada em UTC/modo estrito'
);
$pdo->exec('DELETE FROM rate_limits');
$pdo->exec('DELETE FROM webhook_events');

$crypto = new Crypto($config);
$limiter = new RateLimiter($database, $crypto);
$mailer = new Mailer($config, $database, $crypto);
$audit = new AuditLog($database, $crypto);
$licenses = new LicenseService($config, $database, $crypto);
$auth = new AuthService($database, $limiter, $licenses, $mailer, $audit);
$trials = new TrialService($config, $database, $crypto, $limiter);
$admin = new AdminService($config, $database, $crypto, $limiter);

expectApi(
    fn () => $auth->resetPassword(Request::fromValues(
        'POST',
        '/api/v1/auth/password/reset',
        [],
        [
            'token' => Crypto::randomToken(32),
            'password' => 'senha nova longa e exclusiva',
        ],
        '127.0.0.10'
    )),
    'INVALID_RESET_TOKEN'
);

$suffix = substr(hash('sha256', Crypto::randomToken(16)), 0, 12);

// O painel deve aplicar exatamente a senha escolhida pelo administrador, sem
// gerar codigo aleatorio, e exigir confirmacao antes de criar a conta.
$adminToken = Crypto::randomToken(32);
$adminCsrf = Crypto::randomToken(24);
$adminIp = '127.0.0.90';
$adminUserAgent = 'ChurchDAW admin integration';
$pdo->prepare(
    "INSERT INTO admin_sessions
        (id, token_hash, csrf_hash, csrf_ciphertext, ip_hash,
         user_agent_hash, expires_at)
     VALUES
        (:id, :token_hash, :csrf_hash, :csrf_ciphertext, :ip_hash,
         :user_agent_hash, DATE_ADD(CURRENT_TIMESTAMP(6), INTERVAL 1 HOUR))"
)->execute([
    'id' => Crypto::uuid(),
    'token_hash' => Crypto::tokenHash($adminToken),
    'csrf_hash' => Crypto::tokenHash($adminCsrf),
    'csrf_ciphertext' => $crypto->encrypt($adminCsrf),
    'ip_hash' => $crypto->hashIp($adminIp),
    'user_agent_hash' => hash('sha256', $adminUserAgent),
]);
$_COOKIE['__Host-churchdaw_admin'] = $adminToken;
$adminEmail = "admin-issued-{$suffix}@example.test";
$adminPassword = '12345678';
$adminRequest = static fn (
    string $chosen,
    string $confirmation,
    string $activationPolicy = 'platform_bundle'
): Request =>
    Request::fromValues(
        'POST',
        '/admin/licenses/issue',
        ['user-agent' => $adminUserAgent],
        [
            'csrf_token' => $adminCsrf,
            'email' => $adminEmail,
            'display_name' => 'Cliente do Painel',
            'customer_password' => $chosen,
            'customer_password_confirmation' => $confirmation,
            'activation_policy' => $activationPolicy,
        ],
        $adminIp
    );
expectApi(
    fn () => $admin->issueLicense($adminRequest(
        $adminPassword,
        'confirmacao diferente 2026'
    )),
    'PASSWORD_MISMATCH'
);
$adminIssued = $admin->issueLicense($adminRequest(
    $adminPassword,
    $adminPassword
));
$adminUserHash = $pdo->prepare(
    'SELECT password_hash FROM users WHERE email = :email LIMIT 1'
);
$adminUserHash->execute(['email' => $adminEmail]);
expect(
    password_verify($adminPassword, (string) $adminUserHash->fetchColumn()),
    'painel nao salvou a senha escolhida pelo administrador'
);
expect(
    ($adminIssued['temporary_code'] ?? null) === null,
    'painel ainda devolveu senha automatica'
);
$adminUpdatedPassword = '87654321';
$adminExisting = $admin->issueLicense($adminRequest(
    $adminUpdatedPassword,
    $adminUpdatedPassword
));
$adminUserHash->execute(['email' => $adminEmail]);
expect(
    ($adminExisting['already_active'] ?? false) === true
        && password_verify(
            $adminUpdatedPassword,
            (string) $adminUserHash->fetchColumn()
        ),
    'painel nao atualizou a senha escolhida para conta existente'
);
$adminUser = $pdo->prepare(
    'SELECT id, status FROM users WHERE email = :email LIMIT 1'
);
$adminUser->execute(['email' => $adminEmail]);
$adminUserRow = $adminUser->fetch();
expect(is_array($adminUserRow), 'cliente administrativo nao foi localizado');
$adminUserId = (string) $adminUserRow['id'];
$adminChosenReplacement = 'abcdefgh';
$admin->setCustomerPassword(
    Request::fromValues(
        'POST',
        "/admin/users/{$adminUserId}/password",
        ['user-agent' => $adminUserAgent],
        [
            'csrf_token' => $adminCsrf,
            'customer_password' => $adminChosenReplacement,
            'customer_password_confirmation' => $adminChosenReplacement,
        ],
        $adminIp
    ),
    $adminUserId
);
$adminUserHash->execute(['email' => $adminEmail]);
expect(
    password_verify(
        $adminChosenReplacement,
        (string) $adminUserHash->fetchColumn()
    ),
    'painel nao aceitou senha de oito caracteres para cliente existente'
);

$adminLicenseId = (string) ($adminIssued['license_id'] ?? '');
$adminLicensePolicy = $pdo->prepare(
    'SELECT activation_policy, max_active_devices
     FROM licenses WHERE id = :id LIMIT 1'
);
$adminLicensePolicy->execute(['id' => $adminLicenseId]);
$adminLicensePolicyRow = $adminLicensePolicy->fetch();
expect(
    is_array($adminLicensePolicyRow)
        && $adminLicensePolicyRow['activation_policy'] === 'platform_bundle'
        && (int) $adminLicensePolicyRow['max_active_devices'] === 3,
    'painel nao criou o pacote de tres plataformas por padrao'
);
$adminPolicyRequest = static fn (string $policy): Request =>
    Request::fromValues(
        'POST',
        "/admin/licenses/{$adminLicenseId}/policy",
        ['user-agent' => $adminUserAgent],
        [
            'csrf_token' => $adminCsrf,
            'activation_policy' => $policy,
        ],
        $adminIp
    );
$admin->updateLicensePolicy(
    $adminPolicyRequest('single_device'),
    $adminLicenseId
);
$adminLicensePolicy->execute(['id' => $adminLicenseId]);
$singlePolicyRow = $adminLicensePolicy->fetch();
expect(
    is_array($singlePolicyRow)
        && $singlePolicyRow['activation_policy'] === 'single_device'
        && (int) $singlePolicyRow['max_active_devices'] === 1,
    'painel nao converteu a licenca para um aparelho'
);
$admin->updateLicensePolicy(
    $adminPolicyRequest('platform_bundle'),
    $adminLicenseId
);

$adminWindowsSecret = Crypto::randomToken(32);
$adminMacSecret = Crypto::randomToken(32);
$auth->login(loginRequest(
    $adminEmail,
    $adminChosenReplacement,
    'admin-windows-' . $suffix,
    $adminWindowsSecret,
    'Windows do pacote',
    'windows'
));
$auth->login(loginRequest(
    $adminEmail,
    $adminChosenReplacement,
    'admin-macos-' . $suffix,
    $adminMacSecret,
    'Mac do pacote',
    'macos'
));
$adminAndroidKey = androidKeyPair();
$auth->login(androidLoginRequest(
    $adminEmail,
    $adminChosenReplacement,
    'admin-android-' . $suffix,
    $adminAndroidKey['encoded'],
    'admin-android-fingerprint-' . $suffix,
    'Android do pacote'
));
$adminActivePlatforms = $pdo->prepare(
    "SELECT GROUP_CONCAT(platform ORDER BY platform SEPARATOR ',')
     FROM activations
     WHERE license_id = :license_id AND status = 'active'"
);
$adminActivePlatforms->execute(['license_id' => $adminLicenseId]);
expect(
    $adminActivePlatforms->fetchColumn() === 'android,macos,windows',
    'pacote nao manteve uma ativacao simultanea por plataforma'
);
expectApi(
    fn () => $auth->login(loginRequest(
        $adminEmail,
        $adminChosenReplacement,
        'admin-windows-extra-' . $suffix,
        Crypto::randomToken(32),
        'Segundo Windows',
        'windows'
    )),
    'ACTIVATION_LIMIT'
);
expectApi(
    fn () => $admin->updateLicensePolicy(
        $adminPolicyRequest('single_device'),
        $adminLicenseId
    ),
    'LICENSE_HAS_MULTIPLE_DEVICES'
);
$adminDashboard = $admin->dashboard(Request::fromValues(
    'GET',
    '/admin',
    ['user-agent' => $adminUserAgent],
    [],
    $adminIp
));
$adminDashboardLicense = null;
foreach (($adminDashboard['licenses'] ?? []) as $dashboardLicense) {
    if (is_array($dashboardLicense)
        && ($dashboardLicense['license_id'] ?? '') === $adminLicenseId) {
        $adminDashboardLicense = $dashboardLicense;
        break;
    }
}
expect(
    is_array($adminDashboardLicense)
        && count($adminDashboardLicense['devices'] ?? []) === 3,
    'painel nao agrupou as tres vagas na mesma licenca'
);
$admin->revokeLicense(
    Request::fromValues(
        'POST',
        "/admin/licenses/{$adminLicenseId}/revoke",
        ['user-agent' => $adminUserAgent],
        ['csrf_token' => $adminCsrf, 'reason' => 'Teste de reativacao'],
        $adminIp
    ),
    $adminLicenseId
);
$admin->reactivateLicense(
    Request::fromValues(
        'POST',
        "/admin/licenses/{$adminLicenseId}/reactivate",
        ['user-agent' => $adminUserAgent],
        ['csrf_token' => $adminCsrf],
        $adminIp
    ),
    $adminLicenseId
);
$adminLicenseStatus = $pdo->prepare(
    'SELECT status FROM licenses WHERE id = :id LIMIT 1'
);
$adminLicenseStatus->execute(['id' => $adminLicenseId]);
expect(
    $adminLicenseStatus->fetchColumn() === 'active',
    'painel nao reativou a licenca revogada'
);

foreach (['blocked', 'active'] as $customerStatus) {
    $admin->setCustomerStatus(
        Request::fromValues(
            'POST',
            "/admin/users/{$adminUserId}/status",
            ['user-agent' => $adminUserAgent],
            ['csrf_token' => $adminCsrf, 'status' => $customerStatus],
            $adminIp
        ),
        $adminUserId
    );
    $adminUser->execute(['email' => $adminEmail]);
    $row = $adminUser->fetch();
    expect(
        is_array($row) && (string) $row['status'] === $customerStatus,
        'painel nao atualizou o status do cliente'
    );
}
$adminDashboardData = $admin->dashboard(Request::fromValues(
    'GET',
    '/admin',
    ['user-agent' => $adminUserAgent],
    [],
    $adminIp
));
expect(
    count($adminDashboardData['events'] ?? []) >= 4,
    'painel nao carregou o historico administrativo'
);
unset($_COOKIE['__Host-churchdaw_admin']);

$email = "desktop-{$suffix}@example.test";
$password = 'frase longa exclusiva 2026';
$userId = Crypto::uuid();
$orderId = Crypto::uuid();
$licenseId = Crypto::uuid();

$database->transaction(function (PDO $pdo) use (
    $config,
    $email,
    $password,
    $userId,
    $orderId,
    $licenseId
): void {
    $pdo->prepare(
        "INSERT INTO users
            (id, email, display_name, password_hash, status, email_verified_at)
         VALUES
            (:id, :email, 'Teste Desktop', :password_hash, 'active', CURRENT_TIMESTAMP(6))"
    )->execute([
        'id' => $userId,
        'email' => $email,
        'password_hash' => PasswordPolicy::hash($password),
    ]);
    $pdo->prepare(
        "INSERT INTO orders
            (id, user_id, product_code, provider, idempotency_key,
             expected_amount_cents, currency, status, terms_version,
             privacy_version, accepted_at, acceptance_ip_hash, paid_at)
         VALUES
            (:id, :user_id, :product_code, 'mercado_pago', :idempotency_key,
             :amount, :currency, 'paid', :terms_version, :privacy_version,
             CURRENT_TIMESTAMP(6), :acceptance_ip_hash, CURRENT_TIMESTAMP(6))"
    )->execute([
        'id' => $orderId,
        'user_id' => $userId,
        'product_code' => $config->string('product.code'),
        'idempotency_key' => 'fixture-' . $orderId,
        'amount' => $config->int('product.price_cents'),
        'currency' => $config->string('product.currency'),
        'terms_version' => $config->string('legal.terms_version'),
        'privacy_version' => $config->string('legal.privacy_version'),
        'acceptance_ip_hash' => hash('sha256', 'integration-fixture'),
    ]);
    $pdo->prepare(
        "INSERT INTO licenses
            (id, user_id, product_code, order_id, status, max_active_devices)
         VALUES
            (:id, :user_id, :product_code, :order_id, 'active', 1)"
    )->execute([
        'id' => $licenseId,
        'user_id' => $userId,
        'product_code' => $config->string('product.code'),
        'order_id' => $orderId,
    ]);
});

$installationA = 'installation-a-' . $suffix;
$installationB = 'installation-b-' . $suffix;
$secretA = Crypto::randomToken(32);
$secretB = Crypto::randomToken(32);
$loginA = $auth->login(loginRequest(
    $email,
    $password,
    $installationA,
    $secretA,
    'PC A / Windows'
));
$claims = compactClaims((string) $loginA['lease_token']);
expect($claims['installation_id'] === $installationA, 'lease vinculada ao PC A');
expect($claims['license_id'] === $licenseId, 'lease vinculada a licenca');
expect($claims['aud'] === 'churchdaw-desktop', 'audience desktop');
expect((int) $claims['offline_until'] > (int) $claims['lease_expires_at'], 'janela offline');

expectApi(
    fn () => $auth->login(loginRequest(
        $email,
        $password,
        $installationB,
        $secretB,
        'PC B / Windows'
    )),
    'ACTIVATION_LIMIT'
);

$refreshOne = (string) $loginA['refresh_token'];
$family = $pdo->prepare(
    'SELECT family_id FROM refresh_tokens WHERE token_hash = :hash LIMIT 1'
);
$family->execute(['hash' => Crypto::tokenHash($refreshOne)]);
$familyId = (string) $family->fetchColumn();
expect($familyId !== '', 'familia de refresh criada');

$refreshed = $auth->refresh(refreshRequest($installationA, $secretA, $refreshOne));
$refreshTwo = (string) $refreshed['refresh_token'];
expect($refreshTwo !== $refreshOne, 'refresh token rotacionado');
expectApi(
    fn () => $auth->refresh(refreshRequest(
        $installationA,
        Crypto::randomToken(32),
        $refreshOne
    )),
    'INVALID_DEVICE_PROOF'
);
$retryRequest = refreshRequest($installationA, $secretA, $refreshOne);
$retryRecovered = $auth->refresh($retryRequest);
expect(
    (string) $retryRecovered['refresh_token'] === $refreshTwo,
    'retry idempotente recuperou o mesmo refresh token'
);
expectApi(
    fn () => $auth->refresh($retryRequest),
    'REPLAY_DETECTED'
);
$familyStatus = $pdo->prepare(
    'SELECT COUNT(*) FROM refresh_tokens
     WHERE family_id = :family_id AND revoked_at IS NULL'
);
$familyStatus->execute(['family_id' => $familyId]);
expect((int) $familyStatus->fetchColumn() > 0, 'retry valido preservou a familia');

$advanced = $auth->refresh(refreshRequest($installationA, $secretA, $refreshTwo));
$refreshThree = (string) $advanced['refresh_token'];
expect($refreshThree !== $refreshTwo, 'replacement ativo continuou a rotacao');
expectApi(
    fn () => $auth->refresh(refreshRequest($installationA, $secretA, $refreshOne)),
    'SESSION_REUSED'
);
$familyStatus->execute(['family_id' => $familyId]);
expect((int) $familyStatus->fetchColumn() === 0, 'reuso apos avancar revogou a familia e persistiu');
expectApi(
    fn () => $auth->refresh(refreshRequest($installationA, $secretA, $refreshThree)),
    'INVALID_SESSION'
);

$licenses->deactivateCurrentDevice(
    $userId,
    Request::fromValues('POST', '/api/v1/account/device/deactivate')
);
$loginB = $auth->login(loginRequest(
    $email,
    $password,
    $installationB,
    $secretB,
    'PC B / Windows'
));
expect(
    compactClaims((string) $loginB['lease_token'])['installation_id'] === $installationB,
    'transferencia liberou o PC B'
);

$licenses->deactivateCurrentDevice(
    $userId,
    Request::fromValues('POST', '/api/v1/account/device/deactivate', [], [], '127.0.0.30')
);
$androidInstallation = 'android-installation-' . $suffix;
$androidFingerprint = 'android-id:' . $suffix . ':tank3';
$androidKeys = androidKeyPair();
$androidLogin = $auth->login(androidLoginRequest(
    $email,
    $password,
    $androidInstallation,
    $androidKeys['encoded'],
    $androidFingerprint,
    'TANK3 / Android'
));
$androidClaims = compactClaims((string) $androidLogin['lease_token']);
expect($androidClaims['aud'] === 'churchdaw-android', 'audience Android');
expect($androidClaims['mode'] === 'licensed', 'modo pago Android');
expect(
    str_contains((string) $androidClaims['scope'],
        'library:download'),
    'lease usada pelo app Android libera biblioteca'
);
expect(
    (int) $androidClaims['exp'] === (int) $androidClaims['offline_until']
        && (int) $androidClaims['exp'] > (int) $androidClaims['lease_expires_at'],
    'lease de biblioteca expira na janela offline'
);
$androidRefresh = (string) $androidLogin['refresh_token'];
$androidRefreshRequest = androidProofRequest(
    '/api/v1/auth/refresh',
    $androidInstallation,
    $androidKeys['private'],
    $androidRefresh
);
$androidRefreshed = $auth->refresh($androidRefreshRequest);
expect(
    (string) $androidRefreshed['refresh_token'] !== $androidRefresh,
    'refresh Android rotacionado'
);
$androidRefreshedClaims = compactClaims(
    (string) $androidRefreshed['lease_token']
);
expect(
    str_contains(
        (string) $androidRefreshedClaims['scope'],
        'library:download'
    )
        && (int) $androidRefreshedClaims['exp']
            === (int) $androidRefreshedClaims['offline_until'],
    'refresh Android renova lease de biblioteca com expiracao'
);

// Reinstalar remove a chave privada do Android Keystore. Mesmo quando o
// installation_id permanece igual, o login autenticado precisa registrar a
// chave publica nova e revogar as sessoes ligadas a chave anterior.
$androidReinstallKeys = androidKeyPair();
$androidReinstallLogin = $auth->login(androidLoginRequest(
    $email,
    $password,
    $androidInstallation,
    $androidReinstallKeys['encoded'],
    $androidFingerprint,
    'TANK3 reinstalado / Android'
));
expectApi(
    fn () => $auth->refresh(androidProofRequest(
        '/api/v1/auth/refresh',
        $androidInstallation,
        $androidKeys['private'],
        (string) $androidRefreshed['refresh_token']
    )),
    'INVALID_SESSION'
);
$androidReinstallRefresh = $auth->refresh(androidProofRequest(
    '/api/v1/auth/refresh',
    $androidInstallation,
    $androidReinstallKeys['private'],
    (string) $androidReinstallLogin['refresh_token']
));
expect(
    (string) $androidReinstallRefresh['refresh_token']
        !== (string) $androidReinstallLogin['refresh_token'],
    'reinstalacao Android com mesmo id nao aceitou a chave nova'
);

// Se o APK vier assinado por outro certificado, Android 8+ pode fornecer um
// ANDROID_ID diferente. O login com e-mail e senha transfere atomicamente a
// unica vaga Android; a instalacao antiga nao continua autorizada.
$androidChangedInstallation = 'android-reinstalled-' . $suffix;
$androidChangedKeys = androidKeyPair();
$androidChangedLogin = $auth->login(androidLoginRequest(
    $email,
    $password,
    $androidChangedInstallation,
    $androidChangedKeys['encoded'],
    'android-id:changed-cert:' . $suffix,
    'TANK3 atualizado / Android'
));
$androidChangedClaims = compactClaims(
    (string) $androidChangedLogin['lease_token']
);
expect(
    $androidChangedClaims['installation_id'] === $androidChangedInstallation,
    'reinstalacao Android com novo id nao recebeu a vaga da licenca'
);
expectApi(
    fn () => $auth->refresh(androidProofRequest(
        '/api/v1/auth/refresh',
        $androidInstallation,
        $androidReinstallKeys['private'],
        (string) $androidReinstallRefresh['refresh_token']
    )),
    'INVALID_SESSION'
);
$activeAndroid = $pdo->prepare(
    "SELECT COUNT(*)
     FROM activations a
     JOIN devices d ON d.id = a.device_id
     WHERE a.license_id = :license_id
       AND a.status = 'active'
       AND d.platform = 'android'"
);
$activeAndroid->execute(['license_id' => $licenseId]);
expect(
    (int) $activeAndroid->fetchColumn() === 1,
    'reinstalacao deixou mais de uma ativacao Android ativa'
);

$trialInstallation = 'trial-installation-' . $suffix;
$trialFingerprint = 'android-id:' . $suffix . ':demo';
$trialKeys = androidKeyPair();
$trialReserveRequest = Request::fromValues(
    'POST',
    '/api/v1/trial/reserve',
    [],
    [
        'installation_id' => $trialInstallation,
        'device_public_key' => $trialKeys['encoded'],
        'device_fingerprint' => $trialFingerprint,
        'device_label' => 'TANK3 Demo / Android',
    ],
    '127.0.0.40'
);
$trialReservation = $trials->reserve($trialReserveRequest);
expect($trialReservation['mode'] === 'trial_setup', 'demo reservada antes do relogio');
expect(
    (int) $trialReservation['trial_seconds'] >= 1200,
    'demo oferece pelo menos 20 minutos'
);
$reservationIdempotencyTokens = [];
for ($attempt = 0; $attempt < 10; ++$attempt) {
    $repeatedReservation = $trials->reserve($trialReserveRequest);
    expect(
        $repeatedReservation['mode'] === 'trial_setup',
        'reserva repetida no mesmo aparelho permanece idempotente'
    );
    $reservationIdempotencyTokens[] =
        (string) $repeatedReservation['setup_token'];
}
expect(
    count(array_unique($reservationIdempotencyTokens)) === 10,
    'reserva idempotente renova tokens sem reutilizar credencial'
);
$setupToken = (string) $trialReservation['setup_token'];
$trialNonce = Crypto::randomToken(24);
$trialStartRequest = androidProofRequest(
    '/api/v1/trial/start',
    $trialInstallation,
    $trialKeys['private'],
    $setupToken,
    $trialNonce,
    '127.0.0.41'
);
$trialActive = $trials->start($trialStartRequest);
$trialClaims = compactClaims((string) $trialActive['lease_token']);
expect($trialClaims['mode'] === 'trial', 'lease de demo marcada');
expect(
    (int) $trialClaims['trial_expires_at'] - (int) $trialClaims['iat'] >= 1200,
    'lease de demo tem pelo menos 20 minutos'
);
expectApi(
    fn () => $trials->start($trialStartRequest),
    'REPLAY_DETECTED'
);
$otherTrialKeys = androidKeyPair();
expectApi(
    fn () => $trials->reserve(Request::fromValues(
        'POST',
        '/api/v1/trial/reserve',
        [],
        [
            'installation_id' => 'other-trial-' . $suffix,
            'device_public_key' => $otherTrialKeys['encoded'],
            'device_fingerprint' => $trialFingerprint,
            'device_label' => 'Outro Android',
        ],
        '127.0.0.42'
    )),
    'TRIAL_ALREADY_USED'
);

$webSessions = new WebSessionService($config, $database, $crypto, $limiter);
$pdo->prepare(
    'UPDATE users SET email_verified_at = NULL WHERE id = :id'
)->execute(['id' => $userId]);
$webLogin = $webSessions->login(Request::fromValues(
    'POST',
    '/api/v1/web/login',
    ['user-agent' => 'ChurchDAW integration test'],
    ['email' => $email, 'password' => $password],
    '127.0.0.13'
));
expect(
    (int) $pdo->query(
        "SELECT email_verified_at IS NOT NULL FROM users
         WHERE id = " . $pdo->quote($userId)
    )->fetchColumn() === 1,
    'login web nao pode bloquear conta sem confirmacao de e-mail'
);
$csrfRow = $pdo->prepare(
    'SELECT csrf_hash, csrf_ciphertext FROM web_sessions
     WHERE user_id = :user_id ORDER BY created_at DESC LIMIT 1'
);
$csrfRow->execute(['user_id' => $userId]);
$csrfStored = $csrfRow->fetch();
expect(is_array($csrfStored), 'sessao web persistida');
$csrfPlaintext = $crypto->decrypt((string) $csrfStored['csrf_ciphertext']);
expect(
    hash_equals((string) $webLogin['csrf_token'], $csrfPlaintext)
        && hash_equals((string) $csrfStored['csrf_hash'], Crypto::tokenHash($csrfPlaintext)),
    'CSRF recuperavel apenas com chave do servidor'
);

$provider = new FakePaymentProvider($config);
$paymentService = new PaymentService(
    $config,
    $database,
    $crypto,
    $limiter,
    $provider,
    $mailer
);
$buyerEmail = "buyer-{$suffix}@example.test";
$checkoutKey = 'checkout-' . $suffix . '-0001';
$checkoutRequest = static function (string $email, string $key): Request {
    $octet = 20 + (hexdec(substr(hash('sha256', $email), 0, 2)) % 200);
    return Request::fromValues(
        'POST',
        '/api/v1/checkout/start',
        ['idempotency-key' => $key],
        [
            'email' => $email,
            'display_name' => 'Comprador Teste',
            'password' => 'outra frase longa exclusiva',
            'accept_terms' => true,
        ],
        '127.0.1.' . $octet
    );
};
$checkout = $paymentService->startCheckout($checkoutRequest($buyerEmail, $checkoutKey));
$checkoutAgain = $paymentService->startCheckout($checkoutRequest($buyerEmail, $checkoutKey));
expect($checkout['order_id'] === $checkoutAgain['order_id'], 'checkout idempotente');
expect($provider->preferenceCalls === 1, 'preferencia nao duplicada');
$termsEvidence = $pdo->prepare(
    'SELECT terms_version, privacy_version, accepted_at, acceptance_ip_hash,
            checkout_claim_token, checkout_claimed_at
     FROM orders WHERE id = :id'
);
$termsEvidence->execute(['id' => $checkout['order_id']]);
$termsRow = $termsEvidence->fetch();
expect(is_array($termsRow), 'evidencia juridica persistida');
expect(
    $termsRow['terms_version'] === $config->string('legal.terms_version'),
    'versao dos termos persistida'
);
expect(
    $termsRow['privacy_version'] === $config->string('legal.privacy_version'),
    'versao da privacidade persistida'
);
expect(
    is_string($termsRow['accepted_at'])
        && strlen((string) $termsRow['acceptance_ip_hash']) === 64,
    'data e IP pseudonimizado do aceite persistidos'
);
expect(
    $termsRow['checkout_claim_token'] === null
        && $termsRow['checkout_claimed_at'] === null,
    'claim liberado depois de persistir a preferencia'
);

$concurrentEmail = "concurrent-{$suffix}@example.test";
$concurrentKey = 'checkout-' . $suffix . '-concurrent-owner';
$secondDatabase = new Database($config);
$secondCrypto = new Crypto($config);
$secondService = new PaymentService(
    $config,
    $secondDatabase,
    $secondCrypto,
    new RateLimiter($secondDatabase, $secondCrypto),
    $provider,
    $mailer
);
$callsBeforeConcurrency = $provider->preferenceCalls;
$provider->onCreatePreference = static function () use (
    $secondService,
    $checkoutRequest,
    $concurrentEmail,
    $suffix
): void {
    expectApi(
        fn () => $secondService->startCheckout(
            $checkoutRequest(
                $concurrentEmail,
                'checkout-' . $suffix . '-concurrent-racer'
            )
        ),
        'CHECKOUT_IN_PROGRESS'
    );
};
$concurrentCheckout = $paymentService->startCheckout(
    $checkoutRequest($concurrentEmail, $concurrentKey)
);
expect(
    $provider->preferenceCalls === $callsBeforeConcurrency + 1,
    'somente o dono do claim chamou o provedor'
);
$concurrentOrders = $pdo->prepare(
    'SELECT COUNT(*) FROM orders o
     JOIN users u ON u.id = o.user_id
     WHERE u.email = :email AND o.product_code = :product_code'
);
$concurrentOrders->execute([
    'email' => $concurrentEmail,
    'product_code' => $config->string('product.code'),
]);
expect((int) $concurrentOrders->fetchColumn() === 1, 'claim evitou pedido concorrente');
$concurrentState = $pdo->prepare(
    'SELECT status, checkout_claim_token, checkout_claimed_at
     FROM orders WHERE id = :id'
);
$concurrentState->execute(['id' => $concurrentCheckout['order_id']]);
$concurrentRow = $concurrentState->fetch();
expect(
    is_array($concurrentRow)
        && $concurrentRow['status'] === 'pending'
        && $concurrentRow['checkout_claim_token'] === null
        && $concurrentRow['checkout_claimed_at'] === null,
    'claim concorrente terminou limpo'
);

$ambiguousEmail = "ambiguous-{$suffix}@example.test";
$provider->timeoutAfterCreatingPreference = true;
$callsBeforeAmbiguous = $provider->preferenceCalls;
$searchesBeforeAmbiguous = $provider->preferenceSearchCalls;
$ambiguous = $paymentService->startCheckout(
    $checkoutRequest(
        $ambiguousEmail,
        'checkout-' . $suffix . '-ambiguous-timeout'
    )
);
expect(
    str_starts_with((string) $ambiguous['checkout_url'], 'https://')
        && $provider->preferenceCalls === $callsBeforeAmbiguous + 1
        && $provider->preferenceSearchCalls === $searchesBeforeAmbiguous + 1,
    'timeout ambiguo recuperou a preferencia sem repetir o POST'
);

$delayedEmail = "delayed-{$suffix}@example.test";
$delayedKey = 'checkout-' . $suffix . '-ambiguous-delayed';
$provider->delayNextPreferenceVisibility = true;
$callsBeforeDelayed = $provider->preferenceCalls;
expectApi(
    fn () => $paymentService->startCheckout(
        $checkoutRequest($delayedEmail, $delayedKey)
    ),
    'CHECKOUT_RECONCILIATION_REQUIRED'
);
$delayedOrderSelect = $pdo->prepare(
    'SELECT id, status FROM orders
     WHERE idempotency_key = :idempotency_key LIMIT 1'
);
$delayedOrderSelect->execute(['idempotency_key' => $delayedKey]);
$delayedOrder = $delayedOrderSelect->fetch();
expect(
    is_array($delayedOrder)
        && $delayedOrder['status'] === 'provider_ambiguous'
        && $provider->preferenceCalls === $callsBeforeDelayed + 1,
    'resultado ambiguo permaneceu bloqueado sem segundo POST'
);
$callsBeforeDelayedRetry = $provider->preferenceCalls;
$delayedRecovered = $paymentService->startCheckout(
    $checkoutRequest($delayedEmail, $delayedKey)
);
expect(
    str_starts_with((string) $delayedRecovered['checkout_url'], 'https://')
        && $provider->preferenceCalls === $callsBeforeDelayedRetry,
    'preferencia com consistencia atrasada foi recuperada apenas por GET'
);

$abandonedEmail = "abandoned-{$suffix}@example.test";
$abandoned = $paymentService->startCheckout(
    $checkoutRequest(
        $abandonedEmail,
        'checkout-' . $suffix . '-abandoned'
    )
);
$provider->preferences[$abandoned['order_id']]['expires'] = true;
$provider->preferences[$abandoned['order_id']]['preference_expired'] = true;
$provider->preferences[$abandoned['order_id']]['expiration_date_to'] =
    gmdate(DATE_ATOM, time() - 3600);
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $abandoned['order_id']
    ),
    'preferencia abandonada foi reconciliada'
);
$abandonedState = $pdo->prepare(
    'SELECT status, open_slot FROM orders WHERE id = :id'
);
$abandonedState->execute(['id' => $abandoned['order_id']]);
$abandonedRow = $abandonedState->fetch();
expect(
    is_array($abandonedRow)
        && $abandonedRow['status'] === 'expired'
        && $abandonedRow['open_slot'] === null,
    'somente preferencia canonicamente vencida liberou nova compra'
);
$afterAbandonment = $paymentService->startCheckout(
    $checkoutRequest(
        $abandonedEmail,
        'checkout-' . $suffix . '-after-abandoned'
    )
);
expect(
    $afterAbandonment['order_id'] !== $abandoned['order_id'],
    'comprador abandonado conseguiu abrir checkout apos reconciliacao'
);
$pdo->prepare(
    'UPDATE orders
     SET checkout_expires_at = DATE_SUB(CURRENT_TIMESTAMP(6), INTERVAL 1 HOUR)
     WHERE id = :id'
)->execute(['id' => $afterAbandonment['order_id']]);
expectApi(
    fn () => $paymentService->startCheckout(
        $checkoutRequest(
            $abandonedEmail,
            'checkout-' . $suffix . '-stale-local-expiry'
        )
    ),
    'CHECKOUT_RECONCILIATION_REQUIRED'
);
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $afterAbandonment['order_id']
    ),
    'preferencia ainda ativa corrigiu o relogio local'
);
$activePreferenceRetry = $paymentService->startCheckout(
    $checkoutRequest(
        $abandonedEmail,
        'checkout-' . $suffix . '-active-provider'
    )
);
expect(
    $activePreferenceRetry['order_id'] === $afterAbandonment['order_id'],
    'preferencia canonica ativa reutilizou a mesma cobranca'
);

$retryEmail = "retry-{$suffix}@example.test";
$retryKey = 'checkout-' . $suffix . '-0002';
$provider->failNextPreference = true;
try {
    $paymentService->startCheckout($checkoutRequest($retryEmail, $retryKey));
    throw new RuntimeException('FAIL: falha simulada do provedor nao propagou');
} catch (RuntimeException $error) {
    expect(
        $error->getMessage() === 'provider indisponivel no teste',
        'falha esperada do provedor'
    );
}
$originalPriceEnvironment = getenv('PRODUCT_PRICE_CENTS');
putenv('PRODUCT_PRICE_CENTS=59900');
try {
    $repricedConfig = Config::load($root);
    $repricedConfig->assertRuntimeReady();
    $repricedPaymentService = new PaymentService(
        $repricedConfig,
        $database,
        $crypto,
        $limiter,
        $provider,
        $mailer
    );
    $retry = $repricedPaymentService->startCheckout(
        $checkoutRequest($retryEmail, $retryKey)
    );
} finally {
    if ($originalPriceEnvironment === false) {
        putenv('PRODUCT_PRICE_CENTS');
    } else {
        putenv('PRODUCT_PRICE_CENTS=' . $originalPriceEnvironment);
    }
}
expect(str_starts_with((string) $retry['checkout_url'], 'https://'), 'checkout recuperado');
$retryStatus = $pdo->prepare(
    'SELECT status, expected_amount_cents, currency
     FROM orders WHERE id = :id'
);
$retryStatus->execute(['id' => $retry['order_id']]);
$retryOrder = $retryStatus->fetch();
expect(
    is_array($retryOrder)
        && $retryOrder['status'] === 'pending'
        && (int) $retryOrder['expected_amount_cents'] === 49900
        && $retryOrder['currency'] === $config->string('product.currency')
        && (float) $provider->preferences[$retry['order_id']]['items'][0]['unit_price']
            === 499.0,
    'retry preservou o preco e a moeda gravados no pedido'
);
expect(
    $retry['email_verification_sent'] === false,
    'checkout nao deve exigir nem reenviar confirmacao de e-mail'
);

$paymentSeed = random_int(810000000000, 819999999990);
$merchantOrderSeed = random_int(910000000000, 919999999990);
$paymentA = (string) $paymentSeed;
$paymentB = (string) ($paymentSeed + 1);
$paymentC = (string) ($paymentSeed + 2);
$merchantOrderId = (string) $merchantOrderSeed;
$provider->payments[$paymentA] = [
    'id' => $paymentA,
    'external_reference' => $checkout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 200,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => ['type' => 'mercadopago', 'id' => $merchantOrderId],
];
$provider->payments[$paymentB] = [
    'id' => $paymentB,
    'external_reference' => $checkout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 299,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => ['type' => 'mercadopago', 'id' => $merchantOrderId],
];
$provider->merchantOrders[$merchantOrderId] = [
    'id' => $merchantOrderId,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $checkout['order_id'],
    'preference_id' => $provider->preferences[$checkout['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [
        [
            'id' => $paymentA,
            'transaction_amount' => 200,
            'total_paid_amount' => 222,
            'currency_id' => $config->string('product.currency'),
            'status' => 'approved',
            'amount_refunded' => 0,
            'date_approved' => gmdate(DATE_ATOM),
        ],
        [
            'id' => $paymentB,
            'transaction_amount' => 299,
            'total_paid_amount' => 333,
            'currency_id' => $config->string('product.currency'),
            'status' => 'approved',
            'amount_refunded' => 0,
            'date_approved' => gmdate(DATE_ATOM),
        ],
    ],
];
$webhookRequest = static fn (
    string $delivery,
    string $resourceId,
    string $type = 'payment'
): Request => Request::fromValues(
    'POST',
    '/api/v1/webhooks/mercado-pago',
    [
        'x-signature' => 'valid-test-signature',
        'x-request-id' => $delivery,
    ],
    [
        'type' => $type,
        'action' => $type === 'payment'
            ? 'payment.updated'
            : 'chargeback.updated',
        'data' => ['id' => $resourceId],
    ],
    '127.0.0.15'
);
$registerCanonicalMerchantOrder = static function (
    array $checkoutResult,
    string $merchantOrderId,
    array $paymentSpecs,
    int|float $paidAmount,
    int|float $refundedAmount = 0,
    int|float $totalAmount = 499,
    string $merchantStatus = 'closed',
    string $merchantOrderStatus = 'paid'
) use ($provider, $config): void {
    $merchantPayments = [];
    foreach ($paymentSpecs as $spec) {
        $paymentId = (string) $spec['id'];
        $amount = $spec['amount'];
        $status = (string) ($spec['status'] ?? 'approved');
        $refund = $spec['refunded'] ?? 0;
        $approvedAt = (string) ($spec['date_approved'] ?? gmdate(DATE_ATOM));
        $provider->payments[$paymentId] = [
            'id' => $paymentId,
            'external_reference' => $checkoutResult['order_id'],
            'status' => $status,
            'currency_id' => $config->string('product.currency'),
            'transaction_amount' => $amount,
            'transaction_amount_refunded' => $refund,
            'collector_id' => $config->string(
                'mercado_pago.collector_id'
            ),
            'date_approved' => $approvedAt,
            'order' => [
                'type' => 'mercadopago',
                'id' => $merchantOrderId,
            ],
        ];
        $merchantPayments[] = [
            'id' => $paymentId,
            'transaction_amount' => $amount,
            'currency_id' => $config->string('product.currency'),
            'status' => $status,
            'amount_refunded' => $refund,
            'date_approved' => $approvedAt,
        ];
    }
    $provider->merchantOrders[$merchantOrderId] = [
        'id' => $merchantOrderId,
        'status' => $merchantStatus,
        'order_status' => $merchantOrderStatus,
        'external_reference' => $checkoutResult['order_id'],
        'preference_id' =>
            $provider->preferences[$checkoutResult['order_id']]['id'],
        'collector' => [
            'id' => $config->string('mercado_pago.collector_id'),
        ],
        'application_id' => $config->string(
            'mercado_pago.application_id'
        ),
        'total_amount' => $totalAmount,
        'paid_amount' => $paidAmount,
        'refunded_amount' => $refundedAmount,
        'payments' => $merchantPayments,
    ];
};
$lateExpiredPayment = (string) ($paymentSeed + 9);
$lateExpiredMerchantOrder = (string) ($merchantOrderSeed + 9);
$provider->payments[$lateExpiredPayment] = [
    'id' => $lateExpiredPayment,
    'external_reference' => $abandoned['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => [
        'type' => 'mercadopago',
        'id' => $lateExpiredMerchantOrder,
    ],
];
$provider->merchantOrders[$lateExpiredMerchantOrder] = [
    'id' => $lateExpiredMerchantOrder,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $abandoned['order_id'],
    'preference_id' => $provider->preferences[$abandoned['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $lateExpiredPayment,
        'transaction_amount' => 499,
        'currency_id' => $config->string('product.currency'),
        'status' => 'approved',
        'amount_refunded' => 0,
        'date_approved' => gmdate(DATE_ATOM),
    ]],
];
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-late-expired-' . $suffix,
        $lateExpiredPayment
    ),
    $lateExpiredPayment
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'worker processou pagamento tardio de preferencia expirada'
);
$lateExpiredState = $pdo->prepare(
    'SELECT o.status,
            (SELECT COUNT(*) FROM licenses l WHERE l.order_id = o.id)
                AS license_count
     FROM orders o WHERE o.id = :id'
);
$lateExpiredState->execute(['id' => $abandoned['order_id']]);
$lateExpiredRow = $lateExpiredState->fetch();
expect(
    is_array($lateExpiredRow)
        && $lateExpiredRow['status'] === 'paid_review'
        && (int) $lateExpiredRow['license_count'] === 0,
    'pedido expirado nunca virou paid nem emitiu licenca automaticamente'
);

$rejectedPayment = (string) ($paymentSeed + 10);
$openedMerchantOrder = (string) ($merchantOrderSeed + 10);
$provider->payments[$rejectedPayment] = [
    'id' => $rejectedPayment,
    'external_reference' => $retry['order_id'],
    'status' => 'rejected',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'order' => ['type' => 'mercadopago', 'id' => $openedMerchantOrder],
];
$provider->merchantOrders[$openedMerchantOrder] = [
    'id' => $openedMerchantOrder,
    'status' => 'opened',
    'external_reference' => $retry['order_id'],
    'preference_id' => $provider->preferences[$retry['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 0,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $rejectedPayment,
        'transaction_amount' => 499,
        'currency_id' => $config->string('product.currency'),
        'status' => 'rejected',
        'amount_refunded' => 0,
    ]],
];
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-opened-no-order-status-' . $suffix,
        $rejectedPayment
    ),
    $rejectedPayment
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'merchant order opened sem order_status foi aceito'
);
$openedOrderStatus = $pdo->prepare(
    'SELECT status, open_slot FROM orders WHERE id = :id'
);
$openedOrderStatus->execute(['id' => $retry['order_id']]);
$openedOrderRow = $openedOrderStatus->fetch();
expect(
    is_array($openedOrderRow)
        && $openedOrderRow['status'] === 'pending'
        && (int) $openedOrderRow['open_slot'] === 1,
    'pedido aberto permaneceu pendente sem liberar compra duplicada'
);
$provider->merchantOrders[$openedMerchantOrder]['status'] = 'expired';
$provider->merchantOrders[$openedMerchantOrder]['order_status'] = '';
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-closed-rejected-' . $suffix,
        $rejectedPayment
    ),
    $rejectedPayment
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'worker processou pagamento rejeitado em merchant order fechado'
);
$openedOrderStatus->execute(['id' => $retry['order_id']]);
$closedRejectedRow = $openedOrderStatus->fetch();
expect(
    is_array($closedRejectedRow)
        && $closedRejectedRow['status'] === 'pending'
        && (int) $closedRejectedRow['open_slot'] === 1,
    'pagamento rejeitado nao liberou preferencia ainda ativa'
);
$preferenceCallsBeforeRejectedRetry = $provider->preferenceCalls;
$rejectedRetry = $paymentService->startCheckout(
    $checkoutRequest(
        $retryEmail,
        'checkout-' . $suffix . '-after-rejected'
    )
);
expect(
    $rejectedRetry['order_id'] === $retry['order_id']
        && $provider->preferenceCalls === $preferenceCallsBeforeRejectedRetry,
    'nova tentativa reutilizou a preferencia rejeitada ainda ativa'
);
$provider->preferences[$retry['order_id']]['preference_expired'] = true;
$provider->preferences[$retry['order_id']]['expiration_date_to'] =
    gmdate(DATE_ATOM, time() - 3600);
expect(
    $paymentService->reconcilePaymentOrder((string) $retry['order_id']),
    'reconciliador conferiu preferencia vencida apos rejeicao'
);
$openedOrderStatus->execute(['id' => $retry['order_id']]);
$expiredRejectedRow = $openedOrderStatus->fetch();
expect(
    is_array($expiredRejectedRow)
        && $expiredRejectedRow['status'] === 'expired'
        && $expiredRejectedRow['open_slot'] === null,
    'somente expiracao canonica liberou checkout rejeitado'
);
$afterRejectedExpiration = $paymentService->startCheckout(
    $checkoutRequest(
        $retryEmail,
        'checkout-' . $suffix . '-after-rejected-expiration'
    )
);
expect(
    $afterRejectedExpiration['order_id'] !== $retry['order_id'],
    'nova preferencia nasceu apenas depois da expiracao canonica'
);

$reconcileCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "reconcile-{$suffix}@example.test",
        'checkout-' . $suffix . '-reconcile'
    )
);
$reconcilePaymentId = (string) ($paymentSeed + 20);
$reconcileMerchantOrderId = (string) ($merchantOrderSeed + 20);
$provider->payments[$reconcilePaymentId] = [
    'id' => $reconcilePaymentId,
    'external_reference' => $reconcileCheckout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => [
        'type' => 'mercadopago',
        'id' => $reconcileMerchantOrderId,
    ],
];
$provider->merchantOrders[$reconcileMerchantOrderId] = [
    'id' => $reconcileMerchantOrderId,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $reconcileCheckout['order_id'],
    'preference_id' =>
        $provider->preferences[$reconcileCheckout['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $reconcilePaymentId,
        'transaction_amount' => 499,
        'currency_id' => $config->string('product.currency'),
        'status' => 'approved',
        'amount_refunded' => 0,
        'date_approved' => gmdate(DATE_ATOM),
    ]],
];
$merchantSearchesBeforeReconcile = $provider->merchantOrderSearchCalls;
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $reconcileCheckout['order_id']
    ),
    'reconciliador encontrou pagamento sem webhook'
);
$reconciledState = $pdo->prepare(
    'SELECT o.status, o.provider_reconciled_at, l.status AS license_status
     FROM orders o
     JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :id'
);
$reconciledState->execute(['id' => $reconcileCheckout['order_id']]);
$reconciledRow = $reconciledState->fetch();
expect(
    is_array($reconciledRow)
        && $reconciledRow['status'] === 'paid'
        && $reconciledRow['license_status'] === 'active'
        && $reconciledRow['provider_reconciled_at'] !== null
        && $provider->merchantOrderSearchCalls
            === $merchantSearchesBeforeReconcile + 1,
    'reconciliacao pos-restore concedeu a licenca canonicamente'
);

$duplicatePaymentId = (string) ($paymentSeed + 21);
$duplicateMerchantOrderId = (string) ($merchantOrderSeed + 21);
$provider->payments[$duplicatePaymentId] = [
    'id' => $duplicatePaymentId,
    'external_reference' => $reconcileCheckout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => [
        'type' => 'mercadopago',
        'id' => $duplicateMerchantOrderId,
    ],
];
$provider->merchantOrders[$duplicateMerchantOrderId] = [
    'id' => $duplicateMerchantOrderId,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $reconcileCheckout['order_id'],
    'preference_id' =>
        $provider->preferences[$reconcileCheckout['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $duplicatePaymentId,
        'transaction_amount' => 499,
        'currency_id' => $config->string('product.currency'),
        'status' => 'approved',
        'amount_refunded' => 0,
        'date_approved' => gmdate(DATE_ATOM),
    ]],
];
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $reconcileCheckout['order_id']
    ),
    'reconciliador inspecionou duplicata mesmo com provider_order_id salvo'
);
$duplicateReview = $pdo->prepare(
    'SELECT status, provider_status FROM orders WHERE id = :id'
);
$duplicateReview->execute(['id' => $reconcileCheckout['order_id']]);
$duplicateReviewRow = $duplicateReview->fetch();
expect(
    is_array($duplicateReviewRow)
        && $duplicateReviewRow['status'] === 'paid_review'
        && str_starts_with(
            (string) $duplicateReviewRow['provider_status'],
            'reconcile/'
        ),
    'segundo merchant order foi bloqueado para revisao manual'
);
$reviewedLicense = $pdo->prepare(
    'SELECT id, status, active_slot FROM licenses
     WHERE order_id = :order_id LIMIT 1'
);
$reviewedLicense->execute(['order_id' => $reconcileCheckout['order_id']]);
$reviewedLicenseBefore = $reviewedLicense->fetch();
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-primary-after-review-' . $suffix,
        $reconcilePaymentId
    ),
    $reconcilePaymentId
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'webhook primario posterior a revisao foi processado'
);
$duplicateReview->execute(['id' => $reconcileCheckout['order_id']]);
$stickyReviewRow = $duplicateReview->fetch();
$reviewedLicense->execute(['order_id' => $reconcileCheckout['order_id']]);
$reviewedLicenseAfter = $reviewedLicense->fetch();
expect(
    is_array($stickyReviewRow)
        && $stickyReviewRow['status'] === 'paid_review'
        && $stickyReviewRow['provider_status']
            === $duplicateReviewRow['provider_status']
        && is_array($reviewedLicenseBefore)
        && is_array($reviewedLicenseAfter)
        && $reviewedLicenseAfter === $reviewedLicenseBefore,
    'paid_review permaneceu sticky sem alterar a licenca'
);

$provider->payments[$duplicatePaymentId]['status'] = 'refunded';
$provider->payments[$duplicatePaymentId]['transaction_amount_refunded'] = 499;
$provider->merchantOrders[$duplicateMerchantOrderId]['refunded_amount'] = 499;
$provider->merchantOrders[$duplicateMerchantOrderId]['payments'][0]['status']
    = 'refunded';
$provider->merchantOrders[$duplicateMerchantOrderId]['payments'][0]['amount_refunded']
    = 499;
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $reconcileCheckout['order_id']
    ),
    'reconciliador agregou refund da cobranca duplicada'
);
$reviewedLicense->execute(['order_id' => $reconcileCheckout['order_id']]);
$singleCoveredReviewLicense = $reviewedLicense->fetch();
expect(
    is_array($singleCoveredReviewLicense)
        && $singleCoveredReviewLicense['status'] === 'active'
        && (int) $singleCoveredReviewLicense['active_slot'] === 1,
    'refund de apenas uma duplicata preservou a licenca coberta pela outra'
);

$provider->payments[$reconcilePaymentId]['status'] = 'refunded';
$provider->payments[$reconcilePaymentId]['transaction_amount_refunded'] = 499;
$provider->merchantOrders[$reconcileMerchantOrderId]['refunded_amount'] = 499;
$provider->merchantOrders[$reconcileMerchantOrderId]['payments'][0]['status']
    = 'refunded';
$provider->merchantOrders[$reconcileMerchantOrderId]['payments'][0]['amount_refunded']
    = 499;
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $reconcileCheckout['order_id']
    ),
    'reconciliador agregou refund das duas cobrancas'
);
$duplicateReview->execute(['id' => $reconcileCheckout['order_id']]);
$fullyRefundedReview = $duplicateReview->fetch();
$reviewedLicense->execute(['order_id' => $reconcileCheckout['order_id']]);
$fullyRefundedReviewLicense = $reviewedLicense->fetch();
expect(
    is_array($fullyRefundedReview)
        && $fullyRefundedReview['status'] === 'paid_review'
        && $fullyRefundedReview['provider_status']
            === $duplicateReviewRow['provider_status']
        && is_array($fullyRefundedReviewLicense)
        && $fullyRefundedReviewLicense['status'] === 'revoked'
        && $fullyRefundedReviewLicense['active_slot'] === null,
    'refund agregado insuficiente revogou sem limpar paid_review'
);

$preLicenseReviewEmail = "prelicense-review-{$suffix}@example.test";
$preLicenseReview = $paymentService->startCheckout(
    $checkoutRequest(
        $preLicenseReviewEmail,
        'checkout-' . $suffix . '-prelicense-review'
    )
);
$preLicensePaymentOne = (string) ($paymentSeed + 30);
$preLicensePaymentTwo = (string) ($paymentSeed + 31);
$preLicenseMerchantOne = (string) ($merchantOrderSeed + 30);
$preLicenseMerchantTwo = (string) ($merchantOrderSeed + 31);
foreach ([
    [$preLicenseMerchantOne, $preLicensePaymentOne],
    [$preLicenseMerchantTwo, $preLicensePaymentTwo],
] as [$reviewMerchantId, $reviewPaymentId]) {
    $provider->payments[$reviewPaymentId] = [
        'id' => $reviewPaymentId,
        'external_reference' => $preLicenseReview['order_id'],
        'status' => 'approved',
        'currency_id' => $config->string('product.currency'),
        'transaction_amount' => 499,
        'collector_id' => $config->string('mercado_pago.collector_id'),
        'date_approved' => gmdate(DATE_ATOM),
        'order' => [
            'type' => 'mercadopago',
            'id' => $reviewMerchantId,
        ],
    ];
    $provider->merchantOrders[$reviewMerchantId] = [
        'id' => $reviewMerchantId,
        'status' => 'closed',
        'order_status' => 'paid',
        'external_reference' => $preLicenseReview['order_id'],
        'preference_id' =>
            $provider->preferences[$preLicenseReview['order_id']]['id'],
        'collector' => ['id' => $config->string('mercado_pago.collector_id')],
        'application_id' => $config->string('mercado_pago.application_id'),
        'total_amount' => 499,
        'paid_amount' => 499,
        'refunded_amount' => 0,
        'payments' => [[
            'id' => $reviewPaymentId,
            'transaction_amount' => 499,
            'currency_id' => $config->string('product.currency'),
            'status' => 'approved',
            'amount_refunded' => 0,
            'date_approved' => gmdate(DATE_ATOM),
        ]],
    ];
}
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $preLicenseReview['order_id']
    ),
    'duplicata anterior a emissao entrou em revisao'
);
$preLicenseReviewState = $pdo->prepare(
    'SELECT o.status, o.open_slot, l.id AS license_id
     FROM orders o
     LEFT JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :id'
);
$preLicenseReviewState->execute(['id' => $preLicenseReview['order_id']]);
$preLicenseReviewRow = $preLicenseReviewState->fetch();
expect(
    is_array($preLicenseReviewRow)
        && $preLicenseReviewRow['status'] === 'paid_review'
        && (int) $preLicenseReviewRow['open_slot'] === 1
        && $preLicenseReviewRow['license_id'] === null,
    'review sem entitlement preservou o bloqueio de cobranca'
);
$preferenceCallsBeforeReviewRetry = $provider->preferenceCalls;
expectApi(
    fn () => $paymentService->startCheckout(
        $checkoutRequest(
            $preLicenseReviewEmail,
            'checkout-' . $suffix . '-prelicense-review-retry'
        )
    ),
    'PAYMENT_REVIEW_REQUIRED'
);
expect(
    $provider->preferenceCalls === $preferenceCallsBeforeReviewRetry,
    'billing hold impediu uma terceira preferencia'
);
$pdo->exec(
    'UPDATE orders
     SET provider_reconcile_attempted_at =
         DATE_ADD(UTC_TIMESTAMP(6), INTERVAL 1 DAY)'
);
$pdo->prepare(
    'UPDATE orders
     SET created_at = DATE_SUB(UTC_TIMESTAMP(6), INTERVAL 91 DAY),
         provider_reconcile_attempted_at = NULL
     WHERE id = :id'
)->execute(['id' => $preLicenseReview['order_id']]);
$provider->hiddenMerchantOrderIdsFromSearch[$preLicenseMerchantOne] = true;
$provider->hiddenMerchantOrderIdsFromSearch[$preLicenseMerchantTwo] = true;
$paymentCallsBeforeOldPreLicenseReview = $provider->paymentCalls;
$merchantCallsBeforeOldPreLicenseReview = $provider->merchantOrderCalls;
$oldPreLicenseStats = $paymentService->reconcilePayments(1);
unset(
    $provider->hiddenMerchantOrderIdsFromSearch[$preLicenseMerchantOne],
    $provider->hiddenMerchantOrderIdsFromSearch[$preLicenseMerchantTwo]
);
$oldPreLicenseState = $pdo->prepare(
    'SELECT o.status, o.provider_order_id, o.provider_reconcile_attempted_at,
            COUNT(p.id) AS payment_count,
            (SELECT COUNT(*) FROM licenses l WHERE l.order_id = o.id)
                AS license_count
     FROM orders o
     LEFT JOIN payments p ON p.order_id = o.id
     WHERE o.id = :id
     GROUP BY o.id, o.status, o.provider_order_id,
              o.provider_reconcile_attempted_at'
);
$oldPreLicenseState->execute(['id' => $preLicenseReview['order_id']]);
$oldPreLicenseRow = $oldPreLicenseState->fetch();
expect(
    $oldPreLicenseStats === [
        'checked' => 1,
        'reconciled' => 1,
        'failed' => 0,
    ]
        && is_array($oldPreLicenseRow)
        && $oldPreLicenseRow['status'] === 'paid_review'
        && $oldPreLicenseRow['provider_order_id'] === null
        && $oldPreLicenseRow['provider_reconcile_attempted_at'] !== null
        && (int) $oldPreLicenseRow['payment_count'] === 2
        && (int) $oldPreLicenseRow['license_count'] === 0
        && $provider->paymentCalls
            >= $paymentCallsBeforeOldPreLicenseReview + 2
        && $provider->merchantOrderCalls
            === $merchantCallsBeforeOldPreLicenseReview + 2,
    'cron recuperou review antigo sem provider_order_id pela evidencia local'
);

$mixedSnapshotEmail = "mixed-snapshot-{$suffix}@example.test";
$mixedSnapshotCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        $mixedSnapshotEmail,
        'checkout-' . $suffix . '-mixed-snapshot'
    )
);
$mixedSnapshotPayment = (string) ($paymentSeed + 32);
$mixedSnapshotMerchant = (string) ($merchantOrderSeed + 32);
$provider->payments[$mixedSnapshotPayment] = [
    'id' => $mixedSnapshotPayment,
    'external_reference' => $mixedSnapshotCheckout['order_id'],
    'status' => 'refunded',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'transaction_amount_refunded' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => [
        'type' => 'mercadopago',
        'id' => $mixedSnapshotMerchant,
    ],
];
$provider->merchantOrders[$mixedSnapshotMerchant] = [
    'id' => $mixedSnapshotMerchant,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $mixedSnapshotCheckout['order_id'],
    'preference_id' =>
        $provider->preferences[$mixedSnapshotCheckout['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $mixedSnapshotPayment,
        'transaction_amount' => 499,
        'currency_id' => $config->string('product.currency'),
        'status' => 'approved',
        'amount_refunded' => 0,
        'date_approved' => gmdate(DATE_ATOM),
    ]],
];
$mixedSnapshotDelivery = 'delivery-mixed-snapshot-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest($mixedSnapshotDelivery, $mixedSnapshotPayment),
    $mixedSnapshotPayment
);
expect(
    $paymentService->processPendingWebhooks(1) === 0,
    'snapshot misto foi recusado para retry'
);
$mixedSnapshotState = $pdo->prepare(
    'SELECT w.id AS event_id, w.status AS event_status, o.status AS order_status,
            o.open_slot,
            (SELECT COUNT(*) FROM licenses l WHERE l.order_id = o.id)
                AS license_count
     FROM webhook_events w
     JOIN orders o ON o.id = :order_id
     WHERE w.delivery_id = :delivery_id'
);
$mixedSnapshotState->execute([
    'order_id' => $mixedSnapshotCheckout['order_id'],
    'delivery_id' => $mixedSnapshotDelivery,
]);
$mixedSnapshotRow = $mixedSnapshotState->fetch();
expect(
    is_array($mixedSnapshotRow)
        && $mixedSnapshotRow['event_status'] === 'failed'
        && $mixedSnapshotRow['order_status'] === 'pending'
        && (int) $mixedSnapshotRow['open_slot'] === 1
        && (int) $mixedSnapshotRow['license_count'] === 0,
    'snapshot divergente nao concedeu entitlement nem liberou nova cobranca'
);
$pdo->prepare('DELETE FROM webhook_events WHERE id = :id')->execute([
    'id' => $mixedSnapshotRow['event_id'],
]);

$duplicateDetailCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "duplicate-detail-{$suffix}@example.test",
        'checkout-' . $suffix . '-duplicate-detail'
    )
);
$duplicateDetailPayment = (string) ($paymentSeed + 40);
$duplicateDetailMerchant = (string) ($merchantOrderSeed + 40);
$duplicateDetailSpec = [
    'id' => $duplicateDetailPayment,
    'amount' => 249.5,
];
$registerCanonicalMerchantOrder(
    $duplicateDetailCheckout,
    $duplicateDetailMerchant,
    [$duplicateDetailSpec, $duplicateDetailSpec],
    499
);
$duplicateDetailDelivery = 'delivery-duplicate-detail-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        $duplicateDetailDelivery,
        $duplicateDetailPayment
    ),
    $duplicateDetailPayment
);
expect(
    $paymentService->processPendingWebhooks(1) === 0,
    'ID de pagamento repetido no mesmo pedido foi recusado'
);
$invalidAggregateState = $pdo->prepare(
    'SELECT w.status AS event_status, o.status AS order_status,
            (SELECT COUNT(*) FROM payments p WHERE p.order_id = o.id)
                AS payment_count,
            (SELECT COUNT(*) FROM licenses l WHERE l.order_id = o.id)
                AS license_count
     FROM webhook_events w
     JOIN orders o ON o.id = :order_id
     WHERE w.delivery_id = :delivery_id'
);
$invalidAggregateState->execute([
    'order_id' => $duplicateDetailCheckout['order_id'],
    'delivery_id' => $duplicateDetailDelivery,
]);
$duplicateDetailRow = $invalidAggregateState->fetch();
expect(
    is_array($duplicateDetailRow)
        && $duplicateDetailRow['event_status'] === 'failed'
        && $duplicateDetailRow['order_status'] === 'pending'
        && (int) $duplicateDetailRow['payment_count'] === 0
        && (int) $duplicateDetailRow['license_count'] === 0,
    'duplicata interna nao persistiu pagamento nem concedeu entitlement'
);
$pdo->prepare(
    'DELETE FROM webhook_events WHERE delivery_id = :delivery_id'
)->execute(['delivery_id' => $duplicateDetailDelivery]);

$grossMismatchCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "gross-mismatch-{$suffix}@example.test",
        'checkout-' . $suffix . '-gross-mismatch'
    )
);
$grossMismatchPaymentOne = (string) ($paymentSeed + 41);
$grossMismatchPaymentTwo = (string) ($paymentSeed + 42);
$grossMismatchMerchant = (string) ($merchantOrderSeed + 41);
$registerCanonicalMerchantOrder(
    $grossMismatchCheckout,
    $grossMismatchMerchant,
    [
        ['id' => $grossMismatchPaymentOne, 'amount' => 499],
        ['id' => $grossMismatchPaymentTwo, 'amount' => 499],
    ],
    499
);
$grossMismatchDelivery = 'delivery-gross-mismatch-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        $grossMismatchDelivery,
        $grossMismatchPaymentOne
    ),
    $grossMismatchPaymentOne
);
expect(
    $paymentService->processPendingWebhooks(1) === 0,
    'soma detalhada maior que o agregado foi recusada'
);
$invalidAggregateState->execute([
    'order_id' => $grossMismatchCheckout['order_id'],
    'delivery_id' => $grossMismatchDelivery,
]);
$grossMismatchRow = $invalidAggregateState->fetch();
expect(
    is_array($grossMismatchRow)
        && $grossMismatchRow['event_status'] === 'failed'
        && $grossMismatchRow['order_status'] === 'pending'
        && (int) $grossMismatchRow['payment_count'] === 0
        && (int) $grossMismatchRow['license_count'] === 0,
    'agregado subdeclarado nao persistiu dinheiro nem entitlement'
);
$pdo->prepare(
    'DELETE FROM webhook_events WHERE delivery_id = :delivery_id'
)->execute(['delivery_id' => $grossMismatchDelivery]);

$zeroRefundCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "zero-refund-{$suffix}@example.test",
        'checkout-' . $suffix . '-zero-refund'
    )
);
$zeroRefundPayment = (string) ($paymentSeed + 43);
$zeroRefundMerchant = (string) ($merchantOrderSeed + 43);
$registerCanonicalMerchantOrder(
    $zeroRefundCheckout,
    $zeroRefundMerchant,
    [[
        'id' => $zeroRefundPayment,
        'amount' => 499,
        'status' => 'refunded',
        'refunded' => 0,
    ]],
    499
);
$zeroRefundDelivery = 'delivery-zero-refund-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest($zeroRefundDelivery, $zeroRefundPayment),
    $zeroRefundPayment
);
expect(
    $paymentService->processPendingWebhooks(1) === 0,
    'status refunded sem valor devolvido foi recusado'
);
$invalidAggregateState->execute([
    'order_id' => $zeroRefundCheckout['order_id'],
    'delivery_id' => $zeroRefundDelivery,
]);
$zeroRefundRow = $invalidAggregateState->fetch();
expect(
    is_array($zeroRefundRow)
        && $zeroRefundRow['event_status'] === 'failed'
        && $zeroRefundRow['order_status'] === 'pending'
        && (int) $zeroRefundRow['payment_count'] === 0
        && (int) $zeroRefundRow['license_count'] === 0,
    'reembolso impossivel nao alterou estado comercial'
);
$pdo->prepare(
    'DELETE FROM webhook_events WHERE delivery_id = :delivery_id'
)->execute(['delivery_id' => $zeroRefundDelivery]);

$expiredNonterminalCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "expired-nonterminal-{$suffix}@example.test",
        'checkout-' . $suffix . '-expired-nonterminal'
    )
);
$expiredNonterminalPayment = (string) ($paymentSeed + 45);
$expiredNonterminalMerchant = (string) ($merchantOrderSeed + 45);
$registerCanonicalMerchantOrder(
    $expiredNonterminalCheckout,
    $expiredNonterminalMerchant,
    [[
        'id' => $expiredNonterminalPayment,
        'amount' => 499,
        'status' => 'pending',
    ]],
    0,
    0,
    499,
    'expired',
    ''
);
$provider->preferences[
    $expiredNonterminalCheckout['order_id']
]['preference_expired'] = true;
$provider->preferences[
    $expiredNonterminalCheckout['order_id']
]['expiration_date_to'] = gmdate(DATE_ATOM, time() - 3600);
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $expiredNonterminalCheckout['order_id']
    ),
    'merchant order expirada com pagamento pendente foi reconciliada'
);
$expiredNonterminalState = $pdo->prepare(
    'SELECT o.status, o.open_slot,
            (SELECT COUNT(*) FROM licenses l WHERE l.order_id = o.id)
                AS license_count
     FROM orders o WHERE o.id = :id'
);
$expiredNonterminalState->execute([
    'id' => $expiredNonterminalCheckout['order_id'],
]);
$expiredPendingRow = $expiredNonterminalState->fetch();
expect(
    is_array($expiredPendingRow)
        && $expiredPendingRow['status'] === 'pending'
        && (int) $expiredPendingRow['open_slot'] === 1
        && (int) $expiredPendingRow['license_count'] === 0,
    'status expired nao liberou checkout com pagamento ainda pendente'
);
$provider->payments[$expiredNonterminalPayment]['status'] = 'approved';
$provider->merchantOrders[
    $expiredNonterminalMerchant
]['payments'][0]['status'] = 'approved';
$provider->merchantOrders[
    $expiredNonterminalMerchant
]['paid_amount'] = 499;
$provider->merchantOrders[
    $expiredNonterminalMerchant
]['order_status'] = 'paid';
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $expiredNonterminalCheckout['order_id']
    ),
    'merchant order expirada com pagamento aprovado foi reconciliada'
);
$expiredNonterminalState->execute([
    'id' => $expiredNonterminalCheckout['order_id'],
]);
$expiredApprovedRow = $expiredNonterminalState->fetch();
expect(
    is_array($expiredApprovedRow)
        && $expiredApprovedRow['status'] === 'pending'
        && (int) $expiredApprovedRow['open_slot'] === 1
        && (int) $expiredApprovedRow['license_count'] === 0,
    'status expired nao liberou nem concedeu entitlement inconclusivo'
);

$overpayCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "overpay-{$suffix}@example.test",
        'checkout-' . $suffix . '-overpay'
    )
);
$overpayPayment = (string) ($paymentSeed + 44);
$overpayMerchant = (string) ($merchantOrderSeed + 44);
$registerCanonicalMerchantOrder(
    $overpayCheckout,
    $overpayMerchant,
    [['id' => $overpayPayment, 'amount' => 699]],
    699
);
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $overpayCheckout['order_id']
    ),
    'sobrepagamento canonico foi reconciliado para revisao'
);
$overpayState = $pdo->prepare(
    'SELECT o.status, o.provider_status,
            COUNT(l.id) AS license_count,
            SUM(l.status = "active" AND l.active_slot = 1)
                AS active_license_count
     FROM orders o
     LEFT JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :id
     GROUP BY o.id, o.status, o.provider_status'
);
$overpayState->execute(['id' => $overpayCheckout['order_id']]);
$overpayRow = $overpayState->fetch();
expect(
    is_array($overpayRow)
        && $overpayRow['status'] === 'paid_review'
        && $overpayRow['provider_status'] === 'review/overpaid'
        && (int) $overpayRow['license_count'] === 1
        && (int) $overpayRow['active_license_count'] === 1,
    'sobrepagamento concedeu no maximo uma licenca e ficou em revisao'
);
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $overpayCheckout['order_id']
    ),
    'sobrepagamento em revisao foi reconciliado novamente'
);
$overpayState->execute(['id' => $overpayCheckout['order_id']]);
$overpayStickyRow = $overpayState->fetch();
expect(
    is_array($overpayStickyRow)
        && $overpayStickyRow['status'] === 'paid_review'
        && $overpayStickyRow['provider_status'] === 'review/overpaid'
        && (int) $overpayStickyRow['license_count'] === 1
        && (int) $overpayStickyRow['active_license_count'] === 1,
    'reconciliacao repetida preservou review e entitlement unico'
);
$pdo->prepare(
    'UPDATE orders
     SET created_at = DATE_SUB(UTC_TIMESTAMP(6), INTERVAL 100 DAY)
     WHERE id = :id'
)->execute(['id' => $overpayCheckout['order_id']]);
$provider->hiddenMerchantOrderIdsFromSearch[$overpayMerchant] = true;
$merchantOrderCallsBeforeOldReview = $provider->merchantOrderCalls;
expect(
    $paymentService->reconcilePaymentOrder(
        (string) $overpayCheckout['order_id']
    ),
    'review antigo foi recuperado pelos IDs de pagamentos locais'
);
unset($provider->hiddenMerchantOrderIdsFromSearch[$overpayMerchant]);
$overpayState->execute(['id' => $overpayCheckout['order_id']]);
$oldReviewRow = $overpayState->fetch();
expect(
    is_array($oldReviewRow)
        && $oldReviewRow['status'] === 'paid_review'
        && $oldReviewRow['provider_status'] === 'review/overpaid'
        && (int) $oldReviewRow['license_count'] === 1
        && (int) $oldReviewRow['active_license_count'] === 1
        && $provider->merchantOrderCalls
            === $merchantOrderCallsBeforeOldReview + 1,
    'review alem da janela de busca permaneceu rastreavel e sticky'
);

$mediationCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "mediation-{$suffix}@example.test",
        'checkout-' . $suffix . '-mediation'
    )
);
$mediationPaymentId = (string) ($paymentSeed + 22);
$mediationMerchantOrderId = (string) ($merchantOrderSeed + 22);
$provider->payments[$mediationPaymentId] = [
    'id' => $mediationPaymentId,
    'external_reference' => $mediationCheckout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => [
        'type' => 'mercadopago',
        'id' => $mediationMerchantOrderId,
    ],
];
$provider->merchantOrders[$mediationMerchantOrderId] = [
    'id' => $mediationMerchantOrderId,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $mediationCheckout['order_id'],
    'preference_id' =>
        $provider->preferences[$mediationCheckout['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $mediationPaymentId,
        'transaction_amount' => 499,
        'currency_id' => $config->string('product.currency'),
        'status' => 'approved',
        'amount_refunded' => 0,
        'date_approved' => gmdate(DATE_ATOM),
    ]],
];
$paymentService->reconcilePaymentOrder(
    (string) $mediationCheckout['order_id']
);
$provider->payments[$mediationPaymentId]['status'] = 'in_mediation';
$provider->merchantOrders[$mediationMerchantOrderId]['payments'][0]['status']
    = 'in_mediation';
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-mediation-' . $suffix,
        $mediationPaymentId
    ),
    $mediationPaymentId
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'worker processou pagamento em mediacao'
);
$mediationState = $pdo->prepare(
    'SELECT o.status AS order_status, l.status AS license_status,
            l.active_slot
     FROM orders o
     JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :id'
);
$mediationState->execute(['id' => $mediationCheckout['order_id']]);
$mediationRow = $mediationState->fetch();
expect(
    is_array($mediationRow)
        && $mediationRow['order_status'] === 'in_mediation'
        && $mediationRow['license_status'] === 'revoked'
        && $mediationRow['active_slot'] === null,
    'mediacao suspendeu o direito em vez de mante-lo ativo'
);
expectApi(
    fn () => $paymentService->startCheckout(
        $checkoutRequest(
            "mediation-{$suffix}@example.test",
            'checkout-' . $suffix . '-mediation-retry'
        )
    ),
    'PAYMENT_REVIEW_REQUIRED'
);
$provider->payments[$mediationPaymentId]['status'] = 'approved';
$provider->merchantOrders[$mediationMerchantOrderId]['payments'][0]['status']
    = 'approved';
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-stale-after-mediation-' . $suffix,
        $mediationPaymentId
    ),
    $mediationPaymentId
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'snapshot aprovado tardio apos mediacao foi processado sem rollback'
);
$mediationAdverseState = $pdo->prepare(
    'SELECT o.status AS order_status, p.status AS payment_status,
            l.status AS license_status, l.active_slot
     FROM orders o
     JOIN payments p
       ON p.order_id = o.id AND p.provider_payment_id = :payment_id
     JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :order_id'
);
$mediationAdverseState->execute([
    'payment_id' => $mediationPaymentId,
    'order_id' => $mediationCheckout['order_id'],
]);
$mediationAdverseRow = $mediationAdverseState->fetch();
expect(
    is_array($mediationAdverseRow)
        && $mediationAdverseRow['order_status'] === 'in_mediation'
        && $mediationAdverseRow['payment_status'] === 'in_mediation'
        && $mediationAdverseRow['license_status'] === 'revoked'
        && $mediationAdverseRow['active_slot'] === null,
    'snapshot aprovado obsoleto nao apagou evidencia de mediacao'
);

$paymentCallsBeforeAck = $provider->paymentCalls;
$merchantCallsBeforeAck = $provider->merchantOrderCalls;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest('delivery-approved-' . $suffix, $paymentB),
    $paymentB
);
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest('delivery-approved-' . $suffix, $paymentB),
    $paymentB
);
expect(
    $provider->paymentCalls === $paymentCallsBeforeAck
        && $provider->merchantOrderCalls === $merchantCallsBeforeAck,
    'ACK do webhook nao chamou o provedor'
);
$queuedEvent = $pdo->prepare(
    'SELECT COUNT(*) AS total, MIN(status) AS status, MIN(attempts) AS attempts
     FROM webhook_events WHERE delivery_id = :delivery_id'
);
$queuedEvent->execute(['delivery_id' => 'delivery-approved-' . $suffix]);
$queuedRow = $queuedEvent->fetch();
expect(
    is_array($queuedRow)
        && (int) $queuedRow['total'] === 1
        && $queuedRow['status'] === 'received'
        && (int) $queuedRow['attempts'] === 0,
    'webhook persistido uma vez antes do ACK'
);
$licenseBeforeWorker = $pdo->prepare(
    'SELECT COUNT(*) FROM licenses WHERE order_id = :order_id'
);
$licenseBeforeWorker->execute(['order_id' => $checkout['order_id']]);
expect((int) $licenseBeforeWorker->fetchColumn() === 0, 'ACK nao concedeu licenca');
expect($paymentService->processPendingWebhooks(1) === 1, 'worker processou webhook');
expect(
    $provider->paymentCalls === $paymentCallsBeforeAck + 2
        && $provider->merchantOrderCalls === $merchantCallsBeforeAck + 1,
    'worker consultou payment e merchant order uma vez'
);
$paidLicense = $pdo->prepare(
    'SELECT l.id, l.user_id, l.status, l.active_slot,
            l.activation_policy, l.max_active_devices
     FROM licenses l WHERE l.order_id = :order_id LIMIT 1'
);
$paidLicense->execute(['order_id' => $checkout['order_id']]);
$paidLicenseRow = $paidLicense->fetch();
expect(
    is_array($paidLicenseRow)
        && $paidLicenseRow['status'] === 'active'
        && (int) $paidLicenseRow['active_slot'] === 1
        && $paidLicenseRow['activation_policy'] === 'platform_bundle'
        && (int) $paidLicenseRow['max_active_devices'] === 3,
    'pagamento nao concedeu o pacote de tres plataformas'
);
$splitRows = $pdo->prepare(
    'SELECT COUNT(*) AS payment_count,
            SUM(amount_cents - refunded_amount_cents) AS net_cents
     FROM payments WHERE order_id = :order_id'
);
$splitRows->execute(['order_id' => $checkout['order_id']]);
$splitRow = $splitRows->fetch();
expect(
    is_array($splitRow)
        && (int) $splitRow['payment_count'] === 2
        && (int) $splitRow['net_cents'] === 49900,
    'pagamentos divididos reconciliados em centavos'
);

$pdo->prepare(
    'UPDATE users SET email_verified_at = NULL WHERE id = :id'
)->execute(['id' => $paidLicenseRow['user_id']]);
$buyerSecret = Crypto::randomToken(32);
$buyerInstallation = 'buyer-installation-' . $suffix;
$buyerLogin = $auth->login(loginRequest(
    $buyerEmail,
    'outra frase longa exclusiva',
    $buyerInstallation,
    $buyerSecret,
    'PC do comprador'
));
expect(
    (int) $pdo->query(
        "SELECT email_verified_at IS NOT NULL FROM users
         WHERE id = " . $pdo->quote((string) $paidLicenseRow['user_id'])
    )->fetchColumn() === 1,
    'login do aplicativo nao pode bloquear cliente pago sem confirmacao'
);

$provider->payments[$paymentA] = [
    'id' => $paymentA,
    'external_reference' => $checkout['order_id'],
    'status' => 'refunded',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 200,
    'transaction_amount_refunded' => 200,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => ['type' => 'mercadopago', 'id' => $merchantOrderId],
];
$provider->merchantOrders[$merchantOrderId]['paid_amount'] = 699;
$provider->merchantOrders[$merchantOrderId]['refunded_amount'] = 200;
$provider->merchantOrders[$merchantOrderId]['payments'][0]['status'] = 'refunded';
$provider->merchantOrders[$merchantOrderId]['payments'][0]['amount_refunded'] = 200;
$provider->merchantOrders[$merchantOrderId]['payments'][] = [
    'id' => $paymentC,
    'transaction_amount' => 200,
    'total_paid_amount' => 240,
    'currency_id' => $config->string('product.currency'),
    'status' => 'approved',
    'amount_refunded' => 0,
    'date_approved' => gmdate(DATE_ATOM),
];
$provider->payments[$paymentC] = [
    'id' => $paymentC,
    'external_reference' => $checkout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 200,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => ['type' => 'mercadopago', 'id' => $merchantOrderId],
];
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest('delivery-refund-covered-' . $suffix, $paymentA),
    $paymentA
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'worker processou reembolso ainda coberto'
);
$stillActive = $pdo->prepare(
    'SELECT l.status, l.active_slot, rt.revoked_at
     FROM licenses l
     JOIN refresh_tokens rt ON rt.license_id = l.id
     WHERE l.id = :license_id LIMIT 1'
);
$stillActive->execute(['license_id' => $paidLicenseRow['id']]);
$stillActiveRow = $stillActive->fetch();
expect(
    is_array($stillActiveRow)
        && $stillActiveRow['status'] === 'active'
        && (int) $stillActiveRow['active_slot'] === 1
        && $stillActiveRow['revoked_at'] === null,
    'reembolso parcial com saldo coberto manteve a licenca'
);

$provider->payments[$paymentC]['status'] = 'refunded';
$provider->payments[$paymentC]['transaction_amount_refunded'] = 200;
$provider->merchantOrders[$merchantOrderId]['refunded_amount'] = 400;
$provider->merchantOrders[$merchantOrderId]['payments'][2]['status'] = 'refunded';
$provider->merchantOrders[$merchantOrderId]['payments'][2]['amount_refunded'] = 200;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest('delivery-refund-underfunded-' . $suffix, $paymentC),
    $paymentC
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'worker processou reembolso abaixo do valor'
);
$revoked = $pdo->prepare(
    'SELECT l.status AS license_status, a.status AS activation_status,
            l.active_slot AS license_active_slot,
            a.active_slot AS activation_active_slot, rt.revoked_at
     FROM licenses l
     JOIN activations a ON a.license_id = l.id
     JOIN refresh_tokens rt ON rt.license_id = l.id
     WHERE l.id = :license_id LIMIT 1'
);
$revoked->execute(['license_id' => $paidLicenseRow['id']]);
$revokedRow = $revoked->fetch();
expect(is_array($revokedRow), 'estado de revogacao encontrado');
expect($revokedRow['license_status'] === 'revoked', 'reembolso revogou licenca');
expect($revokedRow['activation_status'] === 'revoked', 'reembolso revogou ativacao');
expect($revokedRow['license_active_slot'] === null, 'reembolso liberou slot da licenca');
expect(
    $revokedRow['activation_active_slot'] === null,
    'reembolso liberou slot da ativacao'
);
expect($revokedRow['revoked_at'] !== null, 'reembolso revogou refresh');
expectApi(
    fn () => $auth->refresh(refreshRequest(
        $buyerInstallation,
        $buyerSecret,
        (string) $buyerLogin['refresh_token']
    )),
    'INVALID_SESSION'
);
$partialRefundState = $pdo->prepare(
    'SELECT status, open_slot FROM orders WHERE id = :id'
);
$partialRefundState->execute(['id' => $checkout['order_id']]);
$partialRefundRow = $partialRefundState->fetch();
expect(
    is_array($partialRefundRow)
        && $partialRefundRow['status'] === 'partial_refund'
        && $partialRefundRow['open_slot'] === null,
    'saldo parcial virou billing hold explicito'
);
$preferenceCallsBeforePartialRefundRetry = $provider->preferenceCalls;
expectApi(
    fn () => $paymentService->startCheckout(
        $checkoutRequest(
            $buyerEmail,
            'checkout-' . $suffix . '-during-partial-refund'
        )
    ),
    'PAYMENT_REVIEW_REQUIRED'
);
expect(
    $provider->preferenceCalls === $preferenceCallsBeforePartialRefundRetry,
    'saldo parcial impediu uma nova cobranca integral'
);

$provider->payments[$paymentB]['status'] = 'refunded';
$provider->payments[$paymentB]['transaction_amount_refunded'] = 299;
$provider->merchantOrders[$merchantOrderId]['refunded_amount'] = 699;
$provider->merchantOrders[$merchantOrderId]['payments'][1]['status']
    = 'refunded';
$provider->merchantOrders[$merchantOrderId]['payments'][1]['amount_refunded']
    = 299;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest('delivery-refund-full-' . $suffix, $paymentB),
    $paymentB
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'worker processou reembolso integral'
);
$partialRefundState->execute(['id' => $checkout['order_id']]);
$fullRefundRow = $partialRefundState->fetch();
expect(
    is_array($fullRefundRow)
        && $fullRefundRow['status'] === 'refunded'
        && $fullRefundRow['open_slot'] === null,
    'saldo zero encerrou o billing hold e liberou recompra'
);

$repurchaseKey = 'checkout-' . $suffix . '-after-refund';
$repurchase = $paymentService->startCheckout(
    $checkoutRequest($buyerEmail, $repurchaseKey)
);
expect(
    $repurchase['order_id'] !== $checkout['order_id'],
    'reembolso liberou um novo pedido'
);
$oldOpenSlot = $pdo->prepare('SELECT open_slot FROM orders WHERE id = :id');
$oldOpenSlot->execute(['id' => $checkout['order_id']]);
$oldOpenSlotRow = $oldOpenSlot->fetch();
expect(
    is_array($oldOpenSlotRow) && $oldOpenSlotRow['open_slot'] === null,
    'pedido reembolsado liberou open_slot'
);

$paymentD = (string) ($paymentSeed + 3);
$merchantOrderTwo = (string) ($merchantOrderSeed + 1);
$provider->payments[$paymentD] = [
    'id' => $paymentD,
    'external_reference' => $repurchase['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => ['type' => 'mercadopago', 'id' => $merchantOrderTwo],
];
$provider->merchantOrders[$merchantOrderTwo] = [
    'id' => $merchantOrderTwo,
    'status' => 'closed',
    'order_status' => 'paid',
    'external_reference' => $repurchase['order_id'],
    'preference_id' => $provider->preferences[$repurchase['order_id']]['id'],
    'collector' => ['id' => $config->string('mercado_pago.collector_id')],
    'application_id' => $config->string('mercado_pago.application_id'),
    'total_amount' => 499,
    'paid_amount' => 499,
    'refunded_amount' => 0,
    'payments' => [[
        'id' => $paymentD,
        'transaction_amount' => 499,
        'total_paid_amount' => 530,
        'currency_id' => $config->string('product.currency'),
        'status' => 'approved',
        'amount_refunded' => 0,
        'date_approved' => gmdate(DATE_ATOM),
    ]],
];
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest('delivery-repurchase-' . $suffix, $paymentD),
    $paymentD
);
expect($paymentService->processPendingWebhooks(1) === 1, 'recompra processada');
$repurchaseLicense = $pdo->prepare(
    'SELECT id, user_id, status, active_slot
     FROM licenses WHERE order_id = :order_id LIMIT 1'
);
$repurchaseLicense->execute(['order_id' => $repurchase['order_id']]);
$repurchaseLicenseRow = $repurchaseLicense->fetch();
expect(
    is_array($repurchaseLicenseRow)
        && $repurchaseLicenseRow['status'] === 'active'
        && (int) $repurchaseLicenseRow['active_slot'] === 1,
    'recompra concedeu uma nova licenca ativa'
);

$lateRepaymentId = (string) ($paymentSeed + 4);
$provider->payments[$lateRepaymentId] = [
    'id' => $lateRepaymentId,
    'external_reference' => $checkout['order_id'],
    'status' => 'approved',
    'currency_id' => $config->string('product.currency'),
    'transaction_amount' => 499,
    'collector_id' => $config->string('mercado_pago.collector_id'),
    'date_approved' => gmdate(DATE_ATOM),
    'order' => ['type' => 'mercadopago', 'id' => $merchantOrderId],
];
$provider->merchantOrders[$merchantOrderId]['paid_amount'] = 1198;
$provider->merchantOrders[$merchantOrderId]['refunded_amount'] = 699;
$provider->merchantOrders[$merchantOrderId]['payments'][] = [
    'id' => $lateRepaymentId,
    'transaction_amount' => 499,
    'currency_id' => $config->string('product.currency'),
    'status' => 'approved',
    'amount_refunded' => 0,
    'date_approved' => gmdate(DATE_ATOM),
];
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-late-old-order-' . $suffix,
        $lateRepaymentId
    ),
    $lateRepaymentId
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'pagamento tardio foi reconciliado'
);
$oldOrderReview = $pdo->prepare(
    'SELECT o.status AS order_status, l.status AS license_status,
            l.active_slot AS license_active_slot
     FROM orders o
     JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :id'
);
$oldOrderReview->execute(['id' => $checkout['order_id']]);
$oldOrderReviewRow = $oldOrderReview->fetch();
expect(
    is_array($oldOrderReviewRow)
        && $oldOrderReviewRow['order_status'] === 'paid_review'
        && $oldOrderReviewRow['license_status'] === 'revoked'
        && $oldOrderReviewRow['license_active_slot'] === null,
    'pedido antigo repago com reembolso foi para revisao sem reativar licenca'
);
$activeEntitlements = $pdo->prepare(
    "SELECT COUNT(*) FROM licenses
     WHERE user_id = :user_id AND product_code = :product_code
       AND status = 'active' AND active_slot = 1"
);
$activeEntitlements->execute([
    'user_id' => $repurchaseLicenseRow['user_id'],
    'product_code' => $config->string('product.code'),
]);
expect(
    (int) $activeEntitlements->fetchColumn() === 1,
    'usuario manteve exatamente uma licenca ativa'
);

$constraintOrderId = Crypto::uuid();
$duplicateActiveRejected = false;
$pdo->beginTransaction();
try {
    $pdo->prepare(
        "INSERT INTO orders
            (id, user_id, product_code, provider, idempotency_key,
             expected_amount_cents, currency, status, terms_version,
             privacy_version, accepted_at, acceptance_ip_hash)
         VALUES
            (:id, :user_id, :product_code, 'mercado_pago', :idempotency_key,
             :amount, :currency, 'paid', :terms_version, :privacy_version,
             CURRENT_TIMESTAMP(6), :acceptance_ip_hash)"
    )->execute([
        'id' => $constraintOrderId,
        'user_id' => $repurchaseLicenseRow['user_id'],
        'product_code' => $config->string('product.code'),
        'idempotency_key' => 'constraint-' . $constraintOrderId,
        'amount' => $config->int('product.price_cents'),
        'currency' => $config->string('product.currency'),
        'terms_version' => $config->string('legal.terms_version'),
        'privacy_version' => $config->string('legal.privacy_version'),
        'acceptance_ip_hash' => hash('sha256', $constraintOrderId),
    ]);
    $pdo->prepare(
        "INSERT INTO licenses
            (id, user_id, product_code, order_id, status)
         VALUES
            (:id, :user_id, :product_code, :order_id, 'active')"
    )->execute([
        'id' => Crypto::uuid(),
        'user_id' => $repurchaseLicenseRow['user_id'],
        'product_code' => $config->string('product.code'),
        'order_id' => $constraintOrderId,
    ]);
} catch (PDOException $error) {
    $duplicateActiveRejected = (string) $error->getCode() === '23000';
} finally {
    if ($pdo->inTransaction()) {
        $pdo->rollBack();
    }
}
expect($duplicateActiveRejected, 'banco rejeitou segunda licenca ativa');

$nullSlotOrderId = Crypto::uuid();
$activeWithoutSlotRejected = false;
$pdo->beginTransaction();
try {
    $pdo->prepare(
        "INSERT INTO orders
            (id, user_id, product_code, provider, idempotency_key,
             expected_amount_cents, currency, status, terms_version,
             privacy_version, accepted_at, acceptance_ip_hash)
         VALUES
            (:id, :user_id, :product_code, 'mercado_pago', :idempotency_key,
             :amount, :currency, 'paid', :terms_version, :privacy_version,
             CURRENT_TIMESTAMP(6), :acceptance_ip_hash)"
    )->execute([
        'id' => $nullSlotOrderId,
        'user_id' => $repurchaseLicenseRow['user_id'],
        'product_code' => $config->string('product.code'),
        'idempotency_key' => 'constraint-' . $nullSlotOrderId,
        'amount' => $config->int('product.price_cents'),
        'currency' => $config->string('product.currency'),
        'terms_version' => $config->string('legal.terms_version'),
        'privacy_version' => $config->string('legal.privacy_version'),
        'acceptance_ip_hash' => hash('sha256', $nullSlotOrderId),
    ]);
    $pdo->prepare(
        "INSERT INTO licenses
            (id, user_id, product_code, order_id, status)
         VALUES
            (:id, :user_id, :product_code, :order_id, 'active')"
    )->execute([
        'id' => Crypto::uuid(),
        'user_id' => $repurchaseLicenseRow['user_id'],
        'product_code' => $config->string('product.code'),
        'order_id' => $nullSlotOrderId,
    ]);
} catch (PDOException $error) {
    $activeWithoutSlotRejected = (string) $error->getCode() === '23000';
} finally {
    if ($pdo->inTransaction()) {
        $pdo->rollBack();
    }
}
expect(
    $activeWithoutSlotRejected,
    'slot gerado impediu segunda licenca ativa mesmo sem escrita explicita'
);

$chargebackInstallation = 'chargeback-installation-' . $suffix;
$chargebackDeviceSecret = Crypto::randomToken(32);
$chargebackLogin = $auth->login(loginRequest(
    $buyerEmail,
    'outra frase longa exclusiva',
    $chargebackInstallation,
    $chargebackDeviceSecret,
    'PC antes da contestacao'
));
$chargebackId = 'cb-' . substr(hash('sha256', $suffix), 0, 20);
$provider->chargebacks[$chargebackId] = [
    'id' => $chargebackId,
    'payments' => [$paymentD],
    'currency' => $config->string('product.currency'),
    'amount' => 499,
];
$chargebackCallsBeforeAck = $provider->chargebackCalls;
$paymentCallsBeforeChargebackAck = $provider->paymentCalls;
$merchantCallsBeforeChargebackAck = $provider->merchantOrderCalls;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-chargeback-' . $suffix,
        $chargebackId,
        'topic_chargebacks_wh'
    ),
    $chargebackId
);
expect(
    $provider->chargebackCalls === $chargebackCallsBeforeAck
        && $provider->paymentCalls === $paymentCallsBeforeChargebackAck
        && $provider->merchantOrderCalls === $merchantCallsBeforeChargebackAck,
    'ACK do chargeback apenas persistiu o evento'
);
expect(
    $paymentService->processPendingWebhooks(1) === 1
        && $provider->chargebackCalls === $chargebackCallsBeforeAck + 1
        && $provider->paymentCalls === $paymentCallsBeforeChargebackAck + 1
        && $provider->merchantOrderCalls === $merchantCallsBeforeChargebackAck + 1,
    'worker consultou chargeback, payment e merchant order canonicos'
);
$chargebackState = $pdo->prepare(
    'SELECT o.status AS order_status, p.status AS payment_status,
            l.status AS license_status, l.active_slot AS license_active_slot,
            a.status AS activation_status,
            a.active_slot AS activation_active_slot, rt.revoked_at
     FROM orders o
     JOIN payments p
       ON p.order_id = o.id AND p.provider_payment_id = :payment_id
     JOIN licenses l ON l.order_id = o.id
     JOIN activations a ON a.license_id = l.id
     JOIN refresh_tokens rt ON rt.license_id = l.id
     WHERE o.id = :order_id
     ORDER BY rt.created_at DESC LIMIT 1'
);
$chargebackState->execute([
    'payment_id' => $paymentD,
    'order_id' => $repurchase['order_id'],
]);
$chargebackRow = $chargebackState->fetch();
expect(
    is_array($chargebackRow)
        && $chargebackRow['order_status'] === 'charged_back'
        && $chargebackRow['payment_status'] === 'charged_back'
        && $chargebackRow['license_status'] === 'revoked'
        && $chargebackRow['license_active_slot'] === null
        && $chargebackRow['activation_status'] === 'revoked'
        && $chargebackRow['activation_active_slot'] === null
        && $chargebackRow['revoked_at'] !== null,
    'chargeback revogou pedido, pagamento, licenca, ativacao e refresh'
);
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-stale-after-chargeback-' . $suffix,
        $paymentD
    ),
    $paymentD
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'snapshot aprovado tardio apos chargeback foi processado sem rollback'
);
$chargebackState->execute([
    'payment_id' => $paymentD,
    'order_id' => $repurchase['order_id'],
]);
$staleChargebackRow = $chargebackState->fetch();
expect(
    is_array($staleChargebackRow)
        && $staleChargebackRow['order_status'] === 'charged_back'
        && $staleChargebackRow['payment_status'] === 'charged_back'
        && $staleChargebackRow['license_status'] === 'revoked'
        && $staleChargebackRow['license_active_slot'] === null
        && $staleChargebackRow['activation_status'] === 'revoked'
        && $staleChargebackRow['activation_active_slot'] === null
        && $staleChargebackRow['revoked_at'] !== null,
    'snapshot aprovado obsoleto nao apagou evidencia de chargeback'
);
expectApi(
    fn () => $auth->refresh(refreshRequest(
        $chargebackInstallation,
        $chargebackDeviceSecret,
        (string) $chargebackLogin['refresh_token']
    )),
    'INVALID_SESSION'
);
expectApi(
    fn () => $paymentService->startCheckout(
        $checkoutRequest(
            $buyerEmail,
            'checkout-' . $suffix . '-after-chargeback'
        )
    ),
    'PAYMENT_REVIEW_REQUIRED'
);

$splitChargebackCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "split-chargeback-{$suffix}@example.test",
        'checkout-' . $suffix . '-split-chargeback'
    )
);
$splitChargebackPaymentOne = (string) ($paymentSeed + 50);
$splitChargebackPaymentTwo = (string) ($paymentSeed + 51);
$splitChargebackMerchant = (string) ($merchantOrderSeed + 50);
$registerCanonicalMerchantOrder(
    $splitChargebackCheckout,
    $splitChargebackMerchant,
    [
        ['id' => $splitChargebackPaymentOne, 'amount' => 200],
        ['id' => $splitChargebackPaymentTwo, 'amount' => 299],
    ],
    499
);
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-split-chargeback-payment-' . $suffix,
        $splitChargebackPaymentOne
    ),
    $splitChargebackPaymentOne
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'pagamento dividido anterior ao chargeback foi processado'
);
$splitChargebackState = $pdo->prepare(
    "SELECT o.status AS order_status,
            COUNT(DISTINCT p.id) AS payment_count,
            SUM(p.status = 'approved') AS approved_count,
            SUM(p.status = 'charged_back') AS charged_back_count,
            COUNT(DISTINCT l.id) AS license_count,
            MAX(l.status) AS license_status,
            MAX(l.active_slot) AS license_active_slot
     FROM orders o
     LEFT JOIN payments p ON p.order_id = o.id
     LEFT JOIN licenses l ON l.order_id = o.id
     WHERE o.id = :order_id
     GROUP BY o.id, o.status"
);
$splitChargebackState->execute([
    'order_id' => $splitChargebackCheckout['order_id'],
]);
$splitPaidRow = $splitChargebackState->fetch();
expect(
    is_array($splitPaidRow)
        && $splitPaidRow['order_status'] === 'paid'
        && (int) $splitPaidRow['payment_count'] === 2
        && (int) $splitPaidRow['approved_count'] === 2
        && (int) $splitPaidRow['charged_back_count'] === 0
        && (int) $splitPaidRow['license_count'] === 1
        && $splitPaidRow['license_status'] === 'active'
        && (int) $splitPaidRow['license_active_slot'] === 1,
    'pagamento dividido criou um unico entitlement ativo'
);
$splitChargebackId = 'cb-split-ok-' . $suffix;
$provider->chargebacks[$splitChargebackId] = [
    'id' => $splitChargebackId,
    'payments' => [
        $splitChargebackPaymentOne,
        $splitChargebackPaymentTwo,
    ],
    'currency' => $config->string('product.currency'),
    'amount' => 499,
];
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-split-chargeback-' . $suffix,
        $splitChargebackId,
        'topic_chargebacks_wh'
    ),
    $splitChargebackId
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'chargeback agregado de dois pagamentos foi processado'
);
$splitChargebackState->execute([
    'order_id' => $splitChargebackCheckout['order_id'],
]);
$splitChargedBackRow = $splitChargebackState->fetch();
expect(
    is_array($splitChargedBackRow)
        && $splitChargedBackRow['order_status'] === 'charged_back'
        && (int) $splitChargedBackRow['payment_count'] === 2
        && (int) $splitChargedBackRow['approved_count'] === 0
        && (int) $splitChargedBackRow['charged_back_count'] === 2
        && (int) $splitChargedBackRow['license_count'] === 1
        && $splitChargedBackRow['license_status'] === 'revoked'
        && $splitChargedBackRow['license_active_slot'] === null,
    'chargeback agregado revogou atomicamente os dois pagamentos e a licenca'
);

$rollbackChargebackCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "rollback-chargeback-{$suffix}@example.test",
        'checkout-' . $suffix . '-rollback-chargeback'
    )
);
$rollbackChargebackPaymentOne = (string) ($paymentSeed + 52);
$rollbackChargebackPaymentTwo = (string) ($paymentSeed + 53);
$rollbackChargebackMerchant = (string) ($merchantOrderSeed + 52);
$registerCanonicalMerchantOrder(
    $rollbackChargebackCheckout,
    $rollbackChargebackMerchant,
    [
        ['id' => $rollbackChargebackPaymentOne, 'amount' => 200],
        ['id' => $rollbackChargebackPaymentTwo, 'amount' => 299],
    ],
    499
);
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        'delivery-rollback-chargeback-payment-' . $suffix,
        $rollbackChargebackPaymentOne
    ),
    $rollbackChargebackPaymentOne
);
expect(
    $paymentService->processPendingWebhooks(1) === 1,
    'fixture paga para testar rollback de chargeback foi processada'
);
$splitChargebackState->execute([
    'order_id' => $rollbackChargebackCheckout['order_id'],
]);
$rollbackPaidRow = $splitChargebackState->fetch();
expect(
    is_array($rollbackPaidRow)
        && $rollbackPaidRow['order_status'] === 'paid'
        && (int) $rollbackPaidRow['payment_count'] === 2
        && (int) $rollbackPaidRow['approved_count'] === 2
        && (int) $rollbackPaidRow['charged_back_count'] === 0
        && (int) $rollbackPaidRow['license_count'] === 1
        && $rollbackPaidRow['license_status'] === 'active'
        && (int) $rollbackPaidRow['license_active_slot'] === 1,
    'snapshot pago foi capturado antes dos chargebacks invalidos'
);
$oversizedChargebackId = 'cb-split-oversized-' . $suffix;
$provider->chargebacks[$oversizedChargebackId] = [
    'id' => $oversizedChargebackId,
    'payments' => [
        $rollbackChargebackPaymentOne,
        $rollbackChargebackPaymentTwo,
    ],
    'currency' => $config->string('product.currency'),
    'amount' => 500,
];
$oversizedChargebackDelivery =
    'delivery-split-oversized-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        $oversizedChargebackDelivery,
        $oversizedChargebackId,
        'topic_chargebacks_wh'
    ),
    $oversizedChargebackId
);
expect(
    $paymentService->processPendingWebhooks(1) === 0,
    'chargeback acima da soma foi recusado'
);
$splitChargebackState->execute([
    'order_id' => $rollbackChargebackCheckout['order_id'],
]);
$oversizedRollbackRow = $splitChargebackState->fetch();
expect(
    $oversizedRollbackRow === $rollbackPaidRow,
    'chargeback acima da soma nao fez mutacao parcial'
);
$pdo->prepare(
    'DELETE FROM webhook_events WHERE delivery_id = :delivery_id'
)->execute(['delivery_id' => $oversizedChargebackDelivery]);

$originalRollbackCollector = $provider
    ->payments[$rollbackChargebackPaymentTwo]['collector_id'];
$provider->payments[$rollbackChargebackPaymentTwo]['collector_id'] =
    'collector-invalido';
$invalidSecondChargebackId = 'cb-split-invalid-second-' . $suffix;
$provider->chargebacks[$invalidSecondChargebackId] = [
    'id' => $invalidSecondChargebackId,
    'payments' => [
        $rollbackChargebackPaymentOne,
        $rollbackChargebackPaymentTwo,
    ],
    'currency' => $config->string('product.currency'),
    'amount' => 499,
];
$invalidSecondChargebackDelivery =
    'delivery-split-invalid-second-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest(
        $invalidSecondChargebackDelivery,
        $invalidSecondChargebackId,
        'topic_chargebacks_wh'
    ),
    $invalidSecondChargebackId
);
expect(
    $paymentService->processPendingWebhooks(1) === 0,
    'segundo pagamento invalido abortou o chargeback agregado'
);
$provider->payments[$rollbackChargebackPaymentTwo]['collector_id'] =
    $originalRollbackCollector;
$splitChargebackState->execute([
    'order_id' => $rollbackChargebackCheckout['order_id'],
]);
$invalidSecondRollbackRow = $splitChargebackState->fetch();
expect(
    $invalidSecondRollbackRow === $rollbackPaidRow,
    'falha no segundo pagamento preservou o snapshot inteiro'
);
$pdo->prepare(
    'DELETE FROM webhook_events WHERE delivery_id = :delivery_id'
)->execute(['delivery_id' => $invalidSecondChargebackDelivery]);

$missingPayment = (string) ($paymentSeed + 99);
$backoffDelivery = 'delivery-backoff-' . $suffix;
$paymentService->receiveMercadoPagoWebhook(
    $webhookRequest($backoffDelivery, $missingPayment),
    $missingPayment
);
$callsBeforeBackoff = $provider->paymentCalls;
expect(
    $paymentService->processPendingWebhooks(1) === 0
        && $provider->paymentCalls === $callsBeforeBackoff + 1,
    'falha transitoria do webhook foi registrada'
);
$backoffSelect = $pdo->prepare(
    'SELECT id, status, attempts,
            next_attempt_at > CURRENT_TIMESTAMP(6) AS scheduled
     FROM webhook_events WHERE delivery_id = :delivery_id'
);
$backoffSelect->execute(['delivery_id' => $backoffDelivery]);
$backoffRow = $backoffSelect->fetch();
expect(
    is_array($backoffRow)
        && $backoffRow['status'] === 'failed'
        && (int) $backoffRow['attempts'] === 1
        && (int) $backoffRow['scheduled'] === 1,
    'retry recebeu backoff exponencial'
);
$callsBeforeImmediateRetry = $provider->paymentCalls;
expect(
    $paymentService->processPendingWebhooks(1) === 0
        && $provider->paymentCalls === $callsBeforeImmediateRetry,
    'worker nao consumiu todas as tentativas durante indisponibilidade'
);
$pdo->prepare('DELETE FROM webhook_events WHERE id = :id')->execute([
    'id' => $backoffRow['id'],
]);

$batchCheckout = $paymentService->startCheckout(
    $checkoutRequest(
        "batch-reconcile-{$suffix}@example.test",
        'checkout-' . $suffix . '-batch-reconcile'
    )
);
$batchPreference = $provider->preferences[$batchCheckout['order_id']];
unset($provider->preferences[$batchCheckout['order_id']]);
$pdo->exec(
    'UPDATE orders
     SET provider_reconcile_attempted_at = CURRENT_TIMESTAMP(6)'
);
$pdo->prepare(
    'UPDATE orders
     SET provider_reconcile_attempted_at = NULL
     WHERE id = :id'
)->execute(['id' => $batchCheckout['order_id']]);
$failedBatch = $paymentService->reconcilePayments(1);
expect(
    $failedBatch['checked'] === 1
        && $failedBatch['failed'] === 1,
    'falha individual do reconciliador foi contabilizada'
);
$batchFailureState = $pdo->prepare(
    'SELECT provider_reconcile_attempted_at,
            provider_reconcile_failures, provider_reconcile_error
     FROM orders WHERE id = :id'
);
$batchFailureState->execute(['id' => $batchCheckout['order_id']]);
$batchFailureRow = $batchFailureState->fetch();
expect(
    is_array($batchFailureRow)
        && $batchFailureRow['provider_reconcile_attempted_at'] !== null
        && (int) $batchFailureRow['provider_reconcile_failures'] === 1
        && $batchFailureRow['provider_reconcile_error'] !== null,
    'falha registrou cursor e nao pode bloquear pedidos seguintes'
);
$provider->preferences[$batchCheckout['order_id']] = $batchPreference;
$pdo->prepare(
    'UPDATE orders
     SET provider_reconcile_attempted_at = NULL
     WHERE id = :id'
)->execute(['id' => $batchCheckout['order_id']]);
$recoveredBatch = $paymentService->reconcilePayments(1);
$batchFailureState->execute(['id' => $batchCheckout['order_id']]);
$batchRecoveredRow = $batchFailureState->fetch();
expect(
    $recoveredBatch['checked'] === 1
        && $recoveredBatch['failed'] === 0
        && is_array($batchRecoveredRow)
        && (int) $batchRecoveredRow['provider_reconcile_failures'] === 0
        && $batchRecoveredRow['provider_reconcile_error'] === null,
    'reconciliacao posterior limpou o erro sem repetir cobranca'
);

$wooviProvider = new FakeWooviProvider();
$wooviService = new WooviPaymentService(
    $config,
    $database,
    $crypto,
    $limiter,
    $wooviProvider,
    $mailer
);
$wooviPassword = 'Senha Woovi segura #2026';
$wooviBundleEmail = "woovi-bundle-{$suffix}@example.test";
$wooviBundle = $wooviService->startCheckout(wooviCheckoutRequest(
    $wooviBundleEmail,
    $wooviPassword,
    'woovi-bundle-' . $suffix,
    'complete'
));
expect(
    $wooviBundle['plan_code'] === 'complete'
        && $wooviBundle['status'] === 'pending'
        && strlen((string) $wooviBundle['pix_copy_paste']) >= 40,
    'Woovi criou Pix do pacote completo'
);
$wooviOrderState = $pdo->prepare(
    'SELECT plan_code, activation_policy, expected_amount_cents, status
     FROM orders WHERE id = :id'
);
$wooviOrderState->execute(['id' => $wooviBundle['order_id']]);
$wooviBundleOrder = $wooviOrderState->fetch();
expect(
    is_array($wooviBundleOrder)
        && $wooviBundleOrder['plan_code'] === 'complete'
        && $wooviBundleOrder['activation_policy'] === 'platform_bundle'
        && (int) $wooviBundleOrder['expected_amount_cents'] === 19990
        && $wooviBundleOrder['status'] === 'pending',
    'pedido Woovi preservou plano, preco e politica multiplataforma'
);
$wooviProvider->charges[$wooviBundle['order_id']]['status'] = 'COMPLETED';
$wooviProvider->charges[$wooviBundle['order_id']]['transactionID'] =
    'tx-' . str_replace('-', '', (string) $wooviBundle['order_id']);
$wooviProvider->charges[$wooviBundle['order_id']]['paidAt'] = gmdate(DATE_ATOM);
$wooviPaid = $wooviService->status(Request::fromValues(
    'POST',
    '/api/v1/checkout/status',
    [],
    [
        'order_id' => $wooviBundle['order_id'],
        'checkout_token' => $wooviBundle['checkout_token'],
    ],
    '127.0.0.62'
));
expect(
    $wooviPaid['paid'] === true && $wooviPaid['license_issued'] === true,
    'polling Woovi liberou a licenca automaticamente'
);
$wooviLicense = $pdo->prepare(
    'SELECT l.*, u.id AS customer_id, u.email_verified_at
     FROM licenses l JOIN users u ON u.id = l.user_id
     WHERE l.order_id = :order_id'
);
$wooviLicense->execute(['order_id' => $wooviBundle['order_id']]);
$wooviBundleLicense = $wooviLicense->fetch();
expect(
    is_array($wooviBundleLicense)
        && (int) $wooviBundleLicense['max_active_devices'] === 3
        && $wooviBundleLicense['activation_policy'] === 'platform_bundle'
        && $wooviBundleLicense['email_verified_at'] !== null,
    'pagamento completo emitiu tres vagas por plataforma'
);
$auth->login(loginRequest(
    $wooviBundleEmail,
    $wooviPassword,
    'woovi-win-' . $suffix,
    str_repeat('w', 48),
    'PC Woovi',
    'windows'
));
$auth->login(loginRequest(
    $wooviBundleEmail,
    $wooviPassword,
    'woovi-mac-' . $suffix,
    str_repeat('m', 48),
    'Mac Woovi',
    'macos'
));
$wooviAndroidKey = androidKeyPair();
$auth->login(androidLoginRequest(
    $wooviBundleEmail,
    $wooviPassword,
    'woovi-android-' . $suffix,
    $wooviAndroidKey['encoded'],
    hash('sha256', 'woovi-android-' . $suffix),
    'Android Woovi',
    '127.0.0.63'
));
expectApi(
    fn (): array => $auth->login(loginRequest(
        $wooviBundleEmail,
        $wooviPassword,
        'woovi-win-extra-' . $suffix,
        str_repeat('x', 48),
        'Segundo PC',
        'windows'
    )),
    'ACTIVATION_LIMIT'
);
$wooviActivePlatforms = $pdo->prepare(
    "SELECT GROUP_CONCAT(platform ORDER BY platform SEPARATOR ',')
     FROM activations WHERE license_id = :license_id AND status = 'active'"
);
$wooviActivePlatforms->execute(['license_id' => $wooviBundleLicense['id']]);
expect(
    $wooviActivePlatforms->fetchColumn() === 'android,macos,windows',
    'pacote completo manteve exatamente uma vaga por plataforma'
);

$wooviSingleEmail = "woovi-single-{$suffix}@example.test";
$wooviSingle = $wooviService->startCheckout(wooviCheckoutRequest(
    $wooviSingleEmail,
    $wooviPassword,
    'woovi-single-' . $suffix,
    'single'
));
$wooviProvider->charges[$wooviSingle['order_id']]['status'] = 'COMPLETED';
$wooviProvider->charges[$wooviSingle['order_id']]['transactionID'] =
    'tx-' . str_replace('-', '', (string) $wooviSingle['order_id']);
$wooviProvider->charges[$wooviSingle['order_id']]['paidAt'] = gmdate(DATE_ATOM);
$wooviService->reconcileOrder((string) $wooviSingle['order_id']);
$wooviLicense->execute(['order_id' => $wooviSingle['order_id']]);
$wooviSingleLicense = $wooviLicense->fetch();
expect(
    is_array($wooviSingleLicense)
        && (int) $wooviSingleLicense['max_active_devices'] === 1
        && $wooviSingleLicense['activation_policy'] === 'single_device',
    'plano individual emitiu uma unica vaga'
);

$wooviProvider->paymentBaseUrl = 'https://woovi-sandbox.com/pay/';
$wooviProvider->imageBaseUrl =
    'https://api.woovi-sandbox.com/openpix/charge/brcode/image/';
$wooviSandbox = $wooviService->startCheckout(wooviCheckoutRequest(
    "woovi-sandbox-{$suffix}@example.test",
    $wooviPassword,
    'woovi-sandbox-' . $suffix,
    'single'
));
expect(
    str_starts_with(
        (string) $wooviSandbox['checkout_url'],
        'https://woovi-sandbox.com/pay/'
    ),
    'checkout Woovi sandbox aceitou o host oficial de pagamento'
);
$wooviProvider->charges[$wooviSandbox['order_id']]['status'] = 'EXPIRED';
$wooviService->reconcileOrder((string) $wooviSandbox['order_id']);
$wooviProvider->paymentBaseUrl = 'https://app.woovi.com/pay/';
$wooviProvider->imageBaseUrl =
    'https://api.woovi.com/openpix/charge/brcode/image/';

// Pausar novas vendas nunca pode descartar a confirmacao de um Pix que ja
// estava aberto. O webhook continua exigindo a assinatura e a liquidacao
// consulta a cobranca diretamente no provedor antes de emitir a licenca.
$wooviWebhook = $wooviService->startCheckout(wooviCheckoutRequest(
    "woovi-webhook-{$suffix}@example.test",
    $wooviPassword,
    'woovi-webhook-' . $suffix,
    'single'
));
$wooviWebhookTransaction =
    'tx-' . str_replace('-', '', (string) $wooviWebhook['order_id']);
$wooviProvider->charges[$wooviWebhook['order_id']]['status'] = 'COMPLETED';
$wooviProvider->charges[$wooviWebhook['order_id']]['transactionID'] =
    $wooviWebhookTransaction;
$wooviProvider->charges[$wooviWebhook['order_id']]['paidAt'] = gmdate(DATE_ATOM);
$wooviWebhookBody = [
    'event' => 'OPENPIX:CHARGE_COMPLETED',
    'charge' => [
        'correlationID' => $wooviWebhook['order_id'],
        'transactionID' => $wooviWebhookTransaction,
    ],
];
$checkoutEnabledBeforeWebhookTest = getenv('CHECKOUT_ENABLED');
putenv('CHECKOUT_ENABLED=0');
$checkoutPausedConfig = Config::load($root);
expect(
    !$checkoutPausedConfig->bool('checkout.enabled'),
    'fixture realmente pausou a abertura de novos checkouts'
);
$checkoutPausedWooviService = new WooviPaymentService(
    $checkoutPausedConfig,
    $database,
    $crypto,
    $limiter,
    $wooviProvider,
    $mailer
);
expectApi(
    fn () => $checkoutPausedWooviService->receiveWebhook(
        Request::fromValues(
            'POST',
            '/api/v1/webhooks/woovi',
            ['x-webhook-signature' => 'invalid-woovi-signature'],
            $wooviWebhookBody,
            '127.0.0.64'
        )
    ),
    'INVALID_WEBHOOK_SIGNATURE'
);
$webhookCountBeforeProbe = (int) $pdo->query(
    "SELECT COUNT(*) FROM webhook_events WHERE provider = 'woovi'"
)->fetchColumn();
$checkoutPausedWooviService->receiveWebhook(Request::fromValues(
    'POST',
    '/api/v1/webhooks/woovi',
    ['x-webhook-signature' => 'valid-woovi-signature'],
    [
        'data_criacao' => gmdate(DATE_ATOM),
        'event' => 'OPENPIX:CHARGE_COMPLETED',
    ],
    '127.0.0.64'
));
expect(
    (int) $pdo->query(
        "SELECT COUNT(*) FROM webhook_events WHERE provider = 'woovi'"
    )->fetchColumn() === $webhookCountBeforeProbe,
    'probe assinado de cadastro Woovi respondeu sem criar evento financeiro'
);
expectApi(
    fn () => $checkoutPausedWooviService->receiveWebhook(
        Request::fromValues(
            'POST',
            '/api/v1/webhooks/woovi',
            ['x-webhook-signature' => 'valid-woovi-signature'],
            [
                'data_criacao' => gmdate(DATE_ATOM),
                'evento' => 'payload_financeiro_disfarcado',
                'event' => 'OPENPIX:CHARGE_COMPLETED',
            ],
            '127.0.0.64'
        )
    ),
    'INVALID_WEBHOOK'
);
$wooviProbeReturn = $checkoutPausedWooviService->receiveWebhook(
    Request::fromValues(
        'POST',
        '/api/v1/webhooks/woovi',
        ['x-webhook-signature' => 'valid-woovi-signature'],
        [
            'data_criacao' => gmdate(DATE_ATOM),
            'event' => 'OPENPIX:CHARGE_COMPLETED',
        ],
        '127.0.0.64'
    )
);
expect(
    $wooviProbeReturn === null,
    'probe de cadastro Woovi nao pede liberacao imediata'
);
$wooviWebhookReturn = $checkoutPausedWooviService->receiveWebhook(
    Request::fromValues(
        'POST',
        '/api/v1/webhooks/woovi',
        ['x-webhook-signature' => 'valid-woovi-signature'],
        $wooviWebhookBody,
        '127.0.0.64'
    )
);
// Contrato da liberacao IMEDIATA (19/08/2026): o webhook devolve o pedido
// para o App liberar logo depois do 200, sem esperar a janela do cron.
expect(
    $wooviWebhookReturn === (string) $wooviWebhook['order_id'],
    'webhook Woovi devolve o correlationId para liberacao imediata'
);
$checkoutPausedWooviService->settleWebhookNow($wooviWebhookReturn);
expect(
    (int) $pdo->query(
        "SELECT COUNT(*) FROM webhook_events
         WHERE provider = 'woovi' AND status = 'received'"
    )->fetchColumn() === 0,
    'liberacao imediata marcou o evento Woovi como processado'
);
expect(
    $checkoutPausedWooviService->processPendingWebhooks(10) === 0,
    'worker nao reprocessa evento Woovi ja liberado na hora'
);
$wooviLicense->execute(['order_id' => $wooviWebhook['order_id']]);
expect(
    is_array($wooviLicense->fetch()),
    'webhook assinado emitiu a licenca sem depender do polling do cliente'
);
if ($checkoutEnabledBeforeWebhookTest === false) {
    putenv('CHECKOUT_ENABLED');
} else {
    putenv('CHECKOUT_ENABLED=' . $checkoutEnabledBeforeWebhookTest);
}

$wooviAmbiguousEmail = "woovi-ambiguous-{$suffix}@example.test";
$wooviAmbiguousKey = 'woovi-ambiguous-' . $suffix;
$createCallsBeforeAmbiguous = $wooviProvider->createCalls;
$wooviProvider->timeoutAfterCreate = true;
try {
    $wooviService->startCheckout(wooviCheckoutRequest(
        $wooviAmbiguousEmail,
        $wooviPassword,
        $wooviAmbiguousKey,
        'complete'
    ));
    throw new RuntimeException('FAIL: timeout Woovi ambiguo nao propagou');
} catch (PaymentProviderException $error) {
    expect(
        $error->operationMayHaveSucceeded,
        'timeout Woovi foi classificado como ambiguo'
    );
}
$ambiguousOrder = $pdo->prepare(
    "SELECT o.id, o.status FROM orders o
     JOIN users u ON u.id = o.user_id WHERE u.email = :email"
);
$ambiguousOrder->execute(['email' => $wooviAmbiguousEmail]);
$ambiguousRow = $ambiguousOrder->fetch();
expect(
    is_array($ambiguousRow) && $ambiguousRow['status'] === 'provider_ambiguous',
    'timeout Woovi preservou pedido ambiguo para conciliacao'
);
$wooviRecovered = $wooviService->startCheckout(wooviCheckoutRequest(
    $wooviAmbiguousEmail,
    $wooviPassword,
    $wooviAmbiguousKey,
    'complete'
));
expect(
    $wooviRecovered['order_id'] === $ambiguousRow['id']
        && strlen((string) $wooviRecovered['pix_copy_paste']) >= 40
        && $wooviProvider->createCalls === $createCallsBeforeAmbiguous + 1,
    'retry Woovi conciliou a cobranca existente sem duplicar Pix'
);

putenv('MAIL_ENABLED=1');
$mailConfig = Config::load($root);
$mailConfig->assertRuntimeReady();
$queuedMailer = new Mailer($mailConfig, $database, new Crypto($mailConfig));
$queueToken = Crypto::randomToken(32);
$pdo->beginTransaction();
try {
    $queuedMailer->sendVerification(
        'queue-test@example.test',
        'Teste da fila',
        $queueToken
    );
    $queuedMailer->sendVerification(
        'queue-test@example.test',
        'Teste da fila',
        $queueToken
    );
    $queuedMail = $pdo->prepare(
        'SELECT COUNT(*) AS total, MIN(recipient_ciphertext) AS recipient_ciphertext,
                MIN(subject_ciphertext) AS subject_ciphertext,
                MIN(body_ciphertext) AS body_ciphertext
         FROM email_jobs WHERE dedupe_key = :dedupe_key'
    );
    $queuedMail->execute([
        'dedupe_key' => hash(
            'sha256',
            'email_verification|' . Crypto::tokenHash($queueToken)
        ),
    ]);
    $queuedMailRow = $queuedMail->fetch();
    expect(
        is_array($queuedMailRow)
            && (int) $queuedMailRow['total'] === 1
            && !str_contains(
                (string) $queuedMailRow['recipient_ciphertext'],
                'queue-test@example.test'
            )
            && $crypto->decrypt(
                (string) $queuedMailRow['recipient_ciphertext']
            ) === 'queue-test@example.test'
            && str_contains(
                $crypto->decrypt((string) $queuedMailRow['body_ciphertext']),
                $queueToken
            ),
        'fila de e-mail deduplicou e cifrou o conteudo sensivel'
    );
} finally {
    if ($pdo->inTransaction()) {
        $pdo->rollBack();
    }
    putenv('MAIL_ENABLED=0');
}

echo "PASS: SQL suportado, login, vagas por plataforma, Woovi Pix, polling, conciliacao sem duplicidade, lease, refresh, transferencia, CSRF, webhook assincrono, reembolso, mediacao, chargeback, entitlement unico e e-mail cifrado\n";
