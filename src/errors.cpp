/*-------------------------------------------------------------------------
 *
 * errors.cpp
 *    Iceberg error-code mapping utilities for the iceberg_catalog extension.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "iceberg_catalog.h"
#include "errors.h"


bool
iceberg_err_is_iceberg_sqlstate(int sqlerrcode)
{
    return sqlerrcode == ERRCODE_ICEBERG_INVALID_PARAM ||
           sqlerrcode == ERRCODE_ICEBERG_UNAUTHORIZED ||
           sqlerrcode == ERRCODE_ICEBERG_FORBIDDEN ||
           sqlerrcode == ERRCODE_ICEBERG_NOT_FOUND ||
           sqlerrcode == ERRCODE_ICEBERG_CONFLICT ||
           sqlerrcode == ERRCODE_ICEBERG_CONSTRAINT_VIOL ||
           sqlerrcode == ERRCODE_ICEBERG_NOT_SUPPORTED ||
           sqlerrcode == ERRCODE_ICEBERG_INTERNAL_ERROR;
}

/*
 * Metadata APIs expose standard SQLSTATEs.  SQL functions map them to the
 * Iceberg REST-facing P000x codes while preserving already-mapped errors.
 */
void
iceberg_err_rethrow_metadata(ErrorData *edata, const char *context)
{
    int sqlerrcode = edata->sqlerrcode;
    char *message;

    if (iceberg_err_is_iceberg_sqlstate(sqlerrcode)) {
        FreeErrorData(edata);
        PG_RE_THROW();
    }

    message = edata->message == NULL ? pstrdup("metadata operation failed")
                                     : pstrdup(edata->message);
    FreeErrorData(edata);
    FlushErrorState();

    if (sqlerrcode == ERRCODE_INVALID_PARAMETER_VALUE)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("%s: %s", context, message)));
    if (sqlerrcode == ERRCODE_UNDEFINED_OBJECT)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_NOT_FOUND),
                 errmsg("%s: %s", context, message)));
    if (sqlerrcode == ERRCODE_DUPLICATE_OBJECT ||
        sqlerrcode == ERRCODE_T_R_SERIALIZATION_FAILURE ||
        sqlerrcode == ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_CONFLICT),
                 errmsg("%s: %s", context, message)));
    if (sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_NOT_SUPPORTED),
                 errmsg("%s: %s", context, message)));

    ereport(ERROR,
            (errcode(ERRCODE_ICEBERG_INTERNAL_ERROR),
             errmsg("%s: %s", context, message)));
}
