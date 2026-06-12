-- ============================================================================
-- create_table_metadata.sql
--
-- Integration test for iceberg_catalog.create_table.
--
-- Tests the end-to-end flow:
--   1. Register a namespace
--   2. Call create_table with a full set of parameters
--   3. Verify the table head record in tables_internal
--   4. Verify expanded schema fields in table_schemas
--   5. Verify expanded partition spec in partition_specs
--
-- NOTE: this test must be run in a fresh transaction or after cleaning up
-- any previous test_ns / test_tbl records (storage relations in
-- iceberg_catalog must also be dropped between runs).
-- ============================================================================

-- Step 1: create a namespace to hold the table
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES (current_database(), 'test_ns', '{}'::jsonb);

-- Step 2: create the table (returns LoadTableResult JSONB)
SELECT iceberg_catalog.create_table(
    'test_ns',
    'test_tbl',
    '{"type":"struct","fields":[{"id":1,"name":"id","type":"long","required":true},{"id":2,"name":"data","type":"string","required":false}]}'::jsonb,
    'file:///tmp/warehouse/test_ns/test_tbl'::text,
    '{"spec-id":0,"fields":[]}'::jsonb,
    NULL::jsonb,
    false,
    '{"owner":"codex"}'::jsonb
) AS create_result;

-- Step 3: verify the table head record
SELECT namespace,
       table_name,
       metadata_location,
       table_location,
       last_column_id,
       current_schema_id,
       default_spec_id
FROM iceberg_catalog.tables_internal
WHERE namespace = 'test_ns'
  AND table_name = 'test_tbl';

-- Step 4: verify the expanded schema fields
SELECT field_position,
       field_id,
       field_name,
       field_required,
       field_type
FROM iceberg_catalog.table_schemas s
JOIN iceberg_catalog.tables_internal t
  ON s.table_uuid = t.table_uuid
WHERE t.namespace = 'test_ns'
  AND t.table_name = 'test_tbl'
ORDER BY field_position;

-- Step 5: verify the partition spec (unpartitioned → sentinel with -1)
SELECT field_position,
       field_id,
       source_id,
       field_name,
       transform
FROM iceberg_catalog.partition_specs p
JOIN iceberg_catalog.tables_internal t
  ON p.table_uuid = t.table_uuid
WHERE t.namespace = 'test_ns'
  AND t.table_name = 'test_tbl'
ORDER BY field_position;
