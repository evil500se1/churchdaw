SET @churchdaw_has_provider_reconciled_at = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND column_name = 'provider_reconciled_at'
);
SET @churchdaw_add_provider_reconciled_at = IF(
    @churchdaw_has_provider_reconciled_at = 0,
    'ALTER TABLE orders ADD COLUMN provider_reconciled_at DATETIME(6) NULL AFTER provider_status',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_provider_reconciled_at;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_reconciliation_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND index_name = 'orders_reconciliation_idx'
);
SET @churchdaw_add_reconciliation_index = IF(
    @churchdaw_has_reconciliation_index = 0,
    'ALTER TABLE orders ADD KEY orders_reconciliation_idx (provider, provider_reconciled_at, created_at)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_reconciliation_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
