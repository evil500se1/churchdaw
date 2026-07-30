CREATE TABLE IF NOT EXISTS email_jobs (
    id CHAR(36) PRIMARY KEY,
    type VARCHAR(32) NOT NULL,
    dedupe_key CHAR(64) NOT NULL,
    recipient_ciphertext TEXT NOT NULL,
    subject_ciphertext TEXT NOT NULL,
    body_ciphertext MEDIUMTEXT NOT NULL,
    status VARCHAR(24) NOT NULL DEFAULT 'queued',
    attempts SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    available_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    processing_started_at DATETIME(6) NULL,
    last_error VARCHAR(500) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    sent_at DATETIME(6) NULL,
    UNIQUE KEY email_jobs_dedupe_unique (dedupe_key),
    KEY email_jobs_queue_idx (status, available_at, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC;
