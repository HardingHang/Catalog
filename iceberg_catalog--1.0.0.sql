-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION iceberg_catalog" to load this file. \quit

CREATE SCHEMA iceberg_catalog;

CREATE TABLE iceberg_catalog.namespaces (
    catalog_name TEXT NOT NULL,
    namespace    TEXT NOT NULL,
    properties   JSONB NOT NULL DEFAULT '{}'::JSONB,
    PRIMARY KEY (catalog_name, namespace),
    CHECK (jsonb_typeof(properties) = 'object')
);

CREATE TABLE iceberg_catalog.tables_internal (
    relid                      REGCLASS NOT NULL,
    namespace                  TEXT NOT NULL,
    table_name                 TEXT NOT NULL,
    table_uuid                 UUID NOT NULL,
    metadata_location          TEXT NOT NULL,
    previous_metadata_location TEXT,
    table_location             TEXT NOT NULL,
    last_column_id             INT NOT NULL,
    current_schema_id          INT,
    current_snapshot_id        BIGINT,
    default_spec_id            INT,
    PRIMARY KEY (namespace, table_name),
    UNIQUE (table_uuid),
    UNIQUE (relid)
);

CREATE TABLE iceberg_catalog.table_schemas (
    table_uuid       UUID NOT NULL,
    schema_id        INT NOT NULL,
    field_position   INT NOT NULL,
    field_id         INT NOT NULL,
    field_name       TEXT NOT NULL,
    field_required   BOOLEAN NOT NULL,
    field_type       TEXT NOT NULL,
    field_doc        TEXT,
    PRIMARY KEY (table_uuid, schema_id, field_position),
    UNIQUE (table_uuid, schema_id, field_id),
    FOREIGN KEY (table_uuid)
        REFERENCES iceberg_catalog.tables_internal(table_uuid)
        ON DELETE CASCADE,
    CHECK (field_position >= 0)
);

CREATE TABLE iceberg_catalog.snapshots (
    table_uuid      UUID NOT NULL,
    snapshot_id     BIGINT NOT NULL,
    schema_id       INT,
    timestamp_ms    BIGINT NOT NULL,
    manifest_list   TEXT,
    total_records   BIGINT,
    PRIMARY KEY (table_uuid, snapshot_id),
    FOREIGN KEY (table_uuid)
        REFERENCES iceberg_catalog.tables_internal(table_uuid)
        ON DELETE CASCADE
);

CREATE TABLE iceberg_catalog.partition_specs (
    table_uuid      UUID NOT NULL,
    spec_id         INT NOT NULL,
    field_position  INT NOT NULL,
    field_id        INT,
    source_id       INT,
    field_name      TEXT,
    transform       TEXT,
    PRIMARY KEY (table_uuid, spec_id, field_position),
    UNIQUE (table_uuid, spec_id, field_id),
    FOREIGN KEY (table_uuid)
        REFERENCES iceberg_catalog.tables_internal(table_uuid)
        ON DELETE CASCADE,
    -- Use field_position = -1 to represent an unpartitioned spec row.
    CHECK (
        (field_position = -1
            AND field_id IS NULL
            AND source_id IS NULL
            AND field_name IS NULL
            AND transform IS NULL)
        OR
        (field_position >= 0
            AND field_id IS NOT NULL
            AND source_id IS NOT NULL
            AND field_name IS NOT NULL
            AND transform IS NOT NULL)
    )
);

CREATE TABLE iceberg_catalog.tables_external (
    catalog_name               TEXT NOT NULL,
    namespace                  TEXT NOT NULL,
    table_name                 TEXT NOT NULL,
    metadata_location          TEXT NOT NULL,
    previous_metadata_location TEXT,
    PRIMARY KEY (catalog_name, namespace, table_name),
    FOREIGN KEY (catalog_name, namespace)
        REFERENCES iceberg_catalog.namespaces(catalog_name, namespace)
        ON DELETE RESTRICT
);

CREATE OR REPLACE VIEW iceberg_catalog.iceberg_tables AS
SELECT
    current_database()::TEXT AS catalog_name,
    namespace AS table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.tables_internal
UNION ALL
SELECT
    catalog_name,
    namespace AS table_namespace,
    table_name,
    metadata_location,
    previous_metadata_location
FROM iceberg_catalog.tables_external e
WHERE catalog_name <> current_database()::TEXT;

CREATE OR REPLACE VIEW iceberg_catalog.iceberg_namespace_properties AS
SELECT
    n.catalog_name,
    n.namespace,
    p.key AS property_key,
    p.value AS property_value
FROM iceberg_catalog.namespaces n
, jsonb_each_text(n.properties) AS p(key, value);

COMMENT ON SCHEMA iceberg_catalog IS
'Iceberg catalog metadata managed by the iceberg_catalog extension.';

COMMENT ON TABLE iceberg_catalog.namespaces IS
'Namespace directory records and namespace-level properties.';

COMMENT ON TABLE iceberg_catalog.tables_internal IS
'Internal Iceberg table heads bound to local openGauss relations.';

COMMENT ON TABLE iceberg_catalog.table_schemas IS
'Expanded schema field definitions from metadata.json schemas[].';

COMMENT ON TABLE iceberg_catalog.snapshots IS
'Snapshot summaries cached from metadata.json snapshots[].';

COMMENT ON TABLE iceberg_catalog.partition_specs IS
'Expanded partition spec field definitions from metadata.json partition-specs[].';

COMMENT ON TABLE iceberg_catalog.tables_external IS
'External catalog directory records without local relation binding.';

COMMENT ON VIEW iceberg_catalog.iceberg_tables IS
'JDBC Catalog compatible table directory view over internal and external records.';

COMMENT ON VIEW iceberg_catalog.iceberg_namespace_properties IS
'JDBC Catalog compatible namespace property rows expanded from namespaces.properties.';


-- ============================================================================
-- SQL Functions
-- ============================================================================

CREATE OR REPLACE FUNCTION iceberg_catalog.drop_namespace(
    p_namespace TEXT
) RETURNS JSONB
LANGUAGE C VOLATILE
AS 'iceberg_catalog', 'iceberg_drop_namespace';

CREATE OR REPLACE FUNCTION iceberg_catalog.create_table(
    p_namespace    TEXT,
    p_table_name   TEXT,
    p_schema       JSONB,
    p_location     TEXT    DEFAULT NULL,
    p_partition_spec JSONB DEFAULT NULL,
    p_write_order  JSONB   DEFAULT NULL,
    p_stage_create BOOLEAN DEFAULT FALSE,
    p_properties   JSONB   DEFAULT NULL
) RETURNS JSONB
LANGUAGE C VOLATILE
AS 'iceberg_catalog', 'iceberg_create_table';

CREATE OR REPLACE FUNCTION iceberg_catalog.is_table_existed(
    p_namespace TEXT,
    p_table     TEXT
) RETURNS JSONB
LANGUAGE C STABLE
AS 'iceberg_catalog', 'iceberg_is_table_existed';

CREATE OR REPLACE FUNCTION iceberg_catalog.load_table(
    p_namespace TEXT,
    p_table     TEXT
) RETURNS JSONB
LANGUAGE C STABLE
AS 'iceberg_catalog', 'iceberg_load_table';
