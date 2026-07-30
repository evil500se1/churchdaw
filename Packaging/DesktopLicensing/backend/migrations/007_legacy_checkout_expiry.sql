-- Migration 004 originally inferred a three-day expiry for preferences created
-- before expiry evidence was stored. That inference is unsafe: a boleto or Pix
-- preference can still be payable after the locally invented timestamp. Keep
-- those legacy orders open until a canonical provider reconciliation or manual
-- review decides their state.
UPDATE orders
SET checkout_expires_at = NULL
WHERE terms_version = 'legacy-unknown'
  AND privacy_version = 'legacy-unknown'
  AND checkout_url IS NOT NULL;
