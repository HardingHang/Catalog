#ifndef ICEBERG_CATALOG_TABLE_H
#define ICEBERG_CATALOG_TABLE_H

#include "fmgr.h"

extern "C" Datum iceberg_create_table(PG_FUNCTION_ARGS);
extern "C" Datum iceberg_rename_table(PG_FUNCTION_ARGS);

#endif /* ICEBERG_CATALOG_TABLE_H */
