SET @churchdaw_add_reconciliation_attempt_columns = (
    SELECT CONCAT(
        'ALTER TABLE orders ',
        GROUP_CONCAT(definition ORDER BY ordinal SEPARATOR ', ')
    )
    FROM (
        SELECT 1 AS ordinal,
               'ADD COLUMN provider_reconcile_attempted_at DATETIME(6) NULL AFTER provider_reconciled_at' AS definition
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'provider_reconcile_attempted_at'
        )
        UNION ALL
        SELECT 2,
               'ADD COLUMN provider_reconcile_failures SMALLINT UNSIGNED NOT NULL DEFAULT 0 AFTER provider_reconcile_attempted_at'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'provider_reconcile_failures'
        )
        UNION ALL
        SELECT 3,
               'ADD COLUMN provider_reconcile_error VARCHAR(500) NULL AFTER provider_reconcile_failures'
        WHERE NOT EXISTS (
            SELECT 1 FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'orders'
              AND column_name = 'provider_reconcile_error'
        )
    ) AS missing_columns
);
SET @churchdaw_add_reconciliation_attempt_columns = IF(
    @churchdaw_add_reconciliation_attempt_columns IS NULL,
    'SELECT 1',
    @churchdaw_add_reconciliation_attempt_columns
);
PREPARE churchdaw_statement FROM @churchdaw_add_reconciliation_attempt_columns;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_reconcile_attempt_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND index_name = 'orders_reconcile_attempt_idx'
);
SET @churchdaw_add_reconcile_attempt_index = IF(
    @churchdaw_has_reconcile_attempt_index = 0,
    'ALTER TABLE orders ADD KEY orders_reconcile_attempt_idx (provider, provider_reconcile_attempted_at, created_at)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_reconcile_attempt_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
