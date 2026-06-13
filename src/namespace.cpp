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


/* ---- drop_namespace ---- */

PG_FUNCTION_INFO_V1(iceberg_drop_namespace);

Datum
iceberg_drop_namespace(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace  TEXT  (required)
     *
     * Returns: JSONB
     *   {"success": true}
     *
     * Errors:
     *   P0001 — p_namespace is NULL or empty
     *   P0004 — namespace does not exist
     *   P0005 — namespace still contains tables (foreign key constraint)
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_drop_namespace: expected 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* 2. Validate p_namespace */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. TODO: META Check namespace exists
     *
     * if (!iceberg_meta_namespace_exists(p_namespace))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *              errmsg("The given namespace does not exist")));
     */

    /* 4. TODO: META Check namespace has no tables
     *
     * if (iceberg_meta_namespace_has_tables(p_namespace))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_CONFLICT),
     *              errmsg("Cannot drop namespace: tables still exist")));
     */

    /* 5. TODO: META DeleteNamespace
     *
     * iceberg_meta_delete_namespace(p_namespace);
     */

    /* 6. Stub: return success.
     * TODO: Replace with META steps above once META module is available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"success\": true}")));
}
