SET @churchdaw_has_webhook_next_attempt = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'webhook_events'
      AND column_name = 'next_attempt_at'
);
SET @churchdaw_add_webhook_next_attempt = IF(
    @churchdaw_has_webhook_next_attempt = 0,
    'ALTER TABLE webhook_events ADD COLUMN next_attempt_at DATETIME(6) NULL DEFAULT CURRENT_TIMESTAMP(6) AFTER attempts',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_webhook_next_attempt;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

SET @churchdaw_has_old_webhook_queue_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'webhook_events'
      AND index_name = 'webhook_events_queue_idx'
);
SET @churchdaw_drop_old_webhook_queue_index = IF(
    @churchdaw_has_old_webhook_queue_index > 0,
    'ALTER TABLE webhook_events DROP INDEX webhook_events_queue_idx',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_old_webhook_queue_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

ALTER TABLE webhook_events
    ADD KEY webhook_events_queue_idx (status, next_attempt_at, received_at);
