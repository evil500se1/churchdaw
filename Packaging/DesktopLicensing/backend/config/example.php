<?php

declare(strict_types=1);

// Copie para um caminho fora do document root e aponte CHURCHDAW_CONFIG_FILE
// para ele. Nunca versionar chaves ou credenciais reais.
return [
    'app.env' => 'production',
    'app.release' => '2026-07-28.1',
    'app.base_url' => 'https://churchdaw.com',
    // IPs/CIDRs separados por vírgula (por exemplo, os ranges atuais da Cloudflare).
    'app.trusted_proxy' => '',
    'app.encryption_key' => 'BASE64_DE_32_BYTES_ALEATORIOS',
    'app.ip_hash_key' => 'MINIMO_32_CARACTERES_ALEATORIOS',
    'db.dsn' => 'mysql:host=127.0.0.1;dbname=churchdaw;charset=utf8mb4',
    'db.user' => 'churchdaw_app',
    'db.password' => 'SENHA_FORTE_DO_BANCO',
    'license.private_key_path' => '/home/USUARIO/secure/churchdaw-license-private.pem',
    'license.key_id' => 'churchdaw-2026-01',
    'trial.minutes' => 20,
    'trial.setup_hours' => 24,
    'library.storage_path' => '/home/USUARIO/churchdaw-private/android-packs',
    'mercado_pago.access_token' => 'APP_USR-...',
    'mercado_pago.webhook_secret' => 'SEGREDO_DO_WEBHOOK',
    'mercado_pago.collector_id' => '',
    'mercado_pago.application_id' => '',
    'mercado_pago.sandbox' => false,
    'checkout.expiration_minutes' => 4320,
    'checkout.claim_ttl_seconds' => 120,
    'checkout.enabled' => false,
    'product.code' => 'churchdaw-perpetual',
    'product.name' => 'ChurchDAW Windows, macOS e Android',
    'product.currency' => 'BRL',
    'product.price_cents' => 49900,
    'legal.terms_version' => '2026-07-28',
    'legal.privacy_version' => '2026-07-28',
    'mail.from' => 'contato@churchdaw.com',
    'mail.reply_to' => 'contato@churchdaw.com',
    'mail.enabled' => true,
    'support.url' => 'https://churchdaw.com/#duvidas',
];
