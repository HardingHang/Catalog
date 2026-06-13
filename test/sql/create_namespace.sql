-- ============================================================================
-- iceberg_catalog.create_namespace 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- ============================================================================
-- 第一部分：正常场景
-- ============================================================================

-- 1. 基础调用：仅填必填参数 p_namespace，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.create_namespace('test_ns')) AS result_type;

-- 2. 返回结构的顶层 key 校验：应包含 "namespace" 和 "properties"
SELECT
    iceberg_catalog.create_namespace('test_ns') ? 'namespace'  AS has_namespace,
    iceberg_catalog.create_namespace('test_ns') ? 'properties' AS has_properties;

-- 3. "namespace" 字段应为数组，且包含传入的命名空间
SELECT
    jsonb_typeof(iceberg_catalog.create_namespace('sales') -> 'namespace') AS namespace_type,
    (iceberg_catalog.create_namespace('sales') -> 'namespace' -> 0)        AS first_element;

-- 4. p_properties 传入空对象
SELECT iceberg_catalog.create_namespace('ns_empty_props', '{}'::JSONB);

-- 5. p_properties 传入自定义属性
SELECT iceberg_catalog.create_namespace(
    'accounting',
    '{"owner": "Ralph", "created_at": "1452120468"}'::JSONB
);

-- 6. 使用命名参数调用
SELECT iceberg_catalog.create_namespace(
    p_namespace => 'hr_dept',
    p_properties => '{"region": "us-east-1"}'::JSONB
);

-- 7. p_properties 显式传入 NULL（等价于不传）
SELECT iceberg_catalog.create_namespace('nullable_props', NULL::JSONB);

-- ============================================================================
-- 第二部分：参数校验 — 报错场景
-- ============================================================================

-- 8. p_namespace 为空字符串 → 报错 (P0001)
SAVEPOINT sp8;
SELECT iceberg_catalog.create_namespace('', '{"key":"val"}'::JSONB);
ROLLBACK TO SAVEPOINT sp8;

-- 9. p_namespace 为 NULL → 报错 (P0001)
SAVEPOINT sp9;
SELECT iceberg_catalog.create_namespace(NULL::TEXT, '{}'::JSONB);
ROLLBACK TO SAVEPOINT sp9;

-- 10. p_properties 为 JSONB string（非 object） → 报错 (P0001)
SAVEPOINT sp10;
SELECT iceberg_catalog.create_namespace('ns', '"not_an_object"'::JSONB);
ROLLBACK TO SAVEPOINT sp10;

-- 11. p_properties 为 JSONB array（非 object） → 报错 (P0001)
SAVEPOINT sp11;
SELECT iceberg_catalog.create_namespace('ns', '["array"]'::JSONB);
ROLLBACK TO SAVEPOINT sp11;

-- 12. p_properties 为 JSONB number（非 object） → 报错 (P0001)
SAVEPOINT sp12;
SELECT iceberg_catalog.create_namespace('ns', '42'::JSONB);
ROLLBACK TO SAVEPOINT sp12;

-- ============================================================================
-- 第三部分：未实现的功能 — 报错场景 (Stub 阶段不触发，但预留)
-- ============================================================================

-- 13. TODO: 重复创建同一 namespace → 报错 (P0005)
-- SAVEPOINT sp13;
-- SELECT iceberg_catalog.create_namespace('dup_ns');
-- SELECT iceberg_catalog.create_namespace('dup_ns');  -- 第二次应报 P0005
-- ROLLBACK TO SAVEPOINT sp13;

-- ============================================================================
-- 第四部分：边界场景
-- ============================================================================

-- 14. 命名空间名称为特殊字符（合法标识符）
SELECT iceberg_catalog.create_namespace('ns-with-dash');
SELECT iceberg_catalog.create_namespace('ns_with_underscore');
SELECT iceberg_catalog.create_namespace('NS123MixedCase');

-- 15. properties 中包含多层嵌套对象
SELECT iceberg_catalog.create_namespace(
    'nested_ns',
    '{"env":"prod","config":{"replicas":3,"tags":{"team":"platform","cost":"low"}}}'::JSONB
);

-- 16. properties 中包含数组
SELECT iceberg_catalog.create_namespace(
    'arr_ns',
    '{"owners":["alice","bob"],"regions":["us","eu"]}'::JSONB
);

ROLLBACK;
