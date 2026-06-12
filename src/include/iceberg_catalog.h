/*-------------------------------------------------------------------------
 *
 * iceberg_catalog.h
 *    Shared declarations for the iceberg_catalog extension.
 *
 * Defines the extension VERSION string and custom SQLSTATE error codes
 * that map to Iceberg REST Catalog HTTP error semantics.
 *
 *-------------------------------------------------------------------------
 */

#ifndef ICEBERG_CATALOG_H
#define ICEBERG_CATALOG_H

#define ICEBERG_CATALOG_VERSION "1.0.0"

/*
 * Custom SQLSTATE codes for Iceberg REST Catalog error mapping.
 *
 * These use the "P000x" range (PostgreSQL reserved for PLs) to define
 * semantic error codes that can be mapped to HTTP status codes by the
 * REST gateway layer.
 *
 * Code     HTTP mapping          Meaning
 * ------   --------------------  ------------------------------------
 * P0001    400 Bad Request       Invalid parameter value
 * P0002    401 Unauthorized      Missing or invalid credentials
 * P0003    403 Forbidden         Insufficient privileges
 * P0004    404 Not Found         Namespace or table does not exist
 * P0005    409 Conflict          Resource already exists (e.g. duplicate table)
 * P0006    422 Unprocessable     Constraint violation
 * P0008    501 Not Implemented   Feature not yet supported
 * P0009    500 Internal Error    Unexpected internal failure
 */
#define ERRCODE_ICEBERG_INVALID_PARAM     MAKE_SQLSTATE('P','0','0','0','1')  /* 400 Bad Request */
#define ERRCODE_ICEBERG_UNAUTHORIZED      MAKE_SQLSTATE('P','0','0','0','2')  /* 401 Unauthorized */
#define ERRCODE_ICEBERG_FORBIDDEN         MAKE_SQLSTATE('P','0','0','0','3')  /* 403 Forbidden */
#define ERRCODE_ICEBERG_NOT_FOUND         MAKE_SQLSTATE('P','0','0','0','4')  /* 404 Not Found */
#define ERRCODE_ICEBERG_CONFLICT          MAKE_SQLSTATE('P','0','0','0','5')  /* 409 Conflict */
#define ERRCODE_ICEBERG_CONSTRAINT_VIOL   MAKE_SQLSTATE('P','0','0','0','6')  /* 422 Unprocessable */
#define ERRCODE_ICEBERG_NOT_SUPPORTED     MAKE_SQLSTATE('P','0','0','0','8')  /* 501 Not Implemented */
#define ERRCODE_ICEBERG_INTERNAL_ERROR    MAKE_SQLSTATE('P','0','0','0','9')  /* 500 Internal Error */

#endif /* ICEBERG_CATALOG_H */
