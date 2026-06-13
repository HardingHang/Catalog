-- ============================================================================
-- iceberg_catalog.create_table 测试用例
--
-- 前置条件：iceberg_catalog 扩展已安装
-- ============================================================================

BEGIN;

-- TODO: Replace direct metadata table setup with create_namespace once exposed.
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES
    (current_database(), 'test_ns', '{}'::JSONB),
    (current_database(), 'ns_full', '{}'::JSONB),
    (current_database(), 'ns_stage', '{}'::JSONB),
    (current_database(), 'ns_props', '{}'::JSONB);

-- 1. 基础调用：仅填 3 个必填参数，返回合法 JSONB
SELECT jsonb_typeof(iceberg_catalog.create_table(
    'test_ns',
    'test_tbl_basic',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true},{"id":2,"name":"data","type":"string","required":false}]}'::JSONB
)) AS result_type;

-- 2. 返回结构包含三个顶层 key
WITH result AS (
    SELECT iceberg_catalog.create_table(
        'test_ns',
        'test_tbl_keys',
        '{"type":"struct","fields":[]}'::JSONB
    ) AS value
)
SELECT value ? 'metadata-location' AS has_metadata_location,
       value ? 'metadata'          AS has_metadata,
       value ? 'config'            AS has_config
FROM result;

-- 3. 传入全部 8 个参数
SELECT iceberg_catalog.create_table(
    'ns_full',
    'tbl_full',
    '{"type":"struct","fields":[{"id":1,"name":"col1","type":"string","required":false}]}'::JSONB,
    's3://bucket/path/to/table'::TEXT,
    '{"spec-id":0,"fields":[]}'::JSONB,
    '{"order-id":0,"fields":[]}'::JSONB,
    FALSE,
    '{"owner":"test"}'::JSONB
) AS create_result;

-- 4. 检查 create_table 写入的元信息表数据
SELECT count(*) = 1 AS has_table_head,
       bool_and(metadata_location IS NOT NULL AND length(metadata_location) > 0) AS has_metadata_location,
       bool_and(previous_metadata_location IS NULL) AS previous_metadata_location_is_null,
       bool_and(table_location = 's3://bucket/path/to/table') AS table_location_matches,
       bool_and(last_column_id = 1) AS last_column_id_matches,
       bool_and(current_schema_id = 0) AS current_schema_id_matches,
       bool_and(default_spec_id = 0) AS default_spec_id_matches
FROM iceberg_catalog.tables_internal
WHERE namespace = 'ns_full'
  AND table_name = 'tbl_full';

SELECT count(*) = 1 AS has_schema_field,
       bool_and(field_position = 0) AS schema_position_matches,
       bool_and(field_id = 1) AS schema_field_id_matches,
       bool_and(field_name = 'col1') AS schema_field_name_matches,
       bool_and(field_required = FALSE) AS schema_required_matches,
       bool_and(field_type = 'string') AS schema_type_matches
FROM iceberg_catalog.table_schemas s
JOIN iceberg_catalog.tables_internal t
  ON s.table_uuid = t.table_uuid
WHERE t.namespace = 'ns_full'
  AND t.table_name = 'tbl_full';

SELECT count(*) = 1 AS has_partition_spec,
       bool_and(field_position = -1) AS partition_position_matches,
       bool_and(field_id IS NULL) AS partition_field_id_is_null,
       bool_and(source_id IS NULL) AS partition_source_id_is_null,
       bool_and(field_name IS NULL) AS partition_field_name_is_null,
       bool_and(transform IS NULL) AS partition_transform_is_null
FROM iceberg_catalog.partition_specs p
JOIN iceberg_catalog.tables_internal t
  ON p.table_uuid = t.table_uuid
WHERE t.namespace = 'ns_full'
  AND t.table_name = 'tbl_full';

-- 4. p_stage_create = TRUE（暂存创建）
SELECT iceberg_catalog.create_table(
    'ns_stage',
    'tbl_stage',
    '{"type":"struct","fields":[]}'::JSONB,
    p_stage_create => TRUE
);

-- 5. p_namespace 为空串 → 报错
SAVEPOINT sp5;
SELECT iceberg_catalog.create_table('', 'tbl', '{"type":"struct","fields":[]}'::JSONB);
ROLLBACK TO SAVEPOINT sp5;

-- 6. p_table_name 为空串 → 报错
SAVEPOINT sp6;
SELECT iceberg_catalog.create_table('test_ns', '', '{"type":"struct","fields":[]}'::JSONB);
ROLLBACK TO SAVEPOINT sp6;

-- 7. p_schema 为 NULL → 报错
SAVEPOINT sp7;
SELECT iceberg_catalog.create_table('test_ns', 'tbl_null_schema', NULL::JSONB);
ROLLBACK TO SAVEPOINT sp7;

-- 8. p_properties 传入空对象
SELECT iceberg_catalog.create_table(
    'ns_props',
    'tbl_props',
    '{"type":"struct","fields":[]}'::JSONB,
    p_properties => '{}'::JSONB
);

ROLLBACK;
