-- ============================================================================
-- iceberg_catalog.load_namespace 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- ============================================================================
-- 第一部分：正常场景 — 返回类型与结构校验
-- ============================================================================

-- 1. 返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.load_namespace('test_ns')) AS result_type;

-- 2. 返回结构包含 "namespace" 和 "properties" 两个顶层 key
SELECT
    iceberg_catalog.load_namespace('test_ns') ? 'namespace'  AS has_namespace,
    iceberg_catalog.load_namespace('test_ns') ? 'properties' AS has_properties;

-- 3. "namespace" 字段应为数组，且包含传入的命名空间
SELECT
    jsonb_typeof(iceberg_catalog.load_namespace('sales') -> 'namespace') AS namespace_type,
    (iceberg_catalog.load_namespace('sales') -> 'namespace' -> 0)        AS first_element;

-- ============================================================================
-- 第二部分：Namespace 存在 — 加载元数据
-- ============================================================================

-- 4. 先插入 namespace，再查询
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'accounting', '{"owner": "Ralph", "created_at": "1452120468"}'::JSONB);

SELECT iceberg_catalog.load_namespace('accounting');

-- 5. 带空 properties 的 namespace
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'empty_props_ns', '{}'::JSONB);

SELECT iceberg_catalog.load_namespace('empty_props_ns');

-- ============================================================================
-- 第三部分：参数校验 — 报错场景
-- ============================================================================

-- 6. p_namespace 为空字符串 → 报错 (P0001)
SAVEPOINT sp6;
SELECT iceberg_catalog.load_namespace('');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_namespace 为 NULL → 报错 (P0001)
SAVEPOINT sp7;
SELECT iceberg_catalog.load_namespace(NULL::TEXT);
ROLLBACK TO SAVEPOINT sp7;

-- ============================================================================
-- 第四部分：未实现的功能 — 报错场景 (Stub 阶段不触发，但预留)
-- ============================================================================

-- 8. TODO: Namespace 不存在 → 报错 (P0004)
-- SAVEPOINT sp8;
-- SELECT iceberg_catalog.load_namespace('non_existent_namespace');
-- ROLLBACK TO SAVEPOINT sp8;

-- ============================================================================
-- 第五部分：边界场景
-- ============================================================================

-- 9. 命名空间名称含特殊字符（短横线、下划线、大小写）
SELECT iceberg_catalog.load_namespace('ns-with-dash');
SELECT iceberg_catalog.load_namespace('ns_with_underscore');
SELECT iceberg_catalog.load_namespace('NS123MixedCase');

-- 10. c## 模式前缀命名空间
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'c##special', '{"env": "test"}'::JSONB);

SELECT iceberg_catalog.load_namespace('c##special');

ROLLBACK;
