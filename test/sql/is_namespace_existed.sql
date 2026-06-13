-- ============================================================================
-- iceberg_catalog.is_namespace_existed 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- ============================================================================
-- 第一部分：正常场景 — 返回类型与结构校验
-- ============================================================================

-- 1. 返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.is_namespace_existed('some_ns')) AS result_type;

-- 2. 返回结构包含 "exists" key
SELECT iceberg_catalog.is_namespace_existed('some_ns') ? 'exists' AS has_exists_key;

-- ============================================================================
-- 第二部分：Namespace 不存在 — 不抛异常，返回 {"exists": false}
-- ============================================================================

-- 3. 不存在的 namespace 返回 {"exists": false}
SELECT iceberg_catalog.is_namespace_existed('non_existent_namespace');

-- 4. 验证 exists 值为 false
SELECT (iceberg_catalog.is_namespace_existed('does_not_exist') ->> 'exists')::BOOLEAN AS exists_value;

-- 5. 多次查询同一不存在的 namespace，结果一致
SELECT
    (iceberg_catalog.is_namespace_existed('repeat_ns') ->> 'exists')::BOOLEAN AS first_call,
    (iceberg_catalog.is_namespace_existed('repeat_ns') ->> 'exists')::BOOLEAN AS second_call;

-- ============================================================================
-- 第三部分：Namespace 存在 — 返回 {"exists": true}
-- ============================================================================

-- 6. 先插入 namespace，再查询
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'existing_ns', '{}'::JSONB);

SELECT iceberg_catalog.is_namespace_existed('existing_ns');

-- 7. 验证 exists 值为 true
SELECT (iceberg_catalog.is_namespace_existed('existing_ns') ->> 'exists')::BOOLEAN AS exists_value;

-- 8. 带 properties 的 namespace
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'accounting', '{"owner": "Ralph", "created_at": "1452120468"}'::JSONB);

SELECT iceberg_catalog.is_namespace_existed('accounting');

-- ============================================================================
-- 第四部分：参数校验 — 报错场景
-- ============================================================================

-- 9. p_namespace 为空字符串 → 报错 (P0001)
SAVEPOINT sp9;
SELECT iceberg_catalog.is_namespace_existed('');
ROLLBACK TO SAVEPOINT sp9;

-- 10. p_namespace 为 NULL → 报错 (P0001)
SAVEPOINT sp10;
SELECT iceberg_catalog.is_namespace_existed(NULL::TEXT);
ROLLBACK TO SAVEPOINT sp10;

-- ============================================================================
-- 第五部分：边界场景
-- ============================================================================

-- 11. 命名空间名称含特殊字符（短横线、下划线、大小写）
SELECT iceberg_catalog.is_namespace_existed('ns-with-dash');
SELECT iceberg_catalog.is_namespace_existed('ns_with_underscore');
SELECT iceberg_catalog.is_namespace_existed('NS123MixedCase');

-- 12. 较长的 namespace 名称（合法标识符范围内）
SELECT iceberg_catalog.is_namespace_existed('a_very_long_namespace_name_that_is_still_valid');

ROLLBACK;
