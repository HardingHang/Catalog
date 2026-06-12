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

#include <stdlib.h>
#include <string.h>

#include "errors.h"
#include "iceberg_catalog.h"
#include "metadata.h"
#include "table.h"


static char *
jsonb_to_cstring(Jsonb *value)
{
    return DatumGetCString(DirectFunctionCall1(jsonb_out, PointerGetDatum(value)));
}

static int
temporary_last_column_id(const char *schema_json)
{
    const char *cursor = schema_json;
    int max_id = 0;

    /*
     * Temporary bridge until SDK schema parsing is wired in.  The metadata
     * layer still validates the full schema JSON before writing cache rows.
     */
    while ((cursor = strstr(cursor, "\"id\"")) != NULL) {
        const char *colon = strchr(cursor, ':');
        long value;

        if (colon == NULL)
            break;
        colon++;
        while (*colon == ' ' || *colon == '\t')
            colon++;
        value = strtol(colon, NULL, 10);
        if (value > max_id)
            max_id = (int) value;
        cursor = colon;
    }

    return max_id;
}


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

    /* 4. Check namespace exists */

    PG_TRY();
    {
        if (!iceberg_meta_namespace_exists(p_namespace))
            ereport(ERROR,
                    (errcode(ERRCODE_ICEBERG_NOT_FOUND),
                     errmsg("namespace not found")));
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        iceberg_err_rethrow_metadata(edata, "create table namespace check");
    }
    PG_END_TRY();

    /* 5. Check table does not already exist */

    PG_TRY();
    {
        if (iceberg_meta_table_exists(p_namespace, p_table_name))
            ereport(ERROR,
                    (errcode(ERRCODE_ICEBERG_CONFLICT),
                     errmsg("table already exists")));
    }
    PG_CATCH();
    {
        ErrorData *edata = CopyErrorData();
        iceberg_err_rethrow_metadata(edata, "create table existence check");
    }
    PG_END_TRY();

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

    /* 8. META InsertTable */

    {
        /*
         * TODO: Replace these temporary values with the SDK CreateTable result
         * and the DDL CreateStorage relation OID once those modules are wired.
         */
        static Oid next_temporary_relid = 800000;
        static unsigned int next_temporary_uuid = 1;
        char *schema_json = jsonb_to_cstring(p_schema);
        char *partition_fields_json = p_partition_spec == NULL ? NULL : jsonb_to_cstring(p_partition_spec);
        char *metadata_location = psprintf("file:///tmp/iceberg_catalog/%s/%s/metadata/v1.metadata.json",
                                           p_namespace, p_table_name);
        char *table_location = p_location == NULL
                                   ? psprintf("file:///tmp/iceberg_catalog/%s/%s", p_namespace, p_table_name)
                                   : p_location;
        char *table_uuid = psprintf("00000000-0000-0000-0000-%012u", next_temporary_uuid++);
        MetaRegisterTableInput meta_input;

        memset(&meta_input, 0, sizeof(meta_input));
        meta_input.table_info.relid = next_temporary_relid++;
        meta_input.table_info.namespace_name = p_namespace;
        meta_input.table_info.table_name = p_table_name;
        meta_input.table_info.table_uuid = table_uuid;
        meta_input.table_info.metadata_location = metadata_location;
        meta_input.table_info.previous_metadata_location = NULL;
        meta_input.table_info.table_location = table_location;
        meta_input.table_info.last_column_id = temporary_last_column_id(schema_json);
        meta_input.table_info.current_schema_id = 0;
        meta_input.table_info.has_current_schema_id = true;
        meta_input.table_info.has_current_snapshot_id = false;
        meta_input.table_info.default_spec_id = 0;
        meta_input.table_info.has_default_spec_id = true;
        meta_input.schema_json = schema_json;
        meta_input.partition_fields_json = partition_fields_json;
        meta_input.schema_id = 0;
        meta_input.spec_id = 0;

        PG_TRY();
        {
            iceberg_meta_register_table(p_namespace, p_table_name, &meta_input);
        }
        PG_CATCH();
        {
            ErrorData *edata = CopyErrorData();
            iceberg_err_rethrow_metadata(edata, "create table metadata registration");
        }
        PG_END_TRY();
    }

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
