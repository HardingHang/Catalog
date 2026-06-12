-- ============================================================================
-- iceberg_catalog.drop_table 测试用例
-- ============================================================================

BEGIN;

-- 1. 基础调用：返回 {"success": true}
SELECT iceberg_catalog.drop_table('test_ns', 'test_tbl');

-- 2. p_purge = FALSE（显式传入）
SELECT iceberg_catalog.drop_table('ns', 'tbl', FALSE);

-- 3. p_purge = TRUE → 报错（暂不支持）
SAVEPOINT sp3;
SELECT iceberg_catalog.drop_table('ns', 'tbl', TRUE);
ROLLBACK TO SAVEPOINT sp3;

-- 4. p_namespace 为空串 → 报错
SAVEPOINT sp4;
SELECT iceberg_catalog.drop_table('', 'tbl');
ROLLBACK TO SAVEPOINT sp4;

-- 5. p_table 为空串 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.drop_table('ns', '');
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_namespace 为 NULL → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.drop_table(NULL, 'tbl');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_table 为 NULL → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.drop_table('ns', NULL);
ROLLBACK TO SAVEPOINT sp7;

ROLLBACK;
