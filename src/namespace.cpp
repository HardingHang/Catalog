/*-------------------------------------------------------------------------
 *
 * namespace.cpp
 *    Iceberg namespace SQL function implementations.
 *
 * Stub implementation: all openGauss catalog metadata-namespace operations
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
#include "namespace.h"


/* ---- create_namespace ---- */

PG_FUNCTION_INFO_V1(iceberg_create_namespace);

Datum
iceberg_create_namespace(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace   TEXT    (required)
     *   2. p_properties  JSONB   (optional, default NULL)
     *
     * Returns: JSONB (CreateNamespaceResponse)
     *   {"namespace": ["<namespace>"], "properties": {<key>: <value>, ...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_create_namespace: expected at least 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_properties (optional, default NULL -> treat as empty object {}) */
    Jsonb *p_properties = NULL;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        p_properties = DatumGetJsonb(PG_GETARG_DATUM(1));

    /* 2. Validate p_namespace */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. Validate p_properties (if provided, must be a JSONB object) */

    /* TODO:
     * if (p_properties != NULL && !jsonb_is_object(p_properties))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
     *              errmsg("p_properties must be a JSONB object")));
     */

    /* 4. TODO: META Check namespace not already exists
     *
     * if (iceberg_meta_namespace_exists(p_namespace))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_CONFLICT),
     *              errmsg("The given namespace already exists")));
     */

    /* 5. TODO: META InsertNamespace (先写元信息表，利用 PK 约束仲裁并发冲突)
     *
     * char *props_str = (p_properties != NULL)
     *     ? jsonb_to_cstring(p_properties)
     *     : "{}";
     * iceberg_meta_insert_namespace(p_namespace, props_str);
     */

    /* 6. TODO: META GetNamespace → construct and return response
     *
     * NamespaceInfo meta_info = iceberg_meta_get_namespace(p_namespace);
     * StringInfo buf = makeStringInfo();
     * appendStringInfo(buf,
     *     "{\"namespace\":[\"%s\"],\"properties\":%s}",
     *     meta_info.namespace_name,
     *     meta_info.properties);
     * PG_RETURN_JSONB_P(jsonb_parse(buf->data));
     */

    /* 7. Stub: return minimal response.
     * TODO: Replace with META.GetNamespace() result once META module is available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"namespace\":[\"TODO\"],\"properties\":{}}")));
}
