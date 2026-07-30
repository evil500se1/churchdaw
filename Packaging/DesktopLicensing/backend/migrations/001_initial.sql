SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version VARCHAR(64) PRIMARY KEY,
    applied_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS users (
    id CHAR(36) PRIMARY KEY,
    email VARCHAR(254) NOT NULL,
    display_name VARCHAR(96) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'active',
    email_verified_at DATETIME(6) NULL,
    password_changed_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY users_email_unique (email),
    KEY users_status_idx (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS user_tokens (
    id CHAR(36) PRIMARY KEY,
    user_id CHAR(36) NOT NULL,
    type VARCHAR(32) NOT NULL,
    token_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    used_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY user_tokens_hash_unique (token_hash),
    KEY user_tokens_user_type_idx (user_id, type, expires_at),
    CONSTRAINT user_tokens_user_fk FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS products (
    code VARCHAR(80) PRIMARY KEY,
    name VARCHAR(160) NOT NULL,
    currency CHAR(3) NOT NULL,
    price_cents INT UNSIGNED NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'active',
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS orders (
    id CHAR(36) PRIMARY KEY,
    user_id CHAR(36) NOT NULL,
    product_code VARCHAR(80) NOT NULL,
    provider VARCHAR(32) NOT NULL,
    idempotency_key VARCHAR(128) NOT NULL,
    expected_amount_cents INT UNSIGNED NOT NULL,
    currency CHAR(3) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'created',
    open_slot TINYINT UNSIGNED NULL,
    checkout_claim_token CHAR(36) NULL,
    checkout_claimed_at DATETIME(6) NULL,
    checkout_expires_at DATETIME(6) NULL,
    terms_version VARCHAR(32) NOT NULL,
    privacy_version VARCHAR(32) NOT NULL,
    accepted_at DATETIME(6) NOT NULL,
    acceptance_ip_hash CHAR(64) NOT NULL,
    provider_preference_id VARCHAR(128) NULL,
    provider_order_id VARCHAR(128) NULL,
    checkout_url TEXT NULL,
    provider_payment_id VARCHAR(128) NULL,
    provider_status VARCHAR(64) NULL,
    provider_reconciled_at DATETIME(6) NULL,
    provider_reconcile_attempted_at DATETIME(6) NULL,
    provider_reconcile_failures SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    provider_reconcile_error VARCHAR(500) NULL,
    paid_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY orders_idempotency_unique (user_id, idempotency_key),
    UNIQUE KEY orders_one_open_per_product (user_id, product_code, open_slot),
    UNIQUE KEY orders_provider_preference_unique (provider, provider_preference_id),
    UNIQUE KEY orders_provider_order_unique (provider, provider_order_id),
    KEY orders_provider_payment_idx (provider, provider_payment_id),
    KEY orders_status_idx (status, created_at),
    KEY orders_reconciliation_idx (provider, provider_reconciled_at, created_at),
    KEY orders_reconcile_attempt_idx (
        provider, provider_reconcile_attempted_at, created_at
    ),
    CONSTRAINT orders_user_fk FOREIGN KEY (user_id) REFERENCES users(id),
    CONSTRAINT orders_product_fk FOREIGN KEY (product_code) REFERENCES products(code)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS payments (
    id CHAR(36) PRIMARY KEY,
    order_id CHAR(36) NOT NULL,
    provider VARCHAR(32) NOT NULL,
    provider_payment_id VARCHAR(128) NOT NULL,
    amount_cents INT UNSIGNED NOT NULL,
    refunded_amount_cents INT UNSIGNED NOT NULL DEFAULT 0,
    currency CHAR(3) NOT NULL,
    status VARCHAR(64) NOT NULL,
    raw_fingerprint CHAR(64) NOT NULL,
    approved_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY payments_provider_unique (provider, provider_payment_id),
    KEY payments_order_idx (order_id),
    CONSTRAINT payments_order_fk FOREIGN KEY (order_id) REFERENCES orders(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS licenses (
    id CHAR(36) PRIMARY KEY,
    user_id CHAR(36) NOT NULL,
    product_code VARCHAR(80) NOT NULL,
    order_id CHAR(36) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'active',
    active_slot TINYINT UNSIGNED NULL,
    max_active_devices TINYINT UNSIGNED NOT NULL DEFAULT 1,
    issued_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    revoked_at DATETIME(6) NULL,
    revoke_reason VARCHAR(160) NULL,
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY licenses_order_unique (order_id),
    UNIQUE KEY licenses_one_active_per_product (user_id, product_code, active_slot),
    KEY licenses_user_status_idx (user_id, status),
    CONSTRAINT licenses_active_slot_check CHECK (
        (status = 'active' AND active_slot IS NOT NULL AND active_slot = 1)
        OR (status <> 'active' AND active_slot IS NULL)
    ),
    CONSTRAINT licenses_user_fk FOREIGN KEY (user_id) REFERENCES users(id),
    CONSTRAINT licenses_product_fk FOREIGN KEY (product_code) REFERENCES products(code),
    CONSTRAINT licenses_order_fk FOREIGN KEY (order_id) REFERENCES orders(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS devices (
    id CHAR(36) PRIMARY KEY,
    license_id CHAR(36) NOT NULL,
    installation_id VARCHAR(128) NOT NULL,
    label VARCHAR(120) NOT NULL,
    platform VARCHAR(24) NOT NULL,
    secret_ciphertext TEXT NOT NULL,
    secret_fingerprint CHAR(64) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'active',
    first_seen_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    last_seen_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY devices_installation_unique (license_id, installation_id),
    KEY devices_status_idx (license_id, status),
    CONSTRAINT devices_license_fk FOREIGN KEY (license_id) REFERENCES licenses(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS activations (
    id CHAR(36) PRIMARY KEY,
    license_id CHAR(36) NOT NULL,
    device_id CHAR(36) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'active',
    active_slot TINYINT UNSIGNED NULL,
    activated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    last_validated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    deactivated_at DATETIME(6) NULL,
    deactivation_reason VARCHAR(64) NULL,
    UNIQUE KEY activations_one_active_per_license (license_id, active_slot),
    KEY activations_device_idx (device_id, status),
    CONSTRAINT activations_license_fk FOREIGN KEY (license_id) REFERENCES licenses(id),
    CONSTRAINT activations_device_fk FOREIGN KEY (device_id) REFERENCES devices(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS refresh_tokens (
    id CHAR(36) PRIMARY KEY,
    family_id CHAR(36) NOT NULL,
    user_id CHAR(36) NOT NULL,
    license_id CHAR(36) NOT NULL,
    device_id CHAR(36) NOT NULL,
    token_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    consumed_at DATETIME(6) NULL,
    revoked_at DATETIME(6) NULL,
    replaced_by_id CHAR(36) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY refresh_tokens_hash_unique (token_hash),
    KEY refresh_tokens_family_idx (family_id, revoked_at),
    KEY refresh_tokens_device_idx (device_id, expires_at),
    CONSTRAINT refresh_tokens_user_fk FOREIGN KEY (user_id) REFERENCES users(id),
    CONSTRAINT refresh_tokens_license_fk FOREIGN KEY (license_id) REFERENCES licenses(id),
    CONSTRAINT refresh_tokens_device_fk FOREIGN KEY (device_id) REFERENCES devices(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS request_nonces (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    device_id CHAR(36) NOT NULL,
    nonce_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY request_nonces_unique (device_id, nonce_hash),
    KEY request_nonces_expiry_idx (expires_at),
    CONSTRAINT request_nonces_device_fk FOREIGN KEY (device_id) REFERENCES devices(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS license_transfers (
    id CHAR(36) PRIMARY KEY,
    license_id CHAR(36) NOT NULL,
    from_device_id CHAR(36) NULL,
    requested_by_user_id CHAR(36) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    KEY license_transfers_window_idx (license_id, created_at),
    CONSTRAINT license_transfers_license_fk FOREIGN KEY (license_id) REFERENCES licenses(id),
    CONSTRAINT license_transfers_device_fk FOREIGN KEY (from_device_id) REFERENCES devices(id),
    CONSTRAINT license_transfers_user_fk FOREIGN KEY (requested_by_user_id) REFERENCES users(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS webhook_events (
    id CHAR(36) PRIMARY KEY,
    provider VARCHAR(32) NOT NULL,
    delivery_id VARCHAR(160) NOT NULL,
    resource_id VARCHAR(160) NOT NULL,
    event_type VARCHAR(80) NOT NULL,
    payload_json MEDIUMTEXT NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'received',
    attempts SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    next_attempt_at DATETIME(6) NULL DEFAULT CURRENT_TIMESTAMP(6),
    processing_started_at DATETIME(6) NULL,
    last_error VARCHAR(500) NULL,
    received_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    processed_at DATETIME(6) NULL,
    UNIQUE KEY webhook_events_delivery_unique (provider, delivery_id),
    KEY webhook_events_queue_idx (status, next_attempt_at, received_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS rate_limits (
    bucket VARCHAR(64) NOT NULL,
    key_hash CHAR(64) NOT NULL,
    window_started_at DATETIME NOT NULL,
    attempts INT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (bucket, key_hash, window_started_at),
    KEY rate_limits_cleanup_idx (window_started_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS web_sessions (
    id CHAR(36) PRIMARY KEY,
    user_id CHAR(36) NOT NULL,
    token_hash CHAR(64) NOT NULL,
    csrf_hash CHAR(64) NOT NULL,
    csrf_ciphertext TEXT NOT NULL,
    ip_hash CHAR(64) NOT NULL,
    user_agent_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    last_seen_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    revoked_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY web_sessions_token_unique (token_hash),
    KEY web_sessions_user_idx (user_id, expires_at),
    CONSTRAINT web_sessions_user_fk FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS email_jobs (
    id CHAR(36) PRIMARY KEY,
    type VARCHAR(32) NOT NULL,
    dedupe_key CHAR(64) NOT NULL,
    recipient_ciphertext TEXT NOT NULL,
    subject_ciphertext TEXT NOT NULL,
    body_ciphertext MEDIUMTEXT NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'queued',
    attempts SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    available_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    processing_started_at DATETIME(6) NULL,
    last_error VARCHAR(500) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    sent_at DATETIME(6) NULL,
    UNIQUE KEY email_jobs_dedupe_unique (dedupe_key),
    KEY email_jobs_queue_idx (status, available_at, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS audit_events (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    request_id VARCHAR(128) NULL,
    actor_user_id CHAR(36) NULL,
    license_id CHAR(36) NULL,
    event_type VARCHAR(80) NOT NULL,
    ip_hash CHAR(64) NULL,
    metadata_json TEXT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    KEY audit_events_user_idx (actor_user_id, created_at),
    KEY audit_events_license_idx (license_id, created_at),
    KEY audit_events_type_idx (event_type, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;
