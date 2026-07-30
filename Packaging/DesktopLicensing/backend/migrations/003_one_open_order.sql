SET @churchdaw_has_open_slot = (
    SELECT COUNT(*)
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND column_name = 'open_slot'
);
SET @churchdaw_add_open_slot = IF(
    @churchdaw_has_open_slot = 0,
    'ALTER TABLE orders ADD COLUMN open_slot TINYINT UNSIGNED NULL AFTER status',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_open_slot;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;

CREATE TEMPORARY TABLE churchdaw_open_order_keep AS
SELECT user_id, product_code, MIN(id) AS keep_id
FROM orders
WHERE status IN ('creating', 'provider_error', 'pending')
GROUP BY user_id, product_code;

UPDATE orders AS orders_to_normalise
JOIN churchdaw_open_order_keep AS keepers
  ON keepers.user_id = orders_to_normalise.user_id
 AND keepers.product_code = orders_to_normalise.product_code
SET orders_to_normalise.open_slot =
        IF(orders_to_normalise.id = keepers.keep_id, 1, NULL),
    orders_to_normalise.status =
        IF(orders_to_normalise.id = keepers.keep_id,
           orders_to_normalise.status,
           'superseded')
WHERE orders_to_normalise.status IN ('creating', 'provider_error', 'pending');

DROP TEMPORARY TABLE churchdaw_open_order_keep;

SET @churchdaw_has_open_order_index = (
    SELECT COUNT(*)
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name = 'orders'
      AND index_name = 'orders_one_open_per_product'
);
SET @churchdaw_add_open_order_index = IF(
    @churchdaw_has_open_order_index = 0,
    'ALTER TABLE orders ADD UNIQUE KEY orders_one_open_per_product (user_id, product_code, open_slot)',
    'SELECT 1'
);
PREPARE churchdaw_statement FROM @churchdaw_add_open_order_index;
EXECUTE churchdaw_statement;
DEALLOCATE PREPARE churchdaw_statement;
