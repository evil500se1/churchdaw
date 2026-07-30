-- MySQL 5.7 parses CHECK clauses but does not enforce them. Generated columns
-- make the uniqueness slots database-derived without requiring TRIGGER
-- privilege. Existing UNIQUE indexes then serialize every competing checkout,
-- entitlement and activation.

SET @churchdaw_duplicate_open_orders = (
    SELECT COUNT(*)
    FROM (
        SELECT user_id, product_code
        FROM orders
        WHERE status IN (
                  'creating', 'provider_error', 'provider_ambiguous', 'pending'
              )
           OR (status = 'paid_review' AND open_slot = 1)
        GROUP BY user_id, product_code
        HAVING COUNT(*) > 1
    ) AS duplicate_open_orders
);
SET @churchdaw_assert_no_duplicate_open_orders = IF(
    @churchdaw_duplicate_open_orders = 0,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Pedidos abertos duplicados exigem revisao manual antes da migracao 011'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_no_duplicate_open_orders;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_duplicate_active_licenses = (
    SELECT COUNT(*)
    FROM (
        SELECT user_id, product_code
        FROM licenses
        WHERE status = 'active'
        GROUP BY user_id, product_code
        HAVING COUNT(*) > 1
    ) AS duplicate_active_licenses
);
SET @churchdaw_assert_no_duplicate_active_licenses = IF(
    @churchdaw_duplicate_active_licenses = 0,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Licencas ativas duplicadas exigem revisao manual antes da migracao 011'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_no_duplicate_active_licenses;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_duplicate_active_activations = (
    SELECT COUNT(*)
    FROM (
        SELECT license_id
        FROM activations
        WHERE status = 'active'
        GROUP BY license_id
        HAVING COUNT(*) > 1
    ) AS duplicate_active_activations
);
SET @churchdaw_assert_no_duplicate_active_activations = IF(
    @churchdaw_duplicate_active_activations = 0,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Ativacoes ativas duplicadas exigem revisao manual antes da migracao 011'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_no_duplicate_active_activations;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_checkout_reservation = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND column_name = 'reserves_checkout_slot'
);
SET @churchdaw_add_checkout_reservation = IF(
    @churchdaw_has_checkout_reservation = 0,
    'ALTER TABLE orders ADD COLUMN reserves_checkout_slot TINYINT UNSIGNED NOT NULL DEFAULT 0 AFTER status',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_checkout_reservation;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_order_slot_is_generated = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND column_name = 'open_slot'
      AND extra LIKE '%STORED GENERATED%'
);
SET @churchdaw_copy_checkout_reservation = IF(
    @churchdaw_order_slot_is_generated = 0,
    'UPDATE orders SET reserves_checkout_slot = IF(open_slot = 1, 1, 0)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_copy_checkout_reservation;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_open_order_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND index_name = 'orders_one_open_per_product'
);
SET @churchdaw_drop_open_order_index = IF(
    @churchdaw_has_open_order_index > 0,
    'ALTER TABLE orders DROP INDEX orders_one_open_per_product',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_open_order_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_order_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND column_name = 'open_slot'
);
SET @churchdaw_drop_writable_order_slot = IF(
    @churchdaw_has_order_slot = 1
        AND @churchdaw_order_slot_is_generated = 0,
    'ALTER TABLE orders DROP COLUMN open_slot',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_writable_order_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_order_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND column_name = 'open_slot'
);
SET @churchdaw_add_generated_order_slot = IF(
    @churchdaw_has_order_slot = 0,
    'ALTER TABLE orders ADD COLUMN open_slot TINYINT UNSIGNED GENERATED ALWAYS AS (CASE WHEN status IN (''creating'', ''provider_error'', ''provider_ambiguous'', ''pending'') OR (status = ''paid_review'' AND reserves_checkout_slot = 1) THEN 1 ELSE NULL END) STORED AFTER reserves_checkout_slot',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_generated_order_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_add_open_order_index = IF(
    @churchdaw_has_open_order_index = 0,
    'ALTER TABLE orders ADD UNIQUE KEY orders_one_open_per_product (user_id, product_code, open_slot)',
    'ALTER TABLE orders ADD UNIQUE KEY orders_one_open_per_product (user_id, product_code, open_slot)'
);
PREPARE churchdaw_statement FROM @churchdaw_add_open_order_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_license_active_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'licenses'
      AND index_name = 'licenses_one_active_per_product'
);
SET @churchdaw_drop_license_active_index = IF(
    @churchdaw_has_license_active_index > 0,
    'ALTER TABLE licenses DROP INDEX licenses_one_active_per_product',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_license_active_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

-- MariaDB and MySQL 8 enforce the legacy CHECK and will not let its referenced
-- column be replaced. MySQL 5.7 never exposes that parsed-but-ignored CHECK.
SET @churchdaw_has_license_active_check = (
    SELECT COUNT(*)
    FROM information_schema.table_constraints
    WHERE constraint_schema = DATABASE()
      AND table_name = 'licenses'
      AND constraint_name = 'licenses_active_slot_check'
      AND constraint_type = 'CHECK'
);
SET @churchdaw_drop_license_active_check = IF(
    @churchdaw_has_license_active_check = 0,
    'SELECT 1',
    IF(
        LOWER(VERSION()) LIKE '%mariadb%',
        'ALTER TABLE licenses DROP CONSTRAINT licenses_active_slot_check',
        'ALTER TABLE licenses DROP CHECK licenses_active_slot_check'
    )
);
PREPARE churchdaw_statement FROM @churchdaw_drop_license_active_check;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_license_slot_is_generated = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'licenses'
      AND column_name = 'active_slot'
      AND extra LIKE '%STORED GENERATED%'
);
SET @churchdaw_has_license_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'licenses'
      AND column_name = 'active_slot'
);
SET @churchdaw_drop_writable_license_slot = IF(
    @churchdaw_has_license_slot = 1
        AND @churchdaw_license_slot_is_generated = 0,
    'ALTER TABLE licenses DROP COLUMN active_slot',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_writable_license_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_license_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'licenses'
      AND column_name = 'active_slot'
);
SET @churchdaw_add_generated_license_slot = IF(
    @churchdaw_has_license_slot = 0,
    'ALTER TABLE licenses ADD COLUMN active_slot TINYINT UNSIGNED GENERATED ALWAYS AS (IF(status = ''active'', 1, NULL)) STORED AFTER status',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_generated_license_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

ALTER TABLE licenses
    ADD UNIQUE KEY licenses_one_active_per_product
        (user_id, product_code, active_slot);

SET @churchdaw_has_activation_active_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'activations'
      AND index_name = 'activations_one_active_per_license'
);

-- The composite UNIQUE currently also backs activations_license_fk on MySQL.
-- Keep a temporary plain index while replacing the generated column.
SET @churchdaw_has_activation_migration_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'activations'
      AND index_name = 'activations_license_migration_idx'
);
SET @churchdaw_add_activation_migration_index = IF(
    @churchdaw_has_activation_migration_index = 0,
    'ALTER TABLE activations ADD KEY activations_license_migration_idx (license_id)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_activation_migration_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_drop_activation_active_index = IF(
    @churchdaw_has_activation_active_index > 0,
    'ALTER TABLE activations DROP INDEX activations_one_active_per_license',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_activation_active_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_activation_slot_is_generated = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'activations'
      AND column_name = 'active_slot'
      AND extra LIKE '%STORED GENERATED%'
);
SET @churchdaw_has_activation_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'activations'
      AND column_name = 'active_slot'
);
SET @churchdaw_drop_writable_activation_slot = IF(
    @churchdaw_has_activation_slot = 1
        AND @churchdaw_activation_slot_is_generated = 0,
    'ALTER TABLE activations DROP COLUMN active_slot',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_writable_activation_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_activation_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'activations'
      AND column_name = 'active_slot'
);
SET @churchdaw_add_generated_activation_slot = IF(
    @churchdaw_has_activation_slot = 0,
    'ALTER TABLE activations ADD COLUMN active_slot TINYINT UNSIGNED GENERATED ALWAYS AS (IF(status = ''active'', 1, NULL)) STORED AFTER status',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_generated_activation_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

ALTER TABLE activations
    ADD UNIQUE KEY activations_one_active_per_license
        (license_id, active_slot);

ALTER TABLE activations
    DROP INDEX activations_license_migration_idx;

-- Clean up an unreleased trigger-based draft if a test database saw it.
DROP TRIGGER IF EXISTS churchdaw_orders_open_slot_bi;
DROP TRIGGER IF EXISTS churchdaw_orders_open_slot_bu;
DROP TRIGGER IF EXISTS churchdaw_licenses_active_slot_bi;
DROP TRIGGER IF EXISTS churchdaw_licenses_active_slot_bu;
DROP TRIGGER IF EXISTS churchdaw_activations_active_slot_bi;
DROP TRIGGER IF EXISTS churchdaw_activations_active_slot_bu;

SET @churchdaw_generated_slot_count = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND (
          (table_name = 'orders' AND column_name = 'open_slot')
          OR (table_name = 'licenses' AND column_name = 'active_slot')
          OR (table_name = 'activations' AND column_name = 'active_slot')
      )
      AND extra LIKE '%STORED GENERATED%'
);
SET @churchdaw_assert_generated_slots = IF(
    @churchdaw_generated_slot_count = 3,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Slots gerados incompletos na migracao 011'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_generated_slots;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_slot_unique_indexes = (
    SELECT COUNT(*)
    FROM (
        SELECT table_name, index_name
        FROM information_schema.statistics
        WHERE table_schema = DATABASE()
          AND non_unique = 0
          AND (
              (table_name = 'orders'
               AND index_name = 'orders_one_open_per_product')
              OR
              (table_name = 'licenses'
               AND index_name = 'licenses_one_active_per_product')
              OR
              (table_name = 'activations'
               AND index_name = 'activations_one_active_per_license')
          )
        GROUP BY table_name, index_name
    ) AS unique_slot_indexes
);
SET @churchdaw_assert_slot_unique_indexes = IF(
    @churchdaw_slot_unique_indexes = 3,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Indices UNIQUE de slot incompletos na migracao 011'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_slot_unique_indexes;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
