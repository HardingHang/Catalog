-- ============================================================================
-- iceberg_catalog.update_namespace_properties 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- ============================================================================
-- 第一部分：正常场景 — 返回类型与结构校验
-- ============================================================================

-- 1. 仅使用 p_updates，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.update_namespace_properties(
    'test_ns',
    p_updates => '{"owner": "alice"}'::JSONB
)) AS result_type;

-- 2. 仅使用 p_removals，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.update_namespace_properties(
    'test_ns',
    p_removals => '["deprecated_key"]'::JSONB
)) AS result_type;

-- ============================================================================
-- 第二部分：正常操作 — 更新已有 Namespace 属性
-- ============================================================================

-- 3. 创建 namespace 后更新属性
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'updatable_ns', '{"owner": "bob", "region": "us"}'::JSONB);

SELECT iceberg_catalog.update_namespace_properties(
    'updatable_ns',
    p_updates => '{"owner": "carol", "env": "prod"}'::JSONB
);

-- 4. 删除属性
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'removable_ns', '{"owner": "dave", "temp": "x", "region": "eu"}'::JSONB);

SELECT iceberg_catalog.update_namespace_properties(
    'removable_ns',
    p_removals => '["temp"]'::JSONB
);

-- 5. 同时更新和删除
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'combined_ns', '{"a": "1", "b": "2", "c": "3"}'::JSONB);

SELECT iceberg_catalog.update_namespace_properties(
    'combined_ns',
    p_removals => '["a"]'::JSONB,
    p_updates  => '{"b": "updated", "d": "new"}'::JSONB
);

-- ============================================================================
-- 第三部分：参数校验 — 报错场景
-- ============================================================================

-- 6. p_namespace 为空字符串 → 报错 (P0001)
SAVEPOINT sp6;
SELECT iceberg_catalog.update_namespace_properties(
    '',
    p_updates => '{"key": "val"}'::JSONB
);
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_namespace 为 NULL → 报错 (P0001)
SAVEPOINT sp7;
SELECT iceberg_catalog.update_namespace_properties(
    NULL::TEXT,
    p_updates => '{"key": "val"}'::JSONB
);
ROLLBACK TO SAVEPOINT sp7;

-- 8. p_removals 和 p_updates 同时为 NULL → 报错 (P0001)
SAVEPOINT sp8;
SELECT iceberg_catalog.update_namespace_properties('ns');
ROLLBACK TO SAVEPOINT sp8;

-- ============================================================================
-- 第四部分：未实现的功能 — 报错场景 (Stub 阶段不触发，但预留)
-- ============================================================================

-- 9. TODO: p_removals 不是 JSONB 数组 → 报错 (P0001)
-- SAVEPOINT sp9;
-- SELECT iceberg_catalog.update_namespace_properties(
--     'ns',
--     p_removals => '"not_an_array"'::JSONB
-- );
-- ROLLBACK TO SAVEPOINT sp9;

-- 10. TODO: p_updates 不是 JSONB object → 报错 (P0001)
-- SAVEPOINT sp10;
-- SELECT iceberg_catalog.update_namespace_properties(
--     'ns',
--     p_updates => '"not_an_object"'::JSONB
-- );
-- ROLLBACK TO SAVEPOINT sp10;

-- 11. TODO: removals ∩ updates ≠ ∅ → 报错 (P0006)
-- SAVEPOINT sp11;
-- SELECT iceberg_catalog.update_namespace_properties(
--     'ns',
--     p_removals => '["same_key"]'::JSONB,
--     p_updates  => '{"same_key": "val"}'::JSONB
-- );
-- ROLLBACK TO SAVEPOINT sp11;

-- ============================================================================
-- 第五部分：边界场景
-- ============================================================================

-- 12. p_removals 为空数组（合法，无可删除的 key）
SELECT iceberg_catalog.update_namespace_properties(
    'ns',
    p_removals => '[]'::JSONB
);

-- 13. p_updates 为空对象（合法，无更新的 key）
SELECT iceberg_catalog.update_namespace_properties(
    'ns',
    p_updates => '{}'::JSONB
);

-- 14. 使用位置参数
SELECT iceberg_catalog.update_namespace_properties(
    'positional_ns',
    '["x"]'::JSONB,
    '{"y": "z"}'::JSONB
);

ROLLBACK;
