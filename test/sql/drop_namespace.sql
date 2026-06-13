-- ============================================================================
-- iceberg_catalog.drop_namespace 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- ============================================================================
-- 第一部分：正常场景 — 返回类型与结构校验
-- ============================================================================

-- 1. 返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.drop_namespace('some_ns')) AS result_type;

-- 2. 返回结构包含 "success" key，且值为 true
SELECT
    iceberg_catalog.drop_namespace('some_ns') ? 'success'              AS has_success,
    (iceberg_catalog.drop_namespace('some_ns') ->> 'success')::BOOLEAN AS success_value;

-- ============================================================================
-- 第二部分：删除已存在的 Namespace
-- ============================================================================

-- 3. 创建 namespace 后删除
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'temp_ns', '{"owner": "test"}'::JSONB);

SELECT iceberg_catalog.drop_namespace('temp_ns');

-- 4. 验证 namespace 已被删除（直接查元数据表）
SELECT count(*) = 0 AS is_deleted
FROM iceberg_catalog.namespaces
WHERE namespace = 'temp_ns';

-- 5. 创建并删除空 properties 的 namespace
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'empty_ns', '{}'::JSONB);

SELECT iceberg_catalog.drop_namespace('empty_ns');

-- ============================================================================
-- 第三部分：参数校验 — 报错场景
-- ============================================================================

-- 6. p_namespace 为空字符串 → 报错 (P0001)
SAVEPOINT sp6;
SELECT iceberg_catalog.drop_namespace('');
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_namespace 为 NULL → 报错 (P0001)
SAVEPOINT sp7;
SELECT iceberg_catalog.drop_namespace(NULL::TEXT);
ROLLBACK TO SAVEPOINT sp7;

-- ============================================================================
-- 第四部分：未实现的功能 — 报错场景 (Stub 阶段不触发，但预留)
-- ============================================================================

-- 8. TODO: Namespace 不存在 → 报错 (P0004)
-- SAVEPOINT sp8;
-- SELECT iceberg_catalog.drop_namespace('non_existent_namespace');
-- ROLLBACK TO SAVEPOINT sp8;

-- 9. TODO: Namespace 下有表 → 报错 (P0005)
-- SAVEPOINT sp9;
-- INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
-- VALUES (current_database(), 'ns_with_tables', '{}'::JSONB);
-- -- 需要先插入 tables_external 引用该 namespace
-- INSERT INTO iceberg_catalog.tables_external(catalog_name, namespace, table_name, metadata_location)
-- VALUES (current_database(), 'ns_with_tables', 'some_table', 'file:///tmp/metadata.json');
-- SELECT iceberg_catalog.drop_namespace('ns_with_tables');
-- ROLLBACK TO SAVEPOINT sp9;

-- ============================================================================
-- 第五部分：边界场景
-- ============================================================================

-- 10. 命名空间名称含特殊字符
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'ns-with-dash', '{}'::JSONB);

SELECT iceberg_catalog.drop_namespace('ns-with-dash');

-- 11. 多次删除同一 namespace（第二次应报 P0004，stub 阶段不触发）
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'once_ns', '{}'::JSONB);

SELECT iceberg_catalog.drop_namespace('once_ns');
-- SELECT iceberg_catalog.drop_namespace('once_ns');  -- TODO: 第二次应报 P0004

ROLLBACK;
