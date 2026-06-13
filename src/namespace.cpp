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


/* ---- update_namespace_properties ---- */

PG_FUNCTION_INFO_V1(iceberg_update_namespace_properties);

Datum
iceberg_update_namespace_properties(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace  TEXT    (required)
     *   2. p_removals   JSONB   (optional, default NULL — keys to remove)
     *   3. p_updates    JSONB   (optional, default NULL — keys to set/update)
     *
     * Returns: JSONB (UpdateNamespacePropertiesResponse)
     *   Updated properties as JSONB object, e.g. {"owner": "new", "region": "us"}
     *
     * Errors:
     *   P0001 — p_namespace is NULL/empty, or both p_removals & p_updates NULL,
     *           or p_removals not an array, or p_updates not an object
     *   P0006 — removals ∩ updates ≠ ∅ (overlapping keys)
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_update_namespace_properties: expected at least 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_removals (optional, default NULL) */
    Jsonb *p_removals = NULL;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        p_removals = DatumGetJsonb(PG_GETARG_DATUM(1));

    /* p_updates (optional, default NULL) */
    Jsonb *p_updates = NULL;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2))
        p_updates = DatumGetJsonb(PG_GETARG_DATUM(2));

    /* 2. Validate p_namespace */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("namespace must not be empty")));

    /* 3. Validate: p_removals and p_updates cannot both be NULL */

    if (p_removals == NULL && p_updates == NULL)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_removals and p_updates cannot both be NULL")));

    /* 4. Validate: p_removals (if non-NULL) must be a JSONB array */

    if (p_removals != NULL && !JB_ROOT_IS_ARRAY(p_removals))
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_removals must be a JSONB array")));

    /* 5. Validate: p_updates (if non-NULL) must be a JSONB object */

    if (p_updates != NULL && !JB_ROOT_IS_OBJECT(p_updates))
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_updates must be a JSONB object")));

    /* 6. Validate removals ∩ updates = ∅ */

    if (p_removals != NULL && p_updates != NULL)
    {
        JsonbSuperHeader  rem_header = (JsonbSuperHeader) VARDATA(p_removals);
        JsonbSuperHeader  upd_header = (JsonbSuperHeader) VARDATA(p_updates);
        JsonbIterator    *it;
        JsonbValue         v;
        int tok;

        it = JsonbIteratorInit(rem_header);
        while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
        {
            if (tok == WJB_ELEM)
            {
                JsonbValue *found = FindJsonbValueFromUnsortedObjects(upd_header, &v);
                if (found != NULL)
                    ereport(ERROR,
                            (errcode(ERRCODE_ICEBERG_CONSTRAINT_VIOL),
                             errmsg("removals and updates must not contain overlapping keys")));
            }
        }
    }

    /* 7. TODO: META UpdateNamespaceProperties
     *
     * char *removals_str = (p_removals != NULL)
     *     ? jsonb_to_cstring(p_removals)
     *     : "[]";
     * char *updates_str = (p_updates != NULL)
     *     ? jsonb_to_cstring(p_updates)
     *     : "{}";
     * char *result = iceberg_meta_update_namespace_properties(
     *     p_namespace, removals_str, updates_str);
     * PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in, CStringGetDatum(result)));
     */

    /* 8. Stub: return empty object.
     * TODO: Replace with META.UpdateNamespaceProperties() result once META module is available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{}")));
}
