/*-------------------------------------------------------------------------
 *
 * namespace.cpp
 *    Iceberg namespace SQL function implementations.
 *
 * Stub implementation: all openGauss catalog metadata-namespace operations
 * and Iceberg SDK calls are marked as TODO, pending the underlying
 * modules to be wired up. Currently returns a minimal response.
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


/* ---- create_namespace ---- */

PG_FUNCTION_INFO_V1(iceberg_create_namespace);

Datum
iceberg_create_namespace(PG_FUNCTION_ARGS)
{
    /*-------------------------------------------------------------------------
     * Parameters:
     *   1. p_namespace   TEXT    (required)
     *   2. p_properties  JSONB   (optional, default NULL)
     *
     * Returns: JSONB (CreateNamespaceResponse)
     *   {"namespace": ["<namespace>"], "properties": {<key>: <value>, ...}}
     *-------------------------------------------------------------------------
     */

    /* 1. Extract parameters from PG_FUNCTION_ARGS */

    if (PG_NARGS() < 1)
        elog(ERROR, "iceberg_create_namespace: expected at least 1 argument, got %d", PG_NARGS());

    /* p_namespace (required) */
    char *p_namespace = NULL;
    if (!PG_ARGISNULL(0))
        p_namespace = text_to_cstring(PG_GETARG_TEXT_P(0));

    /* p_properties (optional, default NULL -> treat as empty object {}) */
    Jsonb *p_properties = NULL;
    if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        p_properties = DatumGetJsonb(PG_GETARG_DATUM(1));

    /* 2. Validate required parameters */

    if (p_namespace == NULL || strlen(p_namespace) == 0)
        ereport(ERROR,
                (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
                 errmsg("p_namespace is required and must not be empty")));

    /* 3. Validate p_properties (if provided, must be a JSONB object) */

    /* TODO:
     * if (p_properties != NULL && !jsonb_is_object(p_properties))
     *     ereport(ERROR,
     *             (errcode(ERRCODE_ICEBERG_INVALID_PARAM),
     *              errmsg("p_properties must be a JSONB object")));
     */

    /* 4. TODO: META Check namespace not already exists */

    /* TODO:
     * if (iceberg_meta_namespace_exists(p_namespace))
     *     iceberg_error(ERRCODE_ICEBERG_CONFLICT,
     *                   "AlreadyExistsException",
     *                   "The given namespace already exists");
     */

    /* 5. TODO: META InsertNamespace (先写元信息表，利用 PK 约束仲裁并发冲突) */

    /* TODO:
     * char *props_str = (p_properties != NULL)
     *     ? jsonb_to_cstring(p_properties)
     *     : "{}";
     * iceberg_meta_insert_namespace(p_namespace, props_str);
     */

    /* 6. TODO: SDK CreateNamespace (解析 S3 路径 + 创建 marker) */

    /* TODO:
     * IcebergCatalog *catalog = get_iceberg_catalog();
     * char *error_msg = NULL;
     * char *ns_location = catalog->CreateNamespace(p_namespace, props_str, &error_msg);
     * if (ns_location == NULL)
     *     iceberg_error(ERRCODE_ICEBERG_INTERNAL_ERROR,
     *                   "ServiceUnavailableException",
     *                   error_msg);
     */

    /* 7. TODO: META UpdateNamespaceProperties (若用户未指定 location，将 SDK 返回的路径写入) */

    /* TODO:
     * if (p_properties == NULL || !jsonb_has_key(p_properties, "location"))
     *     iceberg_meta_update_namespace_properties(
     *         p_namespace, "[]",
     *         json_set("{}", "location", ns_location));
     * pfree(ns_location);
     */

    /* 8. TODO: META GetNamespace (获取最终 namespace 信息) */

    /* TODO:
     * NamespaceInfo meta_info = iceberg_meta_get_namespace(p_namespace);
     * StringInfo buf = makeStringInfo();
     * appendStringInfo(buf,
     *     "{\"namespace\":[\"%s\"],\"properties\":%s}",
     *     meta_info.namespace_name,
     *     meta_info.properties);
     * PG_RETURN_JSONB_P(jsonb_parse(buf->data));
     */

    /* 9. Return minimal JSONB response (TODO: replace with real data from SDK/META) */

    /* TODO: Build response from NamespaceInfo returned by catalog->CreateNamespace()
     * once SDK & META modules are available. */

    PG_RETURN_DATUM(DirectFunctionCall1(jsonb_in,
        CStringGetDatum("{\"namespace\":[\"TODO\"],\"properties\":{}}")));
}
