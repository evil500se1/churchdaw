<?php

declare(strict_types=1);

use ChurchDAW\Licensing\ApiException;
use ChurchDAW\Licensing\Config;
use ChurchDAW\Licensing\Crypto;
use ChurchDAW\Licensing\MercadoPagoClient;
use ChurchDAW\Licensing\Money;
use ChurchDAW\Licensing\MigrationState;
use ChurchDAW\Licensing\PasswordPolicy;
use ChurchDAW\Licensing\TrustedProxy;
use ChurchDAW\Licensing\Views;

$root = dirname(__DIR__);
require $root . '/src/autoload.php';

function expect(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException('FAIL: ' . $message);
    }
}

$privateKey = getenv('LICENSE_PRIVATE_KEY_PATH');
if ($privateKey === false || !is_file($privateKey)) {
    throw new RuntimeException('LICENSE_PRIVATE_KEY_PATH ausente.');
}

putenv('APP_ENV=test');
putenv('APP_BASE_URL=https://churchdaw.com');
putenv('APP_ENCRYPTION_KEY=' . base64_encode(str_repeat("\x5a", 32)));
putenv('IP_HASH_KEY=' . str_repeat('i', 48));
putenv('DB_DSN=mysql:host=127.0.0.1;dbname=unused');
putenv('DB_USER=unused');
putenv('MP_ACCESS_TOKEN=TEST-unused');
putenv('MP_WEBHOOK_SECRET=webhook-test-secret-32-bytes-long');
putenv('PRODUCT_PRICE_CENTS=49900');
putenv('MAIL_ENABLED=0');
$libraryStorage = sys_get_temp_dir() . '/churchdaw-unit-library';
if (!is_dir($libraryStorage)
    && !mkdir($libraryStorage, 0700, true)
    && !is_dir($libraryStorage)) {
    throw new RuntimeException('Nao foi possivel preparar a biblioteca de teste.');
}
putenv('LIBRARY_STORAGE_PATH=' . $libraryStorage);

$config = Config::load($root);
$crypto = new Crypto($config);
expect(
    !$config->bool('checkout.enabled'),
    'checkout deve iniciar pausado ate liberacao explicita'
);

TrustedProxy::assertSafeConfiguration(
    '173.245.48.0/20 2400:cb00::/32'
);
$unsafeProxyRejected = false;
try {
    TrustedProxy::assertSafeConfiguration('0.0.0.0/0');
} catch (RuntimeException) {
    $unsafeProxyRejected = true;
}
expect($unsafeProxyRejected, 'TRUSTED_PROXY amplo foi aceito');

putenv('APP_ENV=production');
putenv('MP_SANDBOX=0');
putenv('MP_COLLECTOR_ID=123456');
putenv('MP_APPLICATION_ID=654321');
putenv('DB_PASSWORD=production-test-password');
$missingProductionReleaseRejected = false;
try {
    Config::load($root)->assertRuntimeReady();
} catch (RuntimeException $error) {
    $missingProductionReleaseRejected = str_contains(
        $error->getMessage(),
        'APP_RELEASE'
    );
}
expect(
    $missingProductionReleaseRejected,
    'producao permitiu release sem identificador'
);
putenv('APP_RELEASE=unit-production');
$disabledProductionMailRejected = false;
try {
    Config::load($root)->assertRuntimeReady();
} catch (RuntimeException $error) {
    $disabledProductionMailRejected = str_contains(
        $error->getMessage(),
        'MAIL_ENABLED'
    );
}
expect(
    $disabledProductionMailRejected,
    'producao permitiu vender com e-mail desativado'
);
putenv('MAIL_ENABLED=1');
putenv('MP_ACCESS_TOKEN');
putenv('MP_WEBHOOK_SECRET');
putenv('MP_COLLECTOR_ID');
putenv('MP_APPLICATION_ID');
putenv('MP_SANDBOX=1');
Config::load($root)->assertRuntimeReady();

putenv('CHECKOUT_ENABLED=1');
$checkoutWithoutProviderRejected = false;
try {
    Config::load($root)->assertRuntimeReady();
} catch (RuntimeException $error) {
    $checkoutWithoutProviderRejected = str_contains(
        $error->getMessage(),
        'mercado_pago.access_token'
    );
}
expect(
    $checkoutWithoutProviderRejected,
    'checkout foi ativado sem credenciais do provedor'
);
putenv('CHECKOUT_ENABLED=0');
putenv('APP_ENV=test');
putenv('APP_RELEASE');
putenv('MP_SANDBOX=1');
putenv('MP_COLLECTOR_ID');
putenv('MP_APPLICATION_ID');
putenv('DB_PASSWORD');
putenv('MAIL_ENABLED=0');
$views = new Views($config);

$resetHtml = $views->resetPassword();
$verifyHtml = $views->verifyEmail();
expect(str_contains($resetHtml, 'id="reset-form"'), 'view de reset ausente');
expect(str_contains($verifyHtml, 'id="verify-form"'), 'view de verificacao ausente');
expect(
    str_contains($resetHtml, '/account-assets/unreleased/app.css')
        && str_contains($resetHtml, '/account-assets/unreleased/app.js'),
    'views nao vinculam os assets ao APP_RELEASE'
);
expect(
    !str_contains($resetHtml . $verifyHtml, 'data-token'),
    'token de acao vazou para o HTML'
);

$completeSchema = MigrationState::compare(
    ['001.sql', '002.sql', '003.sql'],
    ['003.sql', '001.sql', '002.sql']
);
expect(
    $completeSchema['ready']
        && $completeSchema['current'] === '003.sql'
        && $completeSchema['missing'] === []
        && $completeSchema['unexpected'] === [],
    'comparacao de schema completo rejeitou lista equivalente'
);
$gappedSchema = MigrationState::compare(
    ['001.sql', '002.sql', '003.sql'],
    ['001.sql', '003.sql', '999.sql']
);
expect(
    !$gappedSchema['ready']
        && $gappedSchema['missing'] === ['002.sql']
        && $gappedSchema['unexpected'] === ['999.sql'],
    'health aceitaria schema com migration intermediaria ausente'
);

$mailerSource = file_get_contents($root . '/src/Mailer.php');
$appSource = file_get_contents($root . '/src/App.php');
$browserSource = file_get_contents($root . '/public/account-assets/app.js');
expect(is_string($mailerSource), 'fonte do Mailer ausente');
expect(is_string($appSource), 'fonte do App ausente');
expect(is_string($browserSource), 'JavaScript do navegador ausente');
expect(
    str_contains($mailerSource, '/verificar-email#token=')
        && str_contains($mailerSource, '/redefinir-senha#token='),
    'links de acao nao usam fragmento'
);
expect(
    !str_contains($mailerSource, '/verificar-email?token=')
        && !str_contains($mailerSource, '/redefinir-senha?token='),
    'Mailer ainda usa token em query string'
);
expect(!str_contains($appSource, "\$_GET['token']"), 'App ainda le token da query');
expect(!str_contains($browserSource, 'dataset.token'), 'JavaScript ainda le token do DOM');

$plaintext = 'segredo-dispositivo-' . random_bytes(8);
$ciphertext = $crypto->encrypt($plaintext);
expect($ciphertext !== $plaintext, 'secretbox nao cifrou');
expect($crypto->decrypt($ciphertext) === $plaintext, 'secretbox nao abriu');
$tamperedBytes = Crypto::base64UrlDecode($ciphertext);
expect(is_string($tamperedBytes), 'secretbox Base64URL valido');
$tamperedBytes[-1] = chr(ord($tamperedBytes[-1]) ^ 0x01);
$tampered = Crypto::base64UrlEncode($tamperedBytes);
$tamperRejected = false;
try {
    $crypto->decrypt($tampered);
} catch (Throwable) {
    $tamperRejected = true;
}
expect($tamperRejected, 'secretbox adulterada foi aceita');

expect(Money::toCents('499.00') === 49900, 'conversao monetaria');
expect(Money::toCents(499.0) === 49900, 'conversao monetaria float');

PasswordPolicy::assertValid('uma frase longa e exclusiva', 'teste@churchdaw.com');
$weakRejected = false;
try {
    PasswordPolicy::assertValid('senha123');
} catch (ApiException) {
    $weakRejected = true;
}
expect($weakRejected, 'senha fraca foi aceita');

$now = time();
$installationId = 'fixture-installation-id';
$claims = [
    'iss' => 'https://churchdaw.com',
    'aud' => 'churchdaw-desktop',
    'sub' => '00000000-0000-4000-8000-000000000001',
    'jti' => '00000000-0000-4000-8000-000000000002',
    'email' => 'teste@churchdaw.com',
    'display_name' => 'Conta de teste',
    'license_id' => '00000000-0000-4000-8000-000000000003',
    'product_code' => 'churchdaw-desktop-perpetual',
    'activation_id' => '00000000-0000-4000-8000-000000000004',
    'activation_label' => 'PC de teste / Windows',
    'installation_id' => $installationId,
    'iat' => $now,
    'nbf' => $now - 30,
    'lease_expires_at' => $now + 3600,
    'offline_until' => $now + 604800,
];
$token = $crypto->signCompact($claims, 'churchdaw-lease+jwt');
$parts = explode('.', $token);
expect(count($parts) === 3, 'token compacto invalido');
$signature = Crypto::base64UrlDecode($parts[2]);
$publicKey = file_get_contents(dirname($root) . '/keys/churchdaw-license-public.pem');
expect(is_string($signature) && is_string($publicKey), 'fixture RSA ausente');
expect(
    openssl_verify(
        $parts[0] . '.' . $parts[1],
        $signature,
        $publicKey,
        OPENSSL_ALGO_SHA256
    ) === 1,
    'assinatura RSA PHP invalida'
);

$legacyAndroidClaims = [
    'iss' => 'https://churchdaw.com',
    'aud' => 'churchdaw-android',
    'sub' => '00000000-0000-4000-8000-000000000011',
    'jti' => '00000000-0000-4000-8000-000000000012',
    'license_id' => '00000000-0000-4000-8000-000000000013',
    'product_code' => 'churchdaw-desktop-perpetual',
    'activation_id' => '00000000-0000-4000-8000-000000000014',
    'installation_id' => 'fixture-android-installation-id',
    'platform' => 'android',
    'mode' => 'licensed',
    'iat' => $now,
    'nbf' => $now - 30,
    'lease_expires_at' => $now + 3600,
    'offline_until' => $now + 604800,
];
$legacyAndroidLease = $crypto->signCompact(
    $legacyAndroidClaims,
    'churchdaw-lease+jwt'
);
$downloadGate = new ReflectionMethod(
    ChurchDAW\Licensing\LibraryDownloadService::class,
    'isAuthorizedDownloadToken'
);
$publicBaseGate = new ReflectionMethod(
    ChurchDAW\Licensing\LibraryDownloadService::class,
    'isPublicBaseLibraryFileName'
);
$safeFileNameGate = new ReflectionMethod(
    ChurchDAW\Licensing\LibraryDownloadService::class,
    'isSafeLibraryFileName'
);
$isPublicBase = static fn (string $fileName): bool =>
    (bool) $publicBaseGate->invoke(null, $fileName);
$isSafeFileName = static fn (string $fileName): bool =>
    (bool) $safeFileNameGate->invoke(null, $fileName);
expect(
    $isPublicBase('church-live-v1-segment-01.zip')
        && $isPublicBase('church-live-v1-segment-30.zip'),
    'partes limite da Church Live nao ficaram publicas'
);
foreach ([
    'church-live-v1-segment-00.zip',
    'church-live-v1-segment-31.zip',
    'church-live-v2-segment-01.zip',
    'worship-keys-v1-segment-01.zip',
    '../church-live-v1-segment-01.zip',
] as $privateOrInvalidFile) {
    expect(
        !$isPublicBase($privateOrInvalidFile),
        "allowlist publica aceitou {$privateOrInvalidFile}"
    );
}
expect(
    $isSafeFileName('worship-keys-v1-segment-01.zip')
        && !$isSafeFileName('../worship-keys-v1-segment-01.zip')
        && !$isSafeFileName('worship-keys-v1-segment-01.php'),
    'validacao generica de nome da biblioteca ficou insegura'
);
$allowsDownload = static function (
    array $candidateClaims,
    string $type
) use ($crypto, $downloadGate, $now): bool {
    return $downloadGate->invoke(
        null,
        $crypto->signCompact($candidateClaims, $type),
        $candidateClaims,
        $now
    );
};
expect(
    $downloadGate->invoke(
        null,
        $legacyAndroidLease,
        $legacyAndroidClaims,
        $now
    ),
    'lease paga Android legada valida foi recusada'
);
expect(
    !$allowsDownload($legacyAndroidClaims, 'at+jwt'),
    'compatibilidade legada aceitou access token'
);
$legacyExpired = $legacyAndroidClaims;
$legacyExpired['offline_until'] = $now - 1;
expect(
    !$allowsDownload($legacyExpired, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou janela offline expirada'
);
$legacyWithScope = $legacyAndroidClaims;
$legacyWithScope['scope'] = 'android:license';
expect(
    !$allowsDownload($legacyWithScope, 'churchdaw-lease+jwt'),
    'compatibilidade legada ignorou scope explicito sem download'
);
$legacyWrongAudience = $legacyAndroidClaims;
$legacyWrongAudience['aud'] = 'churchdaw-desktop';
expect(
    !$allowsDownload($legacyWrongAudience, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou audience desktop'
);
$legacyWrongPlatform = $legacyAndroidClaims;
$legacyWrongPlatform['platform'] = 'windows';
expect(
    !$allowsDownload($legacyWrongPlatform, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou plataforma desktop'
);
$legacyUnknownProduct = $legacyAndroidClaims;
$legacyUnknownProduct['product_code'] = 'churchdaw-outro-produto';
expect(
    !$allowsDownload($legacyUnknownProduct, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou produto desconhecido'
);
$legacyInvalidIdentity = $legacyAndroidClaims;
$legacyInvalidIdentity['license_id'] = 'nao-e-uuid';
expect(
    !$allowsDownload($legacyInvalidIdentity, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou identidade invalida'
);
$legacyInvalidInstallation = $legacyAndroidClaims;
$legacyInvalidInstallation['installation_id'] = '../outro-aparelho';
expect(
    !$allowsDownload($legacyInvalidInstallation, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou installation_id invalido'
);
$legacyTooLong = $legacyAndroidClaims;
$legacyTooLong['offline_until'] = $now + 9 * 86400;
expect(
    !$allowsDownload($legacyTooLong, 'churchdaw-lease+jwt'),
    'compatibilidade legada aceitou janela maior que a historica'
);

$newPaidLease = $legacyAndroidClaims;
$newPaidLease['scope'] = 'android:license library:download';
$newPaidLease['exp'] = $newPaidLease['offline_until'];
expect(
    $allowsDownload($newPaidLease, 'churchdaw-lease+jwt'),
    'lease paga Android nova foi recusada'
);
$newPaidLeaseCurrentProduct = $newPaidLease;
$newPaidLeaseCurrentProduct['product_code'] = 'churchdaw-perpetual';
expect(
    $allowsDownload($newPaidLeaseCurrentProduct, 'churchdaw-lease+jwt'),
    'produto Android atual foi recusado'
);
$newPaidLeaseScopeSpoof = $newPaidLease;
$newPaidLeaseScopeSpoof['scope'] = 'android:license notlibrary:download';
expect(
    !$allowsDownload($newPaidLeaseScopeSpoof, 'churchdaw-lease+jwt'),
    'scope parecido com library:download foi aceito'
);
$newPaidLeaseMissingPurpose = $newPaidLease;
$newPaidLeaseMissingPurpose['scope'] = 'library:download';
expect(
    !$allowsDownload($newPaidLeaseMissingPurpose, 'churchdaw-lease+jwt'),
    'lease paga sem android:license foi aceita'
);
$newPaidLeaseMismatchedExpiry = $newPaidLease;
$newPaidLeaseMismatchedExpiry['exp']--;
expect(
    !$allowsDownload($newPaidLeaseMismatchedExpiry, 'churchdaw-lease+jwt'),
    'lease nova aceitou exp divergente da janela offline'
);
$newPaidLeaseMissingScope = $newPaidLease;
unset($newPaidLeaseMissingScope['scope']);
expect(
    !$allowsDownload($newPaidLeaseMissingScope, 'churchdaw-lease+jwt'),
    'lease com exp mas sem scope foi aceita como legada'
);

$paidAccess = $legacyAndroidClaims;
unset($paidAccess['lease_expires_at'], $paidAccess['offline_until']);
$paidAccess['scope'] = 'android:license library:download';
$paidAccess['exp'] = $now + 900;
expect(
    $allowsDownload($paidAccess, 'at+jwt'),
    'access token pago Android valido foi recusado'
);
$paidAccessTooLong = $paidAccess;
$paidAccessTooLong['exp'] = $now + 3601;
expect(
    !$allowsDownload($paidAccessTooLong, 'at+jwt'),
    'access token pago com duracao excessiva foi aceito'
);
$paidAccessWrongScope = $paidAccess;
$paidAccessWrongScope['scope'] = 'android:license library:download-extra';
expect(
    !$allowsDownload($paidAccessWrongScope, 'at+jwt'),
    'access token aceitou scope por substring'
);
$paidAccessMissingPurpose = $paidAccess;
$paidAccessMissingPurpose['scope'] = 'library:download';
expect(
    !$allowsDownload($paidAccessMissingPurpose, 'at+jwt'),
    'access token sem android:license foi aceito'
);

$trialId = '00000000-0000-4000-8000-000000000021';
$trialSetupClaims = [
    'iss' => 'https://churchdaw.com',
    'aud' => 'churchdaw-android',
    'sub' => 'trial:' . $trialId,
    'jti' => '00000000-0000-4000-8000-000000000022',
    'trial_id' => $trialId,
    'installation_id' => 'fixture-trial-installation-id',
    'public_key_fingerprint' => str_repeat('a', 64),
    'mode' => 'trial_setup',
    'scope' => 'android:trial-start library:download',
    'iat' => $now,
    'nbf' => $now - 30,
    'exp' => $now + 24 * 3600,
];
expect(
    $allowsDownload($trialSetupClaims, 'churchdaw-setup+jwt'),
    'reserva de demonstracao valida foi recusada'
);
$trialSetupMissingPurpose = $trialSetupClaims;
$trialSetupMissingPurpose['scope'] = 'library:download';
expect(
    !$allowsDownload($trialSetupMissingPurpose, 'churchdaw-setup+jwt'),
    'reserva sem android:trial-start foi aceita'
);
$trialSetupTooLong = $trialSetupClaims;
$trialSetupTooLong['exp'] = $now + 72 * 3600 + 1;
expect(
    !$allowsDownload($trialSetupTooLong, 'churchdaw-setup+jwt'),
    'reserva de demonstracao longa demais foi aceita'
);
$trialSetupWrongFingerprint = $trialSetupClaims;
$trialSetupWrongFingerprint['public_key_fingerprint'] = 'nao-e-sha256';
expect(
    !$allowsDownload($trialSetupWrongFingerprint, 'churchdaw-setup+jwt'),
    'reserva com fingerprint invalido foi aceita'
);
expect(
    !$allowsDownload($trialSetupClaims, 'at+jwt'),
    'claims de reserva foram aceitas com tipo de access token'
);
$unsupportedTypeRejected = false;
try {
    $crypto->verifyCompact(
        $crypto->signCompact($trialSetupClaims, 'churchdaw-outro+jwt'),
        ['at+jwt', 'churchdaw-lease+jwt', 'churchdaw-setup+jwt']
    );
} catch (ApiException $error) {
    $unsupportedTypeRejected = $error->apiCode === 'INVALID_TOKEN';
}
expect($unsupportedTypeRejected, 'Crypto aceitou tipo de token desconhecido');

$trialExpiresAt = $now + 20 * 60;
$trialClaims = [
    'iss' => 'https://churchdaw.com',
    'aud' => 'churchdaw-android',
    'sub' => 'trial:' . $trialId,
    'jti' => '00000000-0000-4000-8000-000000000023',
    'trial_id' => $trialId,
    'product_code' => 'churchdaw-demo',
    'activation_id' => 'trial:' . $trialId,
    'installation_id' => 'fixture-trial-installation-id',
    'platform' => 'android',
    'mode' => 'trial',
    'scope' => 'android:trial library:download',
    'iat' => $now,
    'nbf' => $now - 30,
    'exp' => $trialExpiresAt,
    'lease_expires_at' => $trialExpiresAt,
    'offline_until' => $trialExpiresAt,
    'trial_expires_at' => $trialExpiresAt,
];
expect(
    $allowsDownload($trialClaims, 'churchdaw-lease+jwt'),
    'lease de demonstracao valida foi recusada'
);
$trialWrongActivation = $trialClaims;
$trialWrongActivation['activation_id'] =
    'trial:00000000-0000-4000-8000-000000000024';
expect(
    !$allowsDownload($trialWrongActivation, 'churchdaw-lease+jwt'),
    'lease de demonstracao aceitou identidade divergente'
);
$trialMismatchedExpiry = $trialClaims;
$trialMismatchedExpiry['offline_until']--;
expect(
    !$allowsDownload($trialMismatchedExpiry, 'churchdaw-lease+jwt'),
    'lease de demonstracao aceitou expiracoes divergentes'
);
$trialTooLong = $trialClaims;
$trialTooLong['exp'] = $now + 120 * 60 + 1;
$trialTooLong['lease_expires_at'] = $trialTooLong['exp'];
$trialTooLong['offline_until'] = $trialTooLong['exp'];
$trialTooLong['trial_expires_at'] = $trialTooLong['exp'];
expect(
    !$allowsDownload($trialTooLong, 'churchdaw-lease+jwt'),
    'lease de demonstracao longa demais foi aceita'
);

$expiredAccess = $paidAccess;
$expiredAccess['exp'] = $now - 1;
$expiredRejected = false;
try {
    $crypto->verifyCompact(
        $crypto->signCompact($expiredAccess, 'at+jwt'),
        ['at+jwt']
    );
} catch (ApiException $error) {
    $expiredRejected = $error->apiCode === 'TOKEN_EXPIRED';
}
expect($expiredRejected, 'Crypto aceitou access token expirado');

$tamperedParts = explode(
    '.',
    $crypto->signCompact($paidAccess, 'at+jwt')
);
$tamperedParts[2][0] = $tamperedParts[2][0] === 'A' ? 'B' : 'A';
$tamperedRejected = false;
try {
    $crypto->verifyCompact(implode('.', $tamperedParts), ['at+jwt']);
} catch (ApiException $error) {
    $tamperedRejected = $error->apiCode === 'INVALID_TOKEN';
}
expect($tamperedRejected, 'Crypto aceitou assinatura adulterada');

$webhook = new MercadoPagoClient($config);
$requestId = 'request-fixture-123';
$dataId = '123456789';
$timestamp = (string) time();
$manifest = "id:{$dataId};request-id:{$requestId};ts:{$timestamp};";
$signatureHeader = 'ts=' . $timestamp . ',v1='
    . hash_hmac('sha256', $manifest, 'webhook-test-secret-32-bytes-long');
expect(
    $webhook->verifyWebhookSignature($signatureHeader, $requestId, $dataId),
    'webhook valido recusado'
);
expect(
    !$webhook->verifyWebhookSignature($signatureHeader, $requestId, '987654321'),
    'webhook adulterado aceito'
);

$fixturePath = $argv[1] ?? '';
if ($fixturePath !== '') {
    if (file_put_contents($fixturePath, $token) === false) {
        throw new RuntimeException('Nao foi possivel gravar o fixture.');
    }
}

echo "PASS: PHP crypto, dinheiro, senha, RS256 e webhook\n";
echo "FIXTURE_INSTALLATION={$installationId}\n";
echo "FIXTURE_TIME={$now}\n";
