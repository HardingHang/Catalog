/*-------------------------------------------------------------------------
 *
 * iceberg_catalog.cpp
 *    Minimal shared library entrypoint for the iceberg_catalog extension.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"

#include "iceberg_catalog.h"
#include "namespace.h"

PG_MODULE_MAGIC;
