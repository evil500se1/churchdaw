SET @churchdaw_has_old_license_active_check = (
    SELECT COUNT(*)
    FROM information_schema.table_constraints
    WHERE constraint_schema = DATABASE()
      AND table_name = 'licenses'
      AND constraint_name = 'licenses_active_slot_check'
);
SET @churchdaw_drop_old_license_active_check = IF(
    @churchdaw_has_old_license_active_check = 1,
    'ALTER TABLE licenses DROP CONSTRAINT licenses_active_slot_check',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_drop_old_license_active_check;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

ALTER TABLE licenses
    ADD CONSTRAINT licenses_active_slot_check CHECK (
        (status = 'active' AND active_slot IS NOT NULL AND active_slot = 1)
        OR (status <> 'active' AND active_slot IS NULL)
    );
