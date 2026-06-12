/*-------------------------------------------------------------------------
 *
 * metadata.h
 *    Metadata table accessors for the iceberg_catalog extension.
 *
 * This module provides the low-level SPI-based CRUD operations on the
 * Iceberg metadata tables (tables_internal, table_schemas, partition_specs).
 * Higher-level SQL functions (e.g. create_table) call these functions
 * rather than constructing SQL themselves.
 *
 *-------------------------------------------------------------------------
 */

#ifndef ICEBERG_CATALOG_METADATA_H
#define ICEBERG_CATALOG_METADATA_H

#include "postgres.h"

/*
 * MetaTableInfo -- fields that map to columns in iceberg_catalog.tables_internal.
 *
 * Boolean "has_" flags distinguish between "field is 0 / false"
 * and "field was not provided" (i.e. NULL in the database).
 */
typedef struct MetaTableInfo {
    Oid     relid;                         /* OID of the backing storage relation */
    char   *namespace_name;                /* Iceberg namespace (logical schema) */
    char   *table_name;                    /* Iceberg table name */
    char   *table_uuid;                    /* UUID assigned to this table */
    char   *metadata_location;             /* path/URL to v<N>.metadata.json */
    char   *previous_metadata_location;    /* prior metadata location (NULL for new tables) */
    char   *table_location;                /* Iceberg table root path */
    int     last_column_id;                /* highest column id assigned in the schema */
    int     current_schema_id;             /* ID of the current schema */
    bool    has_current_schema_id;         /* false if current_schema_id is not set */
    int64_t current_snapshot_id;           /* ID of the current snapshot */
    bool    has_current_snapshot_id;       /* false if no snapshot exists yet */
    int     default_spec_id;               /* ID of the default partition spec */
    bool    has_default_spec_id;           /* false if default_spec_id is not set */
} MetaTableInfo;

/*
 * MetaRegisterTableInput -- aggregate input for iceberg_meta_register_table().
 *
 * Combines the table head record (MetaTableInfo) with the schema and
 * partition-spec data that are expanded into the dependent tables.
 */
typedef struct MetaRegisterTableInput {
    MetaTableInfo table_info;              /* table head record fields */
    const char   *schema_json;             /* Iceberg struct schema as JSON */
    const char   *partition_fields_json;   /* partition spec fields as JSON */
    int           schema_id;               /* schema version identifier */
    int           spec_id;                 /* partition spec version identifier */
} MetaRegisterTableInput;

/*
 * Check whether an Iceberg namespace exists in the local catalog.
 * Returns true if a row with matching catalog_name and namespace is found.
 */
bool iceberg_meta_namespace_exists(const char *namespace_name);

/*
 * Check whether a table already exists within the given namespace.
 * Both namespace_name and table_name are required (non-empty).
 */
bool iceberg_meta_table_exists(const char *namespace_name, const char *table_name);

/*
 * Register a new Iceberg table in the local metadata tables.
 *
 * Within a single SPI transaction this function:
 *  1. Locks the namespace row for share (prevents concurrent creation races).
 *  2. Inserts the table head record into tables_internal.
 *  3. Expands the schema JSON into table_schemas.
 *  4. Expands the partition spec JSON into partition_specs.
 *
 * The caller is responsible for ensuring the namespace exists and the
 * table name is not already taken.
 */
void iceberg_meta_register_table(const char *namespace_name,
                                 const char *table_name,
                                 const MetaRegisterTableInput *input);

/*
 * Free a MetaTableInfo structure and all of its palloc'd string members.
 * Safe to call with NULL (no-op).
 */
void iceberg_meta_free_table_info(MetaTableInfo *info);

#endif /* ICEBERG_CATALOG_METADATA_H */
