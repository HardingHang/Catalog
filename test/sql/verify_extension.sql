BEGIN;

-- Verify external catalog records and namespace property expansion.
INSERT INTO iceberg_catalog.namespaces(catalog_name, namespace, properties)
VALUES ('external_demo', 'demo_ns', '{"env":"test","owner":"catalog"}'::jsonb);

INSERT INTO iceberg_catalog.tables_external(
    catalog_name,
    namespace,
    table_name,
    metadata_location,
    previous_metadata_location
)
VALUES (
    'external_demo',
    'demo_ns',
    'demo_tbl',
    'file:///tmp/v2.metadata.json',
    'file:///tmp/v1.metadata.json'
);

UPDATE iceberg_catalog.tables_external
SET metadata_location = 'file:///tmp/v3.metadata.json',
    previous_metadata_location = 'file:///tmp/v2.metadata.json'
WHERE catalog_name = 'external_demo'
  AND namespace = 'demo_ns'
  AND table_name = 'demo_tbl';

SELECT
    catalog_name,
    table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.iceberg_tables
WHERE table_namespace = 'demo_ns';

SELECT
    namespace,
    property_key,
    property_value
FROM iceberg_catalog.iceberg_namespace_properties
WHERE catalog_name = 'external_demo'
  AND namespace = 'demo_ns'
ORDER BY property_key;

-- Verify internal catalog records and dependent metadata tables.
CREATE TABLE gv_catalog_verify_rel(id int);

INSERT INTO iceberg_catalog.tables_internal(
    relid,
    namespace,
    table_name,
    table_uuid,
    metadata_location,
    previous_metadata_location,
    table_location,
    last_column_id,
    current_schema_id,
    current_snapshot_id,
    default_spec_id
)
VALUES (
    'gv_catalog_verify_rel'::regclass,
    'internal_ns',
    'internal_tbl',
    '11111111-1111-1111-1111-111111111111',
    'file:///tmp/internal/v2.metadata.json',
    'file:///tmp/internal/v1.metadata.json',
    'file:///tmp/internal',
    2,
    0,
    10,
    0
);

INSERT INTO iceberg_catalog.table_schemas(
    table_uuid,
    schema_id,
    field_position,
    field_id,
    field_name,
    field_required,
    field_type,
    field_doc
)
VALUES
    -- The unpartitioned spec is stored as a sentinel row.
    (
        '11111111-1111-1111-1111-111111111111',
        0,
        0,
        1,
        'id',
        true,
        'int',
        'primary id'
    ),
    (
        '11111111-1111-1111-1111-111111111111',
        0,
        1,
        2,
        'data',
        false,
        'string',
        NULL
    );

INSERT INTO iceberg_catalog.snapshots(
    table_uuid,
    snapshot_id,
    schema_id,
    timestamp_ms,
    manifest_list,
    total_records
)
VALUES (
    '11111111-1111-1111-1111-111111111111',
    10,
    0,
    1710000000000,
    'file:///tmp/internal/snap-10.avro',
    42
);

INSERT INTO iceberg_catalog.partition_specs(
    table_uuid,
    spec_id,
    field_position,
    field_id,
    source_id,
    field_name,
    transform
)
VALUES
    (
        '11111111-1111-1111-1111-111111111111',
        0,
        -1,
        NULL,
        NULL,
        NULL,
        NULL
    ),
    (
        '11111111-1111-1111-1111-111111111111',
        1,
        0,
        1000,
        2,
        'data_bucket',
        'bucket[16]'
    );

SELECT
    catalog_name,
    table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.iceberg_tables
WHERE table_namespace = 'internal_ns';

SELECT count(*) AS schema_field_count
FROM iceberg_catalog.table_schemas
WHERE table_uuid = '11111111-1111-1111-1111-111111111111';

SELECT count(*) AS snapshot_count
FROM iceberg_catalog.snapshots
WHERE table_uuid = '11111111-1111-1111-1111-111111111111';

SELECT count(*) AS partition_spec_count
FROM iceberg_catalog.partition_specs
WHERE table_uuid = '11111111-1111-1111-1111-111111111111';

-- Verify dependent metadata rows are removed with the internal table head.
DELETE FROM iceberg_catalog.tables_internal
WHERE table_uuid = '11111111-1111-1111-1111-111111111111';

SELECT
    (SELECT count(*)
     FROM iceberg_catalog.table_schemas
     WHERE table_uuid = '11111111-1111-1111-1111-111111111111') AS schema_field_count,
    (SELECT count(*)
     FROM iceberg_catalog.snapshots
     WHERE table_uuid = '11111111-1111-1111-1111-111111111111') AS snapshot_count,
    (SELECT count(*)
     FROM iceberg_catalog.partition_specs
     WHERE table_uuid = '11111111-1111-1111-1111-111111111111') AS partition_spec_count;

DELETE FROM iceberg_catalog.tables_external
WHERE catalog_name = 'external_demo'
  AND namespace = 'demo_ns'
  AND table_name = 'demo_tbl';

SELECT count(*)
FROM iceberg_catalog.tables_external
WHERE catalog_name = 'external_demo'
  AND namespace = 'demo_ns'
  AND table_name = 'demo_tbl';

ROLLBACK;
