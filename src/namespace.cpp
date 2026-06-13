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


/* ---- list_namespaces ---- */

PG_FUNCTION_INFO_V1(iceberg_list_namespaces);

Datum
iceberg_list_namespaces(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_parent      TEXT     (optional, default NULL)
     *   2. p_page_size   INTEGER  (optional, default 1000)
     *   3. p_page_token  TEXT     (optional, default NULL)
     *
     * Returns: JSONB (ListNamespacesResponse)
     *   {"namespaces": [["ns1"], ...], "next-page-token": "<token>"}
     *   Last page: {"namespaces": [...], "next-page-token": null}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    /* p_parent (optional, default NULL) */
    char *p_parent = NULL;
    if (PG_NARGS() > 0 && !PG_ARGISNULL(0))
        p_parent = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_page_size (optional, default 1000) */
    int p_page_size = 1000;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        p_page_size = PG_GETARG_INT32(1);

    /* p_page_token (optional, default NULL) */
    char *p_page_token = NULL;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
        p_page_token = text_to_cstring(PG_GETARG_TEXT_P(2));

    /* 2. Validate p_page_size */

    if (p_page_size < 1)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("pageSize must be >= 1")));

    /* 3. TODO: Validate p_parent exists (if non-NULL) */

    /* TODO:
     * if (p_parent != NULL && strlen(p_parent) > 0)
     * {
     *     if (!iceberg_meta_namespace_exists(p_parent))
     *         ereport(ERROR,
     *                 (errcode(ERRCODE_ICEBERG_NOT_FOUND),
     *                  errmsg("The given namespace does not exist")));
     * }
     */

    /* 4. TODO: META ListNamespaces */

    /* TODO:
     * char *result = iceberg_meta_list_namespaces(p_parent, p_page_size, p_page_token);
     * PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(result)));
     */

    /* 5. Stub: return empty list with null next-page-token.
     * TODO: Replace with META.ListNamespaces() result once META module is available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"namespaces\": [], \"next-page-token\": null}")));
}
