-- ============================================================================
-- iceberg_catalog.list_namespaces 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- ============================================================================
-- 第一部分：正常场景 — 返回类型与结构校验
-- ============================================================================

-- 1. 默认参数调用，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.list_namespaces()) AS result_type;

-- 2. 返回结构包含 "namespaces" 和 "next-page-token" 两个顶层 key
SELECT
    iceberg_catalog.list_namespaces() ? 'namespaces'       AS has_namespaces,
    iceberg_catalog.list_namespaces() ? 'next-page-token'  AS has_next_page_token;

-- 3. "namespaces" 字段应为数组
SELECT jsonb_typeof(iceberg_catalog.list_namespaces() -> 'namespaces') AS namespaces_type;

-- 4. 首页 next-page-token 存在（stub 返回 null）
SELECT iceberg_catalog.list_namespaces() -> 'next-page-token' AS next_token;

-- ============================================================================
-- 第二部分：参数组合
-- ============================================================================

-- 5. 指定 p_parent = NULL（列出顶层 namespace，默认行为）
SELECT iceberg_catalog.list_namespaces(p_parent => NULL);

-- 6. 指定 p_page_size
SELECT iceberg_catalog.list_namespaces(p_page_size => 50);

-- 7. 使用位置参数
SELECT iceberg_catalog.list_namespaces(NULL, 100, NULL);

-- 8. 指定 p_page_token（分页）
SELECT iceberg_catalog.list_namespaces(p_page_token => 'eyJvZmZzZXQiIDogMTB9');

-- 9. 全部参数使用命名传参
SELECT iceberg_catalog.list_namespaces(
    p_parent     => 'accounting',
    p_page_size  => 20,
    p_page_token => NULL
);

-- ============================================================================
-- 第三部分：参数校验 — 报错场景
-- ============================================================================

-- 10. p_page_size = 0 → 报错 (P0001)
SAVEPOINT sp10;
SELECT iceberg_catalog.list_namespaces(p_page_size => 0);
ROLLBACK TO SAVEPOINT sp10;

-- 11. p_page_size = -1 → 报错 (P0001)
SAVEPOINT sp11;
SELECT iceberg_catalog.list_namespaces(p_page_size => -1);
ROLLBACK TO SAVEPOINT sp11;

-- ============================================================================
-- 第四部分：未实现的功能 — 报错场景 (Stub 阶段不触发，但预留)
-- ============================================================================

-- 12. TODO: p_parent 指定的父级 Namespace 不存在 → 报错 (P0004)
-- SAVEPOINT sp12;
-- SELECT iceberg_catalog.list_namespaces(p_parent => 'non_existent_parent');
-- ROLLBACK TO SAVEPOINT sp12;

-- ============================================================================
-- 第五部分：边界场景
-- ============================================================================

-- 13. p_page_size 为大值
SELECT iceberg_catalog.list_namespaces(p_page_size => 1000000);

-- 14. p_page_size = 1（最小值合法）
SELECT iceberg_catalog.list_namespaces(p_page_size => 1);

-- 15. 插入 namespace 后调用 list（stub 返回空列表，TODO：META 接入后应包含已插入的 namespace）
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'dept_a', '{}'::JSONB);

INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'dept_b', '{"owner": "alice"}'::JSONB);

SELECT iceberg_catalog.list_namespaces();

ROLLBACK;
