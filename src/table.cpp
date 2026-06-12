/*-------------------------------------------------------------------------
 *
 * table.cpp
 *    Iceberg table SQL function implementations.
 *
 * Stub implementation: all openGauss catalog metadata-table operations
 * and Iceberg SDK calls are marked as TODO, pending the underlying
 * modules to be wired up. Currently returns a minimal response.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"

#include <string.h>

#include "iceberg_catalog.h"
#include "table.h"


/* ---- create_table ---- */

PG_FUNCTION_INFO_V1(iceberg_create_table);

Datum
iceberg_create_table(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace       TEXT     (required)
     *   2. p_table_name      TEXT     (required)
     *   3. p_schema          JSONB    (required)
     *   4. p_location        TEXT     (optional, default NULL)
     *   5. p_partition_spec  JSONB    (optional, default NULL)
     *   6. p_write_order     JSONB    (optional, default NULL)
     *   7. p_stage_create    BOOLEAN  (optional, default FALSE)
     *   8. p_properties      JSONB    (optional, default NULL)
     *
     * Returns: JSONB (LoadTableResult)
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 3)
        elog(ERROR, "iceberg_create_table: expected at least 3 arguments, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_table_name (required) */
    char *p_table_name = NULL;
    if (!PG_ARGISNULL(1))
        p_table_name = text_to_cstring(PG_GETARG_TEXT_P(1));

    /* p_schema (required) */
    Jsonb *p_schema = NULL;
    if (!PG_ARGISNULL(2))
        p_schema = DatumGetJsonb(PG_GETARG_DATUM(2));

    /* p_location (optional, default NULL) */
    char *p_location = NULL;
    if (PG_NARGS() > 3 && !PG_ARGISNULL(3))
        p_location = text_to_cstring(PG_GETARG_TEXT_P(3));

    /* p_partition_spec (optional, default NULL) */
    Jsonb *p_partition_spec = NULL;
    if (PG_NARGS() > 4 && !PG_ARGISNULL(4))
        p_partition_spec = DatumGetJsonb(PG_GETARG_DATUM(4));

    /* p_write_order (optional, default NULL) */
    Jsonb *p_write_order = NULL;
    if (PG_NARGS() > 5 && !PG_ARGISNULL(5))
        p_write_order = DatumGetJsonb(PG_GETARG_DATUM(5));

    /* p_stage_create (optional, default FALSE) */
    bool p_stage_create = false;
    if (PG_NARGS() > 6 && !PG_ARGISNULL(6))
        p_stage_create = PG_GETARG_BOOL(6);

    /* p_properties (optional, default NULL) */
    Jsonb *p_properties = NULL;
    if (PG_NARGS() > 7 && !PG_ARGISNULL(7))
        p_properties = DatumGetJsonb(PG_GETARG_DATUM(7));

    /* 2. Validate required parameters */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_namespace is required and must not be empty")));

    if (p_table_name == NULL || strlen(p_table_name) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_table_name is required and must not be empty")));

    if (p_schema == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_schema is required and must not be NULL")));

    /* 3. TODO: Validate p_schema type == "struct", ValidateType for each field */

    /* TODO: Validate p_schema type is "struct" */
    /* TODO: For each field in p_schema.fields[], call catalog->ValidateType(field.type) */

    /* 4. TODO: Check namespace exists */

    /* TODO:
     * if (!iceberg_meta_namespace_exists(p_namespace))
     *     iceberg_error(ERRCODE_ICEBERG_NOT_FOUND,
     *                   "NoSuchNamespaceException",
     *                   "Namespace does not exist");
     */

    /* 5. TODO: Check table does not already exist */

    /* TODO:
     * if (iceberg_meta_table_exists(p_namespace, p_table_name))
     *     iceberg_error(ERRCODE_ICEBERG_CONFLICT,
     *                   "AlreadyExistsException",
     *                   "Table already exists");
     */

    /* 6. TODO: SDK CreateTable */

    /* TODO:
     * IcebergCatalog *catalog = get_iceberg_catalog();
     * LoadTableResult result = catalog->CreateTable(
     *     p_namespace, p_table_name, p_schema,
     *     p_location, p_partition_spec, p_write_order,
     *     p_stage_create, p_properties);
     * if (result.error)
     *     iceberg_error(ERRCODE_ICEBERG_INTERNAL_ERROR,
     *                   "RuntimeException",
     *                   "Failed to create table via SDK");
     */

    /* 7. TODO: DDL CreateStorage */

    /* TODO:
     * iceberg_ddl_CreateStorage(p_namespace, p_table_name, result);
     */

    /* 8. TODO: META InsertTable */

    /* TODO:
     * iceberg_meta_register_table(p_namespace, p_table_name, result);
     */

    /* 9. TODO: Construct and return JSONB response */

    /* TODO:
     * StringInfo buf = makeStringInfo();
     * appendStringInfo(buf,
     *     "{\"metadata-location\":\"%s\",\"metadata\":{...},\"config\":{...}}",
     *     result.metadata_location);
     * Jsonb *ret = ...;
     * PG_RETURN_JSONB_P(ret);
     */

    /* 10. Return minimal JSONB response (TODO: replace with real data from SDK/META) */

    /* TODO: Build response from IcebergTable returned by catalog->CreateTable()
     * once SDK & META modules are available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"metadata-location\": \"TODO\", \"metadata\": {}, \"config\": {}}")));
}


/* ---- rename_table ---- */

PG_FUNCTION_INFO_V1(iceberg_rename_table);

Datum
iceberg_rename_table(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_src_ns       TEXT     (required)
     *   2. p_src_table    TEXT     (required)
     *   3. p_dst_ns       TEXT     (required)
     *   4. p_dst_table    TEXT     (required)
     *
     * Returns: JSONB ({"success": true})
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters */

    if (PG_NARGS() < 4)
        elog(ERROR, "iceberg_rename_table: expected 4 arguments, got %d", PG_NARGS());

    char *p_src_ns = NULL;
    if (!PG_ARGISNULL(0))
        p_src_ns = text_to_cstring(PG_GETARG_TEXT_P(0));

    char *p_src_table = NULL;
    if (!PG_ARGISNULL(1))
        p_src_table = text_to_cstring(PG_GETARG_TEXT_P(1));

    char *p_dst_ns = NULL;
    if (!PG_ARGISNULL(2))
        p_dst_ns = text_to_cstring(PG_GETARG_TEXT_P(2));

    char *p_dst_table = NULL;
    if (!PG_ARGISNULL(3))
        p_dst_table = text_to_cstring(PG_GETARG_TEXT_P(3));

    /* 2. Validate required parameters */

    if (p_src_ns == NULL || strlen(p_src_ns) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_src_ns is required and must not be empty")));

    if (p_src_table == NULL || strlen(p_src_table) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_src_table is required and must not be empty")));

    if (p_dst_ns == NULL || strlen(p_dst_ns) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_dst_ns is required and must not be empty")));

    if (p_dst_table == NULL || strlen(p_dst_table) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_dst_table is required and must not be empty")));

    /* 3. TODO: Check source table exists via META */

    /* TODO:
     * if (!iceberg_meta_table_exists(p_src_ns, p_src_table))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *              errmsg("The given source table does not exist")));
     */

    /* 4. TODO: Check destination namespace exists via META */

    /* TODO:
     * if (!iceberg_meta_namespace_exists(p_dst_ns))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *              errmsg("The given destination namespace does not exist")));
     */

    /* 5. TODO: Check destination table does not exist via META */

    /* TODO:
     * if (iceberg_meta_table_exists(p_dst_ns, p_dst_table))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_CONFLICT),
     *              errmsg("The requested table identifier already exists")));
     */

    /* 6. TODO: META RenameTable */

    /* TODO:
     * iceberg_meta_rename_table_record(p_src_ns, p_src_table, p_dst_ns, p_dst_table);
     */

    /* 7. NOTE: SDK RenameTable is not needed.
     *
     * The design doc reserves catalog->RenameTable() for implementations that
     * migrate S3 paths on rename. In standard Iceberg, rename only updates the
     * (namespace, table_name) → metadata_location mapping — S3 metadata.json (keyed
     * by table-uuid) and data files are untouched. Since this mapping is managed
     * entirely by META (iceberg_meta_rename_table_record), calling SDK would
     * duplicate the operation and introduce an atomicity gap between META and SDK.
     *
     * No SDK call required. META is the single source of truth for rename.
     */

    /* 8. Return success */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"success\": true}")));
}
