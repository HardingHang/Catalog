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


/* ---- is_namespace_existed ---- */

PG_FUNCTION_INFO_V1(iceberg_is_namespace_existed);

Datum
iceberg_is_namespace_existed(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace  TEXT  (required)
     *
     * Returns: JSONB
     *   {"exists": true}   — namespace exists
     *   {"exists": false}  — namespace does not exist (no exception thrown)
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_is_namespace_existed: expected 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* 2. Validate required parameters */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. TODO: META NamespaceExists
     *
     * bool exists = iceberg_meta_namespace_exists(p_namespace);
     * PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
     *     CStringGetDatum(exists ? "{\"exists\": true}" : "{\"exists\": false}")));
     */

    /* 4. Stub: always returns {"exists": false}.
     * TODO: Replace with META.NamespaceExists() call once META module is available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"exists\": false}")));
}
