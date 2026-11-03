SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci;

ALTER TABLE licenses
    ADD COLUMN activation_policy VARCHAR(32) NOT NULL
        DEFAULT 'single_device' AFTER max_active_devices;

ALTER TABLE licenses
    ADD CONSTRAINT licenses_activation_policy_check CHECK (
        (activation_policy = 'single_device' AND max_active_devices = 1)
        OR
        (activation_policy = 'platform_bundle' AND max_active_devices = 3)
    );

ALTER TABLE devices
    DROP INDEX devices_installation_unique,
    ADD UNIQUE KEY devices_platform_installation_unique
        (license_id, platform, installation_id);

ALTER TABLE activations
    ADD COLUMN platform VARCHAR(24) NULL AFTER device_id;

UPDATE activations a
JOIN devices d ON d.id = a.device_id
SET a.platform = d.platform;

ALTER TABLE activations
    MODIFY COLUMN platform VARCHAR(24) NOT NULL;

ALTER TABLE activations
    ADD KEY activations_license_status_idx (license_id, status);

ALTER TABLE activations
    DROP INDEX activations_one_active_per_license;

ALTER TABLE activations
    ADD COLUMN active_platform VARCHAR(24)
        GENERATED ALWAYS AS (
            IF(status = 'active', platform, NULL)
        ) STORED AFTER active_slot,
    ADD UNIQUE KEY activations_one_active_per_platform
        (license_id, active_platform),
    ADD CONSTRAINT activations_platform_check CHECK (
        platform IN ('windows', 'macos', 'android')
    );
