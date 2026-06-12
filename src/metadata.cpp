/*-------------------------------------------------------------------------
 *
 * metadata.cpp
 *    Metadata table accessors for the iceberg_catalog extension.
 *
 * All catalog metadata mutations go through SPI (Server Programming Interface).
 * Each public function manages its own SPI connect/finish pair unless it is
 * called from another function that already holds the connection
 * (see iceberg_meta_register_table which wraps multiple operations in one
 * SPI transaction).
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/builtins.h"

#include <string.h>

#include "iceberg_catalog.h"
#include "metadata.h"

/* Convenience macro wrapping SPI_execute_with_args with NULL resource owner */
#define ICEBERG_SPI_EXECUTE_WITH_ARGS(src, nargs, argtypes, values, nulls, read_only, tcount) \
    SPI_execute_with_args(src, nargs, argtypes, values, nulls, read_only, tcount, NULL)

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Returns true if value is NULL or an empty string. */
static bool
is_empty_string(const char *value)
{
    return value == NULL || value[0] == '\0';
}

/* Raises an ERROR if value is NULL or empty.  name is used in the error message. */
static void
validate_name(const char *value, const char *name)
{
    if (is_empty_string(value))
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("%s is required and must not be empty", name)));
}

/* ------------------------------------------------------------------ */
/*  SPI lifecycle helpers                                              */
/* ------------------------------------------------------------------ */

static void
connect_spi(void)
{
    if (SPI_connect() != SPI_OK_CONNECT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("failed to connect to SPI")));
}

static void
finish_spi(void)
{
    if (SPI_finish() != SPI_OK_FINISH)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("failed to finish SPI")));
}

/*
 * Execute a single-parameter SELECT that returns at most one row,
 * and return whether any row was returned.
 */
static bool
execute_exists_query(const char *sql, Datum *values, Oid *argtypes)
{
    int rc;

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(sql, 1, argtypes, values, NULL, true, 1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("metadata exists query failed")));

    return SPI_processed > 0;
}

/* ------------------------------------------------------------------ */
/*  Namespace operations                                               */
/* ------------------------------------------------------------------ */

/*
 * Check whether a namespace exists.
 * Queries iceberg_catalog.namespaces filtered by current_database().
 */
bool
iceberg_meta_namespace_exists(const char *namespace_name)
{
    Datum values[1];
    Oid argtypes[1] = {TEXTOID};
    bool exists;

    validate_name(namespace_name, "namespace_name");

    values[0] = CStringGetTextDatum(namespace_name);

    connect_spi();
    exists = execute_exists_query(
        "SELECT 1 "
        "FROM iceberg_catalog.namespaces "
        "WHERE catalog_name = current_database()::text "
        "  AND namespace = $1",
        values,
        argtypes);
    finish_spi();

    return exists;
}

/* ------------------------------------------------------------------ */
/*  Table existence checks                                             */
/* ------------------------------------------------------------------ */

/*
 * Check whether a table already exists in the given namespace.
 * Consults iceberg_catalog.tables_internal.
 */
bool
iceberg_meta_table_exists(const char *namespace_name, const char *table_name)
{
    Datum values[2];
    Oid argtypes[2] = {TEXTOID, TEXTOID};
    bool exists;
    int rc;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");

    values[0] = CStringGetTextDatum(namespace_name);
    values[1] = CStringGetTextDatum(table_name);

    connect_spi();
    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "SELECT 1 "
        "FROM iceberg_catalog.tables_internal "
        "WHERE namespace = $1 "
        "  AND table_name = $2",
        2,
        argtypes,
        values,
        NULL,
        true,
        1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("metadata table exists query failed")));

    exists = SPI_processed > 0;
    finish_spi();

    return exists;
}

/* ------------------------------------------------------------------ */
/*  Table registration  (transactional, multi-statement)               */
/* ------------------------------------------------------------------ */

/*
 * Lock the namespace row with FOR SHARE to prevent concurrent
 * create-table-within-the-same-namespace races.
 * Raises ERROR if the namespace disappears between the check and the lock.
 */
static void
lock_namespace_for_share(const char *namespace_name)
{
    Datum values[1];
    Oid argtypes[1] = {TEXTOID};
    int rc;

    values[0] = CStringGetTextDatum(namespace_name);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "SELECT 1 "
        "FROM iceberg_catalog.namespaces "
        "WHERE catalog_name = current_database()::text "
        "  AND namespace = $1 "
        "FOR SHARE",
        1,
        argtypes,
        values,
        NULL,
        false,
        1);
    if (rc != SPI_OK_SELECT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("failed to lock namespace metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_NOT_FOUND),
                 errmsg("namespace not found")));
}

/*
 * Insert the table head record into iceberg_catalog.tables_internal.
 *
 * The previous_metadata_location field is intentionally NULL for new tables
 * (the caller must validate this before calling).
 */
static void
insert_table_record(const char *namespace_name,
                    const char *table_name,
                    const MetaTableInfo *info)
{
    Datum values[11];
    Oid argtypes[11] = {
        OIDOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID,
        INT4OID, INT4OID, INT8OID, INT4OID};
    char nulls[11] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    int rc;

    values[0] = ObjectIdGetDatum(info->relid);
    values[1] = CStringGetTextDatum(namespace_name);
    values[2] = CStringGetTextDatum(table_name);
    values[3] = CStringGetTextDatum(info->table_uuid);
    values[4] = CStringGetTextDatum(info->metadata_location);
    values[5] = (Datum) 0;
    nulls[5] = 'n';                                    /* previous_metadata_location (NULL for new table) */
    values[6] = CStringGetTextDatum(info->table_location);
    values[7] = Int32GetDatum(info->last_column_id);
    values[8] = Int32GetDatum(info->current_schema_id);
    if (!info->has_current_snapshot_id)
        nulls[9] = 'n';                                /* current_snapshot_id (NULL when no snapshot yet) */
    else
        values[9] = Int64GetDatum(info->current_snapshot_id);
    values[10] = Int32GetDatum(info->default_spec_id);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "INSERT INTO iceberg_catalog.tables_internal("
        "    relid, namespace, table_name, table_uuid,"
        "    metadata_location, previous_metadata_location, table_location,"
        "    last_column_id, current_schema_id, current_snapshot_id, default_spec_id"
        ") VALUES ("
        "    $1, $2, $3, $4::uuid,"
        "    $5, $6, $7,"
        "    $8, $9, $10, $11"
        ")",
        11,
        argtypes,
        values,
        nulls,
        false,
        0);
    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("failed to insert table metadata")));

    if (SPI_processed != 1)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
                 errmsg("unexpected table metadata insert count")));
}

/*
 * Expand the schema JSON into iceberg_catalog.table_schemas.
 *
 * Each field in the Iceberg struct schema produces one row.
 * The schema JSON must have the shape:
 *   {"type":"struct", "fields":[{"id":N, "name":"...", "required":bool, "type":"..."}, ...]}
 */
static void
insert_schema_fields(const char *table_uuid, int schema_id, const char *schema_json)
{
    Datum values[2];
    Datum insert_values[3];
    Oid argtypes[3] = {TEXTOID, INT4OID, TEXTOID};
    int rc;

    values[0] = CStringGetTextDatum(table_uuid);
    values[1] = Int32GetDatum(schema_id);
    insert_values[0] = values[0];
    insert_values[1] = values[1];
    insert_values[2] = CStringGetTextDatum(schema_json);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "INSERT INTO iceberg_catalog.table_schemas("
        "    table_uuid, schema_id, field_position,"
        "    field_id, field_name, field_required, field_type, field_doc"
        ") "
        "SELECT "
        "    $1::uuid,"
        "    $2,"
        "    (row_number() over ())::int - 1,"
        "    (field_value->>'id')::int,"
        "    field_value->>'name',"
        "    (field_value->>'required')::boolean,"
        "    CASE "
        "        WHEN jsonb_typeof(field_value->'type') = 'string' "
        "        THEN field_value->>'type' "
        "        ELSE (field_value->'type')::text "
        "    END,"
        "    field_value->>'doc' "
        "FROM jsonb_array_elements("
        "    CASE "
        "        WHEN jsonb_typeof($3::jsonb) = 'object' "
        "         AND $3::jsonb->>'type' = 'struct' "
        "         AND jsonb_typeof($3::jsonb->'fields') = 'array' "
        "        THEN $3::jsonb->'fields' "
        "        ELSE NULL::jsonb "
        "    END"
        ") AS fields(field_value) "
        "WHERE field_value ? 'id' "
        "  AND field_value ? 'name' "
        "  AND field_value ? 'required' "
        "  AND field_value ? 'type' ",
        3,
        argtypes,
        insert_values,
        NULL,
        false,
        0);
    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("failed to insert schema metadata")));
}

/*
 * Expand the partition spec JSON into iceberg_catalog.partition_specs.
 *
 * Accepts three input shapes:
 *   - object with a "fields" array  → object wrapping fields
 *   - object without "fields"       → treated as empty spec (sentinel row)
 *   - bare array                    → used directly as fields list
 *
 * An unpartitioned spec produces a sentinel row with field_position = -1.
 *
 * NOTE: openGauss requires the `?` operator (not `-> 'key' IS NULL`) to
 * check for key existence, because `IS NULL` on a jsonb expression does
 * not reliably return boolean in openGauss 6.x.
 */
static void
insert_partition_spec(const char *table_uuid, int spec_id, const char *fields_json)
{
    Datum values[3];
    Oid argtypes[3] = {TEXTOID, INT4OID, TEXTOID};
    const char *json = is_empty_string(fields_json) ? "[]" : fields_json;
    int rc;

    values[0] = CStringGetTextDatum(table_uuid);
    values[1] = Int32GetDatum(spec_id);
    values[2] = CStringGetTextDatum(json);

    rc = ICEBERG_SPI_EXECUTE_WITH_ARGS(
        "WITH spec AS ("
        "    SELECT CASE "
        "        WHEN jsonb_typeof($3::jsonb) = 'object' "
        "         AND jsonb_typeof($3::jsonb->'fields') = 'array' "
        "        THEN $3::jsonb->'fields' "
        "        WHEN jsonb_typeof($3::jsonb) = 'object' "
        "         AND NOT ($3::jsonb ? 'fields') "
        "        THEN '[]'::jsonb "
        "        WHEN jsonb_typeof($3::jsonb) = 'array' "
        "        THEN $3::jsonb "
        "        ELSE NULL::jsonb "
        "    END AS fields"
        ") "
        "INSERT INTO iceberg_catalog.partition_specs("
        "    table_uuid, spec_id, field_position,"
        "    field_id, source_id, field_name, transform"
        ") "
        "SELECT $1::uuid, $2, -1, NULL, NULL, NULL, NULL "
        "FROM spec "
        "WHERE fields IS NOT NULL "
        "  AND jsonb_array_length(fields) = 0 "
        "UNION ALL "
        "SELECT "
        "    $1::uuid,"
        "    $2,"
        "    (row_number() over ())::int - 1,"
        "    (field_value->>'field-id')::int,"
        "    (field_value->>'source-id')::int,"
        "    field_value->>'name',"
        "    CASE "
        "        WHEN jsonb_typeof(field_value->'transform') = 'string' "
        "        THEN field_value->>'transform' "
        "        ELSE (field_value->'transform')::text "
        "    END "
        "FROM spec, jsonb_array_elements(spec.fields) AS elems(field_value) "
        "WHERE spec.fields IS NOT NULL "
        "  AND field_value ? 'field-id' "
        "  AND field_value ? 'source-id' "
        "  AND field_value ? 'name' "
        "  AND field_value ? 'transform' ",
        3,
        argtypes,
        values,
        NULL,
        false,
        0);
    if (rc != SPI_OK_INSERT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("failed to insert partition spec metadata")));

    if (SPI_processed == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("partition spec must be a JSON array")));
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/*
 * Register a new Iceberg table in the local metadata tables.
 *
 * This is the main entry-point for table registration.  It runs entirely
 * within one SPI transaction:
 *
 *   1. Lock the namespace row (FOR SHARE) to serialize concurrent creation.
 *   2. Insert the table head record into tables_internal.
 *   3. Expand and insert schema fields into table_schemas.
 *   4. Expand and insert partition spec fields into partition_specs.
 *
 * All input pointers must remain valid for the duration of the call.
 */
void
iceberg_meta_register_table(const char *namespace_name,
                            const char *table_name,
                            const MetaRegisterTableInput *input)
{
    const MetaTableInfo *info;

    validate_name(namespace_name, "namespace_name");
    validate_name(table_name, "table_name");
    if (input == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("input is required")));

    info = &input->table_info;
    if (!OidIsValid(info->relid))
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("table relid is invalid")));
    validate_name(info->table_uuid, "table_uuid");
    validate_name(info->metadata_location, "metadata_location");
    validate_name(info->table_location, "table_location");
    if (info->previous_metadata_location != NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("previous_metadata_location must be NULL when registering a table")));
    validate_name(input->schema_json, "schema_json");
    if (input->schema_id < 0 || input->spec_id < 0 || info->last_column_id < 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("metadata ids must be non-negative")));
    if (!info->has_current_schema_id || info->current_schema_id != input->schema_id)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("current_schema_id must match schema_id")));
    if (!info->has_default_spec_id || info->default_spec_id != input->spec_id)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("default_spec_id must match spec_id")));

    connect_spi();
    lock_namespace_for_share(namespace_name);
    insert_table_record(namespace_name, table_name, info);
    insert_schema_fields(info->table_uuid, input->schema_id, input->schema_json);
    insert_partition_spec(info->table_uuid, input->spec_id, input->partition_fields_json);
    finish_spi();
}

/*
 * Free a MetaTableInfo and all its palloc'd members.
 */
void
iceberg_meta_free_table_info(MetaTableInfo *info)
{
    if (info == NULL)
        return;

    pfree(info->namespace_name);
    pfree(info->table_name);
    pfree(info->table_uuid);
    pfree(info->metadata_location);
    pfree(info->previous_metadata_location);
    pfree(info->table_location);
    pfree(info);
}
