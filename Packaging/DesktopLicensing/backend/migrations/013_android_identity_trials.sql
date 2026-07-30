-- Android licensing uses a non-exportable P-256 key from Android Keystore.
-- Existing desktop shared-secret rows remain valid.

ALTER TABLE devices
    MODIFY secret_ciphertext TEXT NULL,
    ADD COLUMN identity_type VARCHAR(40) NOT NULL
        DEFAULT 'shared-secret-v1' AFTER platform,
    ADD COLUMN public_key_pem TEXT NULL AFTER secret_ciphertext,
    ADD COLUMN device_fingerprint_hash CHAR(64) NULL
        AFTER secret_fingerprint,
    ADD KEY devices_fingerprint_idx
        (device_fingerprint_hash, status);

CREATE TABLE IF NOT EXISTS android_trials (
    id CHAR(36) PRIMARY KEY,
    installation_id VARCHAR(128) NOT NULL,
    device_fingerprint_hash CHAR(64) NOT NULL,
    public_key_pem TEXT NOT NULL,
    public_key_fingerprint CHAR(64) NOT NULL,
    device_label VARCHAR(120) NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'reserved',
    reserved_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    reservation_expires_at DATETIME(6) NOT NULL,
    started_at DATETIME(6) NULL,
    expires_at DATETIME(6) NULL,
    last_seen_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    last_ip_hash CHAR(64) NOT NULL,
    UNIQUE KEY android_trials_fingerprint_unique (device_fingerprint_hash),
    UNIQUE KEY android_trials_installation_unique (installation_id),
    KEY android_trials_status_expiry_idx (status, expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;

CREATE TABLE IF NOT EXISTS android_trial_nonces (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    trial_id CHAR(36) NOT NULL,
    nonce_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY android_trial_nonces_unique (trial_id, nonce_hash),
    KEY android_trial_nonces_expiry_idx (expires_at),
    CONSTRAINT android_trial_nonces_trial_fk
        FOREIGN KEY (trial_id) REFERENCES android_trials(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;
