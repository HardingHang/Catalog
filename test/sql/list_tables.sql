-- ============================================================================
-- iceberg_catalog.list_tables 测试用例
-- ============================================================================

BEGIN;

-- 1. 基础调用：返回 JSONB object
SELECT jsonb_typeof(iceberg_catalog.list_tables('test_ns')) AS result_type;

-- 2. 返回结构包含顶层 key
SELECT
    iceberg_catalog.list_tables('test_ns') ? 'identifiers'     AS has_identifiers,
    iceberg_catalog.list_tables('test_ns') ? 'next-page-token' AS has_next_page_token;

-- 3. 传入分页参数
SELECT iceberg_catalog.list_tables('ns', 10, 'token_xxx');

-- 4. p_page_size = 1（边界值）
SELECT iceberg_catalog.list_tables('ns', 1);

-- 5. p_page_size = 0 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.list_tables('ns', 0);
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_namespace 为空串 → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.list_tables('');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_namespace 为 NULL → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.list_tables(NULL);
ROLLBACK TO SAVEPOINT sp7;

ROLLBACK;
