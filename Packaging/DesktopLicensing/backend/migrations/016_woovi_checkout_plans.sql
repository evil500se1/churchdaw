SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci;

ALTER TABLE orders
    ADD COLUMN plan_code VARCHAR(32) NULL AFTER product_code,
    ADD COLUMN activation_policy VARCHAR(32) NOT NULL
        DEFAULT 'single_device' AFTER plan_code,
    ADD COLUMN checkout_access_token_hash CHAR(64) NULL
        AFTER checkout_claimed_at,
    ADD COLUMN checkout_code MEDIUMTEXT NULL AFTER checkout_url,
    ADD COLUMN checkout_image_url TEXT NULL AFTER checkout_code,
    ADD CONSTRAINT orders_activation_policy_check CHECK (
        activation_policy IN ('single_device', 'platform_bundle')
    );

ALTER TABLE orders
    ADD KEY orders_woovi_poll_idx
        (provider, status, provider_reconciled_at, created_at);
