/*-------------------------------------------------------------------------
 *
 * errors.h
 *    Iceberg error-code mapping utilities for the iceberg_catalog extension.
 *
 * SQL-callable functions use this module to present a consistent Iceberg
 * REST-facing error contract (P000x SQLSTATEs), regardless of whether the
 * underlying error originates from metadata SPI operations, SDK calls, or
 * direct parameter validation.
 *
 *-------------------------------------------------------------------------
 */

#ifndef ICEBERG_CATALOG_ERRORS_H
#define ICEBERG_CATALOG_ERRORS_H

#include "postgres.h"

/*
 * Check whether an error code is already an Iceberg-facing P000x SQLSTATE.
 * Returns true if sqlerrcode is one of the ERRCODE_ICEBERG_* codes defined
 * in iceberg_catalog.h, so callers can pass it through unchanged.
 */
bool iceberg_err_is_iceberg_sqlstate(int sqlerrcode);

/*
 * Re-throw a metadata-standard SQLSTATE as an Iceberg P000x error code.
 *
 * Edata must have been obtained via CopyErrorData() inside a PG_CATCH()
 * block.  This function frees edata and FlushErrorState before raising the
 * mapped ereport(ERROR, ...), so it does not return.
 *
 * Metadata APIs expose standard SQLSTATEs (ERRCODE_DUPLICATE_OBJECT,
 * ERRCODE_UNDEFINED_OBJECT, etc.).  This function maps them to the
 * Iceberg REST-facing P000x codes while preserving already-mapped errors.
 *
 * The context string is prefixed to the error message for traceability.
 */
void iceberg_err_rethrow_metadata(ErrorData *edata, const char *context);

#endif /* ICEBERG_CATALOG_ERRORS_H */
