SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS admin_sessions (
    id CHAR(36) PRIMARY KEY,
    token_hash CHAR(64) NOT NULL,
    csrf_hash CHAR(64) NOT NULL,
    csrf_ciphertext TEXT NOT NULL,
    flash_ciphertext MEDIUMTEXT NULL,
    ip_hash CHAR(64) NOT NULL,
    user_agent_hash CHAR(64) NOT NULL,
    expires_at DATETIME(6) NOT NULL,
    last_seen_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    revoked_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    UNIQUE KEY admin_sessions_token_unique (token_hash),
    KEY admin_sessions_expiry_idx (expires_at, revoked_at)
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_unicode_ci
  ROW_FORMAT=DYNAMIC;

