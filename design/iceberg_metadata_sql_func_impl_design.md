# Iceberg Metadata SQL Function 实现设计

## 1. 文档说明

### 1.1 目标

本文档为 SQL 自定义函数层（对应 `iceberg_metadata_sql_func_def_design.md` 中定义的 14 个函数）做实现设计，基于 **Iceberg v3** 规范。明确：

1. **职责边界**：SQL 自定义函数、元信息表、Iceberg SDK 三层各自的职责范围及边界规则
2. **事务语义**：C++ 元信息模块操作如何保证与外围 SQL 调用处于同一事务
3. SQL Function 对**元信息模块**的接口诉求
4. SQL Function 对 **Iceberg SDK** 的接口诉求
5. SQL Function 实现逻辑

### 1.2 相关文档

```
iceberg_metadata_sql_func_def_design.md  —— SQL 函数接口定义（WHAT）
iceberg_metadata_sql_func_impl_design.md —— 本文件，实现设计（HOW）
gv_catalog_metadata_schema_design.md     —— 元信息表 Schema（PR #1）
```
---


## 2. 系统上下文

### 2.1 双引擎读写架构

```
                        OpenGauss
┌──────────────────────────────────────────────────────────────────┐
│  SQL 自定义函数层                                                  │
│                                                                    │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  SQL 函数声明 (LANGUAGE C)                                    │  │
│  │                                                               │  │
│  │  iceberg_create_namespace() ──→ iceberg_create_namespace_impl │  │
│  │  iceberg_list_namespaces()  ──→ iceberg_list_namespaces_impl  │  │
│  │  iceberg_load_namespace()   ──→ iceberg_load_namespace_impl   │  │
│  │  ... (共 14 个) ────────────→ ...                             │  │
│  └───────────────┬──────────────────────────────────────────────┘  │
│                  │                                                 │
│                  ▼                                                 │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │  C++ 实现层 (iceberg_sql_funcs.cpp)                         │  │
│  │                                                               │  │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐                   │  │
│  │  │ META 调用   │  │ SDK 调用  │  │ JSON 构造 │                   │  │
│  │  │ 元信息表  │  │ 元数据读写│  │ 返回值    │                   │  │
│  │  └─────┬─────┘  └────┬─────┘  └──────────┘                   │  │
│  └────────┼─────────────┼──────────────────────────────────────┘  │
│           │             │                                          │
│           ▼             ▼                                          │
│  ┌────────────────┐  ┌──────────────────────────────┐            │
│  │ 元信息表         │  │ Iceberg SDK 桥接层            │            │
│  │ (iceberg_catalog)│  │ - metadata JSON 读/写        │            │
│  │                  │  │ - manifest/list 操作         │            │
│  │ namespaces       │  │ - snapshot 管理              │            │
│  │ tables_internal  │  └──────────────┬───────────────┘            │
│  │ table_schemas    │                │                              │
│  │ snapshots        │                │ S3 PutObject/GetObject       │
│  │ partition_specs  │                ▼                              │
│  └──────────────────┘  ┌──────────────────────────────┐            │
│                         │ 对象存储 (S3)                 │            │
│                         │ s3://bucket/ns/table/        │            │
│                         │   metadata/v{N}.metadata.json│            │
│                         │   data/*.parquet             │            │
│                         └──────────────────────────────┘            │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 模块交互总览

每个 SQL 自定义函数的实现涉及以下模块的交互：

| 模块 | 职责 | 交互方式 |
|------|------|---------|
| **元信息表** | 持久化 Catalog 对象映射、metadata 指针、高频摘要缓存 | C++ 调用 iceberg_meta_* 接口 |
| **Iceberg SDK** | metadata JSON 构造/解析、schema/snapshot/partition 操作 | C++ 直接调用 C 函数 |
| **对象存储** | 读写 `metadata.json`、manifest 文件等 | Iceberg SDK 内部封装 S3 API |
| **DDL 管理模块** | 创建/删除 delta 表和 FDW 外表 | C 函数调用（非本设计展开范围） |

---

## 3. 职责边界

本章明确 SQL 自定义函数层、元信息表层、Iceberg SDK 层三者的职责边界，防止 Iceberg 语义泄漏到 SQL 函数层。

### 3.1 边界规则清单

SQL 函数实现中 **禁止** 的行为：

| # | 禁止行为 | 原因 | 应由 SDK 负责 |
|---|---------|------|-------------|
| 1 | 拼接 S3 路径（如 `nsLocation + "/" + tableName`） | 路径格式是 Iceberg 语义 | SDK |
| 2 | 知道 metadata 文件命名规则（如 `v{N}.metadata.json`） | 文件命名是 Iceberg 规范约定 | SDK |
| 3 | 硬编码 `DefaultPrefix` 作为 fallback | 默认位置策略是部署级配置 | SDK |
| 4 | 直接操作 metadata JSON 内部字段 | JSON Schema 是 Iceberg 规范 | SDK |
| 5 | 校验 Iceberg 复杂类型（`fixed(L)`、`decimal(P,S)` 等） | 类型系统是 Iceberg 语义 | SDK（`ValidateType`） |
| 6 | 构造 `metadata_location` 路径 | 路径版本号由 SDK 管理 | SDK（`WriteTableMetadata`） |
| 7 | 决定 namespace marker 的 S3 key | S3 key 结构是 SDK 实现细节 | SDK |

### 3.2 判断标准

当不确定某段逻辑应该放在哪一层时，使用以下判断标准：

> **"这段逻辑在切换对象存储（S3 → OSS → HDFS）或切换 Iceberg 版本（v1 → v2 → v3）时是否需要变化？"**
>
> - 如果需要变化 → 属于 **Iceberg SDK** 层
> - 如果不变 → 属于 **SQL 函数** 层

> **"这段逻辑涉及的是数据库概念（约束、索引、事务）还是 Iceberg 表概念（schema、snapshot、partition）？"**
>
> - 数据库概念 → 属于 **元信息表** 层
> - Iceberg 概念 → 属于 **Iceberg SDK** 层
> - 两者的协调 → 属于 **SQL 函数** 层

---

## 4. 事务语义

事务语义由**元信息模块**内部保证。SQL 自定义函数调用的所有 `iceberg_meta_*` 接口均在当前 SQL 函数的事务上下文中执行——元信息模块的底层实现（SPI 或直接表访问）不改变此保证。

## 7. 函数实现伪代码

以下为 14 个 SQL 自定义函数的实现伪代码。伪代码中：
- `catalog` 是 `IcebergCatalog::Open(warehouse, &err)` 返回的会话级单例（第 6.3 节）
- `table->xxx()` 表示调用 `IcebergTable` 的方法（第 6.4 节）
- `META.xxx()` 表示调用元信息模块接口（第 7 章）
- `VALIDATE(cond, SQLSTATE, msg)` 表示参数校验，失败则 `ereport`
- `SDK_CHECK(ptr, error_msg, iceberg_type)` / `META_CHECK(ok, error_msg, iceberg_type)` — 检查错误消息，非 NULL 时包装为 Iceberg REST API JSON 格式并 `ereport`：
  ```
  void SDK_CHECK(void *result, char *error_msg, const char *iceberg_type) {
      if (error_msg != NULL)
          ereport(ERROR, errcode(ERRCODE_P0009),
                  errmsg("{\"type\":\"%s\",\"message\":\"%s\",\"stack\":[]}",
                         iceberg_type, error_msg));
  }
  ```

### 7.1 is_namespace_existed

```
is_namespace_existed(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. return META.NamespaceExists(p_namespace)
        ? {"exists": true}
        : {"exists": false}
```

### 7.2 is_table_existed

```
is_table_existed(p_namespace TEXT, p_table TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. p_table 为 NULL 或空串 → ereport(P0001, "table must not be empty")
3. return META.TableExists(p_namespace, p_table)
        ? {"exists": true}
        : {"exists": false}
```

### 7.3 load_namespace

```
load_namespace(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. meta_info = META.GetNamespace(p_namespace)
   if meta_info == NULL → ereport(P0004, "namespace not found")
3. return {
       "namespace":  [meta_info.namespace_name],
       "properties": json_parse(meta_info.properties)
     }
```

### 7.4 list_namespaces

```
list_namespaces(p_parent TEXT DEFAULT NULL, p_page_size INT DEFAULT 1000,
                p_page_token TEXT DEFAULT NULL) → JSONB

1. if p_page_size < 1 → ereport(P0001, "pageSize must be >= 1")
2. if p_parent 非空:
       if !META.NamespaceExists(p_parent) → ereport(P0004)
3. result = META.ListNamespaces(p_parent, p_page_size, p_page_token)
4. return json_parse(result)
```

### 7.5 list_tables

```
list_tables(p_namespace TEXT, p_page_size INT DEFAULT 1000,
            p_page_token TEXT DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. if p_page_size < 1 → ereport(P0001)
3. if !META.NamespaceExists(p_namespace) → ereport(P0004)
4. result = META.ListTables(p_namespace, p_page_size, p_page_token)
5. return json_parse(result)
```

### 7.6 create_namespace

```
create_namespace(p_namespace TEXT, p_properties JSONB DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. if p_properties 非 NULL 且非合法 JSONB object → ereport(P0001)

3. // ★ 先写元信息表（利用 PK 约束仲裁并发冲突，详见 8.15）
   props_str = p_properties ? jsonb_to_cstring(p_properties) : "{}"
   if META.NamespaceExists(p_namespace) → ereport(P0005)
   META.InsertNamespace(p_namespace, props_str)

4. meta_info = META.GetNamespace(p_namespace)
5. return {"namespace": [meta_info.namespace_name],
           "properties": json_parse(meta_info.properties)}
```

### 7.7 drop_namespace

```
drop_namespace(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. if !META.NamespaceExists(p_namespace) → ereport(P0004)
3. if META.NamespaceHasTables(p_namespace) → ereport(P0005)

4. META.DeleteNamespace(p_namespace)

5. // SDK 清理 S3 marker（best-effort，SDK 内部通过 catalog 执行）
   catalog->DropNamespace(p_namespace)

6. return {"success": true}
```

### 7.8 update_namespace_properties

```
update_namespace_properties(p_namespace TEXT, p_removals JSONB DEFAULT NULL,
                            p_updates JSONB DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. if p_removals 和 p_updates 同时为 NULL → ereport(P0001)
3. removals_arr: 非 NULL 则必须为 JSONB 数组，否则 P0001
4. updates_obj:  非 NULL 则必须为 JSONB object，否则 P0001
5. if removals ∩ updates ≠ ∅ → ereport(P0006)

6. removals_str = p_removals ? jsonb_to_cstring(p_removals) : "[]"
   updates_str  = p_updates  ? jsonb_to_cstring(p_updates)  : "{}"

7. result = META.UpdateNamespaceProperties(p_namespace, removals_str, updates_str)
8. return json_parse(result)
```

### 7.9 rename_table

```
rename_table(p_src_ns TEXT, p_src_table TEXT,
             p_dst_ns TEXT, p_dst_table TEXT) → JSONB

1. 任一参数为 NULL 或空串 → ereport(P0001)
2. if !META.TableExists(p_src_ns, p_src_table)  → ereport(P0004)
3. if !META.NamespaceExists(p_dst_ns)            → ereport(P0004)
4. if META.TableExists(p_dst_ns, p_dst_table)    → ereport(P0005)

5. META.RenameTable(p_src_ns, p_src_table, p_dst_ns, p_dst_table)

6. // SDK：若需同步更新 S3 路径，调用 catalog->RenameTable（预留）
   // 标准 Iceberg 中此步骤为空操作
   error_msg = NULL
   catalog->RenameTable(p_src_ns, p_src_table, p_dst_ns, p_dst_table, &error_msg)
   SDK_CHECK((void*)1/*non-null sentinel*/, error_msg, "ServiceUnavailable")

7. return {"success": true}
```

### 7.10 create_table

```
create_table(p_namespace TEXT, p_table_name TEXT, p_schema JSONB,
             p_location TEXT DEFAULT NULL, p_partition_spec JSONB DEFAULT NULL,
             p_write_order JSONB DEFAULT NULL, p_stage_create BOOL DEFAULT FALSE,
             p_properties JSONB DEFAULT NULL) → JSONB

1. p_namespace/p_table_name 为 NULL 或空串 → ereport(P0001)

2. // Schema 校验
   if p_schema.type ≠ "struct" → ereport(P0001)
   for each field in p_schema.fields:
       if !catalog->ValidateType(field.type, &err) → ereport(P0001, err)

3. // 业务检查
   if !META.NamespaceExists(p_namespace) → ereport(P0004)
   if META.TableExists(p_namespace, p_table_name) → ereport(P0005)

4. // SDK 创建表（内部：UUID → 路径解析 → metadata 构造 → S3 写）
   error_msg = NULL
   table = catalog->CreateTable(p_namespace, p_table_name,
                                 jsonb_to_cstring(p_schema),
                                 p_location,
                                 p_partition_spec ? jsonb_to_cstring(p_partition_spec) : NULL,
                                 p_write_order    ? jsonb_to_cstring(p_write_order)    : NULL,
                                 p_properties     ? jsonb_to_cstring(p_properties)     : NULL,
                                 &error_msg)
   SDK_CHECK(table, error_msg, "ServiceUnavailable")

5. // DDL 管理模块创建 delta 表和 FDW 外表 → 返回 relid
   relid = DDL_CreateStorage(p_namespace, p_table_name, table->GetTableUUID())

6. // 写元信息表（PK (namespace, table_name) 仲裁并发冲突）
   META.InsertTable(p_namespace, p_table_name,
       MetaTableInfo{
           .relid                = relid,
           .table_uuid           = table->GetTableUUID(),
           .metadata_location    = table->GetMetadataLocation(),
           .previous_metadata_location = NULL,
           .table_location       = table->GetTableLocation(),
           .last_column_id       = table->GetLastColumnId(),
           .current_schema_id    = table->GetCurrentSchemaId(),
           .current_snapshot_id  = -1,    // 初始无 snapshot
           .default_spec_id      = table->GetDefaultPartitionSpecId()
       })

7. metadata_json = table->GetMetadataJson()
   delete table
   return {
       "metadata-location": table->GetMetadataLocation(),
       "metadata":          json_parse(metadata_json),
       "config":            {}
     }
```

### 7.11 load_table

```
load_table(p_namespace TEXT, p_table TEXT) → JSONB

1. 任一参数为 NULL 或空串 → ereport(P0001)

2. info = META.GetTable(p_namespace, p_table)
   if info == NULL → ereport(P0004, "table not found")

3. // ★ SDK 加载表（传入 metadata_location 从 S3 读取并解析 metadata JSON）
   error_msg = NULL
   table = catalog->LoadTable(p_namespace, p_table, info->metadata_location, &error_msg)
   SDK_CHECK(table, error_msg, "ServiceUnavailable")

4. metadata_json = table->GetMetadataJson()   // 含 next-row-id
   delete table
   return {
       "metadata-location": info.metadata_location,
       "metadata":          json_parse(metadata_json),
       "config":            {}
     }
```

### 7.12 drop_table

```
drop_table(p_namespace TEXT, p_table TEXT, p_purge BOOL DEFAULT FALSE) → JSONB

1. 任一参数为 NULL 或空串 → ereport(P0001)
2. if p_purge → ereport(P0008, "purge not yet implemented")

3. info = META.GetTableForUpdate(p_namespace, p_table)
   if info == NULL → ereport(P0004, "table not found")

4. DDL_DropStorage(p_namespace, p_table, info.table_uuid)

5. META.DeleteTable(p_namespace, p_table)
     // META 内部处理 ON DELETE CASCADE

6. // SDK 清理（best-effort，若 purge 支持则清理数据文件）
   catalog->DropTable(p_namespace, p_table)

7. return {"success": true}
```

### 7.13 commit_table

```
commit_table(p_namespace TEXT, p_table TEXT,
             p_requirements JSONB, p_updates JSONB) → JSONB

1. 任一参数为 NULL → ereport(P0001)
2. 校验 p_updates 中每个 element.action 为 "add-snapshot" → 否则 P0001

3. info = META.GetTableForUpdate(p_namespace, p_table)
   if info == NULL → ereport(P0004, "table not found")

4. // ★ SDK 加载表对象
   error_msg = NULL
   table = catalog->LoadTable(p_namespace, p_table, info->metadata_location, &error_msg)
   SDK_CHECK(table, error_msg, "ServiceUnavailable")

5. // ★ SDK 应用 requirements + updates + 写 S3（三步合一）
   req_str  = jsonb_to_cstring(p_requirements)
   upd_str  = jsonb_to_cstring(p_updates)
   error_msg = NULL
   newMdlLocation = table->CommitTable(req_str, upd_str, &error_msg)
   SDK_CHECK(newMdlLocation, error_msg, "CommitFailedException")
   // CommitTable 内部：应用 requirements → 追加 snapshot → 写新 metadata JSON 到 S3

6. // 先写元信息表（乐观锁）
   newSnapshotId = get_snapshot_id_from(p_updates)
   META.UpdateTable(p_namespace, p_table,
                     info.metadata_location,    // old → 乐观锁
                     newMdlLocation,
                     newSnapshotId,
                     table->GetCurrentSchemaId(),
                     table->GetLastColumnId())

7. metadata_json = table->GetMetadataJson()
   delete table
   return {
       "metadata-location": newMdlLocation,
       "metadata":          json_parse(metadata_json)
     }
```

### 7.14 add_column

```
add_column(p_namespace TEXT, p_table TEXT,
           p_column_name TEXT, p_column_type TEXT,
           p_column_doc TEXT DEFAULT NULL) → JSONB

1. p_namespace/p_table/p_column_name/p_column_type 任一为 NULL 或空串 → ereport(P0001)
2. if !catalog->ValidateType(p_column_type, &err) → ereport(P0001, err)

3. info = META.GetTableForUpdate(p_namespace, p_table)
   if info == NULL → ereport(P0004, "table not found")

4. // ★ SDK 加载表对象
   error_msg = NULL
   table = catalog->LoadTable(p_namespace, p_table, info->metadata_location, &error_msg)
   SDK_CHECK(table, error_msg, "ServiceUnavailable")

5. // 检查列名冲突
   currentSchema = table->GetCurrentSchema()
   if currentSchema has field named p_column_name → ereport(P0001, "column already exists")

6. // ★ SDK 扩展 Schema（返回新 schema + new field ID）
   newFieldId = 0
   newSchema = table->AddColumn(p_column_name, p_column_type,
                                  p_column_doc, &newFieldId)
   newSchemaId = table->GetCurrentSchemaId() + 1

7. // 自动构造 requirements 和 updates
   requirements = [
       {"type":"assert-table-uuid",            "uuid": info.table_uuid},
       {"type":"assert-ref-snapshot-id",       "ref":"main",
                                                "snapshot-id": info.current_snapshot_id},
       {"type":"assert-current-schema-id",     "current-schema-id": info.current_schema_id},
       {"type":"assert-last-assigned-field-id","last-assigned-field-id": info.last_column_id}
   ]
   updates = [
       {"action":"add-schema",         "schema": json_parse(newSchema->GetSchemaJson()),
                                        "last-column-id": newFieldId},
       {"action":"set-current-schema", "schema-id": newSchemaId}
   ]

8. // ★ SDK 应用变更 + 写 S3
   error_msg = NULL
   newMdlLocation = table->CommitTable(jsonb_to_cstring(requirements),
                                         jsonb_to_cstring(updates), &error_msg)
   SDK_CHECK(newMdlLocation, error_msg, "CommitFailedException")

9. // 先写元信息表（乐观锁）
   META.UpdateTable(p_namespace, p_table,
                     info.metadata_location,
                     newMdlLocation,
                     info.current_snapshot_id,  // snapshot 不变
                     newSchemaId,               // ★ schema 更新
                     newFieldId)                // ★ last_column_id 更新

10. metadata_json = table->GetMetadataJson()
    delete table
    return {
        "metadata-location": newMdlLocation,
        "metadata":          json_parse(metadata_json)
      }
```


###7.15 并发调用分析

SQL 自定义函数可能被多个客户端并发调用。以下分析每个写函数的并发安全性，核心原则是：

> **META（元信息表）先写，利用 DB 事务的锁/PK 约束仲裁冲突；SDK（S3 对象存储）后写，仅在 META 确认成功后执行。**

**为什么 META 必须先写：**

```
✗ SDK → META（错误顺序）：
  Request A: SDK S3 写 ──→ META INSERT ──→ COMMIT
  Request B:     SDK S3 写 ──→ META INSERT (PK冲突→P0005) ──→ ROLLBACK
  问题：B 的 SDK 调用已执行，S3 上产生孤儿文件。B 的错误信息也不精确（SDK 可能报 S3 错误而非 PK 冲突）

✓ META → SDK（正确顺序）：
  Request A: META INSERT ──→ SDK S3 写 ──→ COMMIT
  Request B:     META INSERT (阻塞等待 A 的事务) ──→ ... ──→ A COMMIT 后 → PK冲突→P0005
  优点：B 从未调用 SDK，无孤儿文件。B 立即得到精确的 P0005 错误
```

#### 7.15.1 各函数并发分析

| 函数 | 写顺序 | 并发机制 | 安全？ |
|------|--------|---------|--------|
| `create_namespace` | **META INSERT** | META INSERT 的 PK `(namespace)` 约束仲裁冲突。后到达的请求阻塞等待→前一个 COMMIT 后→PK 冲突→P0005 | ✅ |
| `drop_namespace` | **META DELETE → SDK DropNamespace** | META DELETE 在事务中。SDK cleanup 为 best-effort | ✅ |
| `update_namespace_properties` | 纯 META（SELECT FOR UPDATE → UPDATE） | 行锁串行化。不涉及 SDK | ✅ |
| `rename_table` | **META Update → (可选) SDK RenameTable** | META 的 PK 约束仲裁目标表名冲突。SDK 为可选预留 | ✅ |
| `create_table` | SDK CreateTable → DDL CreateStorage → **META INSERT** | META INSERT 的 PK `(namespace, table_name)` 是最终冲突仲裁点。由于 `table_uuid` 和 `relid` 分别由 SDK 和 DDL 生成，META INSERT 必须在 SDK+DDL 之后。并发场景下两个请求都会执行 SDK S3 写入，但仅一个通过 META INSERT。孤儿 S3 文件的概率性风险可接受（详见 8.15.2） | ✅ |
| `drop_table` | **META GetTableForUpdate → DDL → META DeleteTable → (best-effort) SDK DropTable** | FOR UPDATE 锁 + DELETE 原子性。SDK 为 best-effort | ✅ |
| `commit_table` | **META GetTableForUpdate(FOR UPDATE) → SDK LoadTable → SDK CommitTable → META UpdateTable(乐观锁)** | FOR UPDATE 行锁串行化同一表的并发 commit。SDK S3 写在持有锁期间完成。META UpdateTable 的乐观锁（`WHERE metadata_location = old`）作为防御层 | ✅ |
| `add_column` | 同 `commit_table` | 同 `commit_table` | ✅ |

#### 7.15.2 `create_table` 并发分析

`create_table` 需要 SDK 生成的 `table_uuid`/`metadata_location` 和 DDL 模块生成的 `relid`，META INSERT 必须在两者之后。

```
时间轴 →
Request A: ──SDK CreateTable──┬──DDL CreateStorage──┬──META INSERT──┬──COMMIT──
                               │                     │  (PK仲裁)     │
Request B: ──SDK CreateTable──┴──DDL CreateStorage──┴──META INSERT──┴──PK冲突→P0005
                                                      (B已执行SDK+DDL)
```

- SDK 和 DDL 在 META INSERT 之前执行，并发请求可能都完成 S3 写入和本地表创建
- META INSERT 的 PK `(namespace, table_name)` 是最终冲突仲裁——只有一个请求成功
- 失败的请求：事务回滚 → DDL 创建的表自动删除（openGauss DDL 是事务性的）→ SDK 写入的 S3 metadata 成为孤儿文件
- **孤儿风险可接受**：同一表的并发创建是极端低概率事件，即使发生也只有一个孤儿 metadata JSON 文件（几 KB），不影响系统正确性
- `create_namespace` 无 concurrency 问题：纯 META INSERT，PK 约束直接在数据库层仲裁并发请求

#### 7.15.3 `commit_table` / `add_column` 的 FOR UPDATE 串行化

```
Request A: ──META GetTableForUpdate──┬──SDK Load──┬──SDK CommitTable(S3写)──┬──META UpdateTable──┬──COMMIT──
              (获取行锁)              │            │                         │                    │
Request B: ──META GetTableForUpdate──┴── [阻塞] ──┴── [阻塞] ──────────────┴── [阻塞] ──────────┴── A COMMIT后获取锁，重新读取最新 metadata_location
```

- `GetTableForUpdate` 内部 `SELECT ... FOR UPDATE` 获取行锁，串行化同一表的并发操作
- SDK S3 写在持有锁期间完成——其他请求被阻塞，不会并发写同一表的 S3
- `META UpdateTable` 的乐观锁（`WHERE metadata_location = old`）提供额外防御层

#### 7.15.4 SDK 失败的回滚保证

| 函数 | 失败场景 | 结果 |
|------|---------|------|
| `create_table` | SDK 成功 → DDL 成功 → META INSERT 冲突 | ⚠️ 事务回滚，DDL 表自动删除（DDL 事务性），S3 残留孤儿 metadata JSON（低概率，可接受） |
| `create_table` | SDK 成功 → DDL 失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON |
| `commit_table` | FOR UPDATE 成功 → SDK CommitTable 成功 → META UpdateTable 乐观锁失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON（FOR UPDATE 下极少发生） |
| 所有函数 | META 成功 → SDK 失败 | ✅ 事务回滚，META 变更撤销，SDK 写入失败无残留 |

> **孤儿文件**：仅在 SDK S3 写入成功后 META/DDL 操作失败的极端场景产生（`create_table` 并发冲突、`commit_table` 乐观锁失败等）。孤儿文件处理不在本文档范围内（详见 9.2）。

---

## 8. 错误码映射

与接口设计文档一致，SQL 函数统一使用 `P0001`~`P0009` SQLSTATE：

| SQLSTATE | HTTP | 语义 | C++ 抛出方式 |
|----------|------|------|-------------|
| P0001 | 400 | 参数无效 | `ereport(ERROR, errcode(ERRCODE_P0001), errmsg("{...}"))` |
| P0002 | 401 | 未认证 | 同上 |
| P0003 | 403 | 无权限 | 同上 |
| P0004 | 404 | 资源不存在 | 同上 |
| P0005 | 409 | 资源冲突/提交失败 | 同上 |
| P0006 | 422 | 违反参数约束 | 同上 |
| P0008 | 501 | 功能未实现 | 同上 |
| P0009 | 500 | 服务端内部错误 | 同上 |

所有 `ereport` 的 MESSAGE 遵循 Iceberg REST API 错误格式：
```json
{"type": "<IcebergExceptionType>", "message": "<描述>", "stack": []}
```

C++ 实现示例：
```cpp
ereport(ERROR,
    errcode(ERRCODE_P0005),
    errmsg("{\"type\":\"CommitFailedException\","
           "\"message\":\"Commit conflict: the table has been modified concurrently\","
           "\"stack\":[]}"));
```
