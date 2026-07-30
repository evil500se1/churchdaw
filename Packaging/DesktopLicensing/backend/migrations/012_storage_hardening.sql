-- Pin storage properties instead of inheriting mutable server/database
-- defaults. ROW_FORMAT=DYNAMIC is required for predictable utf8mb4 indexes on
-- MySQL 5.7. Foreign-key checks are disabled only for this connection while
-- parent and child tables are rebuilt to the same charset/collation.

SET @churchdaw_previous_foreign_key_checks = @@SESSION.FOREIGN_KEY_CHECKS;
SET SESSION FOREIGN_KEY_CHECKS = 0;

ALTER TABLE schema_migrations
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE users
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE user_tokens
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE products
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE orders
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE payments
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE licenses
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE devices
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE activations
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE refresh_tokens
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE request_nonces
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE license_transfers
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE webhook_events
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE rate_limits
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE web_sessions
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE email_jobs
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
ALTER TABLE audit_events
    ENGINE=InnoDB,
    ROW_FORMAT=DYNAMIC,
    CONVERT TO CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

SET SESSION FOREIGN_KEY_CHECKS = @churchdaw_previous_foreign_key_checks;

SET @churchdaw_invalid_storage_tables = (
    SELECT COUNT(*)
    FROM information_schema.tables
    WHERE table_schema = DATABASE()
      AND table_type = 'BASE TABLE'
      AND (
          engine <> 'InnoDB'
          OR row_format <> 'Dynamic'
          OR table_collation <> 'utf8mb4_unicode_ci'
      )
);
SET @churchdaw_assert_storage_tables = IF(
    @churchdaw_invalid_storage_tables = 0,
    'SELECT 1',
    'SIGNAL SQLSTATE ''45000'' SET MESSAGE_TEXT = ''Storage/charset divergente depois da migracao 012'''
);
PREPARE churchdaw_statement FROM @churchdaw_assert_storage_tables;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
