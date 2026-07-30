SET @churchdaw_has_csrf_ciphertext = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'web_sessions'
      AND column_name = 'csrf_ciphertext'
);
SET @churchdaw_add_csrf_ciphertext = IF(
    @churchdaw_has_csrf_ciphertext = 0,
    'ALTER TABLE web_sessions ADD COLUMN csrf_ciphertext TEXT NULL AFTER csrf_hash',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_csrf_ciphertext;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

-- Sessões anteriores não têm o token CSRF recuperável. Invalidá-las é mais
-- seguro do que inventar um valor; o usuário apenas precisará entrar novamente.
DELETE FROM web_sessions WHERE csrf_ciphertext IS NULL;
ALTER TABLE web_sessions MODIFY csrf_ciphertext TEXT NOT NULL;

SET @churchdaw_has_processing_started_at = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'webhook_events'
      AND column_name = 'processing_started_at'
);
SET @churchdaw_add_processing_started_at = IF(
    @churchdaw_has_processing_started_at = 0,
    'ALTER TABLE webhook_events ADD COLUMN processing_started_at DATETIME(6) NULL AFTER attempts',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_processing_started_at;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
