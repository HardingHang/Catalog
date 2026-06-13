/*-------------------------------------------------------------------------
 *
 * namespace.cpp
 *    Iceberg namespace SQL function implementations.
 *
 * Stub implementation: all openGauss catalog metadata-namespace operations
 * are marked as TODO, pending the underlying modules to be wired up.
 * Currently returns a minimal response.
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


/* ---- load_namespace ---- */

PG_FUNCTION_INFO_V1(iceberg_load_namespace);

Datum
iceberg_load_namespace(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace  TEXT  (required)
     *
     * Returns: JSONB (GetNamespaceResponse)
     *   {"namespace": ["<namespace>"], "properties": {<key>: <value>, ...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_load_namespace: expected 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* 2. Validate p_namespace */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. TODO: META GetNamespace
     *
     * NamespaceInfo meta_info = iceberg_meta_get_namespace(p_namespace);
     * if (meta_info == NULL)
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *              errmsg("The given namespace does not exist")));
     * StringInfo buf = makeStringInfo();
     * appendStringInfo(buf,
     *     "{\"namespace\":[\"%s\"],\"properties\":%s}",
     *     meta_info.namespace_name,
     *     meta_info.properties);
     * PG_RETURN_JSONB_P(jsonb_parse(buf->data));
     */

    /* 4. Stub: return minimal response.
     * TODO: Replace with META.GetNamespace() result once META module is available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"namespace\":[\"TODO\"],\"properties\":{}}")));
}
