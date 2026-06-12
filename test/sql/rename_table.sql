-- ============================================================================
-- iceberg_catalog.rename_table 测试用例
-- ============================================================================

BEGIN;

-- 1. 基础调用：返回 {"success": true}
SELECT iceberg_catalog.rename_table('src_ns', 'src_tbl', 'dst_ns', 'dst_tbl');

-- 2. 同 Namespace 内重命名
SELECT iceberg_catalog.rename_table('ns', 'old_name', 'ns', 'new_name');

-- 3. 验证返回 JSONB object
SELECT jsonb_typeof(iceberg_catalog.rename_table('a', 'b', 'c', 'd')) AS result_type;

-- 4. p_src_ns 为空串 → 报错
SAVEPOINT sp4;
SELECT iceberg_catalog.rename_table('', 'src', 'dst_ns', 'dst_tbl');
ROLLBACK TO SAVEPOINT sp4;

-- 5. p_src_table 为空串 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.rename_table('src_ns', '', 'dst_ns', 'dst_tbl');
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_dst_ns 为空串 → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.rename_table('src_ns', 'src', '', 'dst_tbl');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_dst_table 为空串 → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.rename_table('src_ns', 'src', 'dst_ns', '');
ROLLBACK TO SAVEPOINT sp7;

-- 8. p_src_ns 为 NULL → 报错
SAVEPOINT sp8;
SELECT iceberg_catalog.rename_table(NULL, 'src', 'dst_ns', 'dst_tbl');
ROLLBACK TO SAVEPOINT sp8;

ROLLBACK;
