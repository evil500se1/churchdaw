SET @churchdaw_add_order_columns = (
    SELECT CONCAT(
        'ALTER TABLE orders ',
        GROUP_CONCAT(definition ORDER BY ordinal SEPARATOR ', ')
    )
    FROM (
        SELECT 1 AS ordinal,
               'ADD COLUMN checkout_claim_token CHAR(36) NULL AFTER open_slot' AS definition
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'checkout_claim_token'
        )
        UNION ALL
        SELECT 2, 'ADD COLUMN checkout_claimed_at DATETIME(6) NULL AFTER checkout_claim_token'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'checkout_claimed_at'
        )
        UNION ALL
        SELECT 3, 'ADD COLUMN checkout_expires_at DATETIME(6) NULL AFTER checkout_claimed_at'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'checkout_expires_at'
        )
        UNION ALL
        SELECT 4, 'ADD COLUMN provider_order_id VARCHAR(128) NULL AFTER provider_preference_id'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'provider_order_id'
        )
        UNION ALL
        SELECT 5, 'ADD COLUMN terms_version VARCHAR(32) NULL AFTER checkout_expires_at'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'terms_version'
        )
        UNION ALL
        SELECT 6, 'ADD COLUMN privacy_version VARCHAR(32) NULL AFTER terms_version'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'privacy_version'
        )
        UNION ALL
        SELECT 7, 'ADD COLUMN accepted_at DATETIME(6) NULL AFTER privacy_version'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'accepted_at'
        )
        UNION ALL
        SELECT 8, 'ADD COLUMN acceptance_ip_hash CHAR(64) NULL AFTER accepted_at'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'acceptance_ip_hash'
        )
    ) AS missing_columns
);
SET @churchdaw_add_order_columns = IF(
    @churchdaw_add_order_columns IS NULL,
    'SELECT 1',
    @churchdaw_add_order_columns
);
PREPARE churchdaw_statement FROM @churchdaw_add_order_columns;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_provider_order_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND index_name = 'orders_provider_order_unique'
);
SET @churchdaw_add_provider_order_index = IF(
    @churchdaw_has_provider_order_index = 0,
    'ALTER TABLE orders ADD UNIQUE KEY orders_provider_order_unique (provider, provider_order_id)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_provider_order_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

UPDATE orders
SET terms_version = COALESCE(terms_version, 'legacy-unknown'),
    privacy_version = COALESCE(privacy_version, 'legacy-unknown'),
    accepted_at = COALESCE(accepted_at, created_at),
    acceptance_ip_hash = COALESCE(
        acceptance_ip_hash,
        SHA2(CONCAT('legacy:', id), 256)
    );

ALTER TABLE orders
    MODIFY terms_version VARCHAR(32) NOT NULL,
    MODIFY privacy_version VARCHAR(32) NOT NULL,
    MODIFY accepted_at DATETIME(6) NOT NULL,
    MODIFY acceptance_ip_hash CHAR(64) NOT NULL;

SET @churchdaw_has_refunded_amount = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'payments'
      AND column_name = 'refunded_amount_cents'
);
SET @churchdaw_add_refunded_amount = IF(
    @churchdaw_has_refunded_amount = 0,
    'ALTER TABLE payments ADD COLUMN refunded_amount_cents INT UNSIGNED NOT NULL DEFAULT 0 AFTER amount_cents',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_refunded_amount;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_license_active_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'licenses'
      AND column_name = 'active_slot'
);
SET @churchdaw_add_license_active_slot = IF(
    @churchdaw_has_license_active_slot = 0,
    'ALTER TABLE licenses ADD COLUMN active_slot TINYINT UNSIGNED NULL AFTER status',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_license_active_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

UPDATE licenses
SET active_slot = IF(status = 'active', 1, NULL);

SET @churchdaw_duplicate_entitlements = (
    SELECT COUNT(*)
    FROM (
        SELECT user_id, product_code
        FROM licenses
        WHERE active_slot = 1
        GROUP BY user_id, product_code
        HAVING COUNT(*) > 1
    ) AS duplicates
);
SET @churchdaw_assert_no_duplicate_entitlements = IF(
    @churchdaw_duplicate_entitlements = 0,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Licencas ativas duplicadas exigem revisao manual antes da migracao 004'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_no_duplicate_entitlements;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_license_active_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'licenses'
      AND index_name = 'licenses_one_active_per_product'
);
SET @churchdaw_add_license_active_index = IF(
    @churchdaw_has_license_active_index = 0,
    'ALTER TABLE licenses ADD UNIQUE KEY licenses_one_active_per_product (user_id, product_code, active_slot)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_license_active_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_license_active_check = (
    SELECT COUNT(*)
    FROM information_schema.table_constraints
    WHERE constraint_schema = DATABASE()
      AND table_name = 'licenses'
      AND constraint_name = 'licenses_active_slot_check'
);
SET @churchdaw_add_license_active_check = IF(
    @churchdaw_has_license_active_check = 0,
    'ALTER TABLE licenses ADD CONSTRAINT licenses_active_slot_check CHECK ((status = ''active'' AND active_slot IS NOT NULL AND active_slot = 1) OR (status <> ''active'' AND active_slot IS NULL))',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_license_active_check;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
