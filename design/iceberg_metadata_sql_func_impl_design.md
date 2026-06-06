# Iceberg Metadata SQL Function 实现设计

## 1. 文档说明

### 1.1 目标

本文档为 SQL 自定义函数层（对应 `iceberg_metadata_sql_func_def_design.md` 中定义的 14 个函数）做实现设计，明确：

1. **设计原则与实现方式**：SQL 函数的实现形式选型及理由
2. **事务语义**：C++ SPI 操作如何保证与外围 SQL 调用处于同一事务
3. 每个 SQL 函数对**元信息表**的读写诉求（通过 SPI）
4. 每个 SQL 函数对 **Iceberg SDK** 的接口诉求（C 函数直接调用）
5. **所有函数的流程图**：完整描述实现逻辑

### 1.2 与其他文档的关系

```
architecture.md                          —— 系统整体架构
iceberg_metadata_sql_func_def_design.md  —— SQL 函数接口定义（WHAT）
iceberg_metadata_sql_func_impl_design.md —— 本文件，实现设计（HOW）
gv_catalog_metadata_schema_design.md     —— 元信息表 Schema（PR #1）
```

### 1.3 术语说明

| 术语 | 含义 |
|------|------|
| **SPI** | Server Programming Interface，PostgreSQL/OpenGauss 提供的 C 语言服务端编程接口，允许 C 函数执行 SQL |
| **Iceberg SDK** | C 扩展模块，封装 metadata JSON 序列化/反序列化、manifest 读写、snapshot 管理等 |
| **LANGUAGE C** | SQL 函数声明方式，函数体由一个 C/C++ 共享库中的符号实现 |

---

## 2. 设计原则与实现方式选择

### 2.1 三种候选方式

SQL 自定义函数的实现有三种方式：

```
方式一：纯 SQL + C++ 子函数
┌─────────────────────────────────────────────┐
│ CREATE FUNCTION ... LANGUAGE plpgsql        │
│ AS $$                                       │
│ BEGIN                                       │
│   -- SQL DML 直接写在函数体中                 │
│   SELECT ... FROM iceberg_catalog.namespaces │
│   INSERT INTO iceberg_catalog.namespaces ... │
│   -- 调用 C++ 子函数（注册为独立 SQL 函数）     │
│   SELECT iceberg_sdk_write_metadata(...)     │
│ END;                                        │
│ $$;                                         │
└─────────────────────────────────────────────┘

方式二：纯 C++ 实现（LANGUAGE C）★ 推荐
┌─────────────────────────────────────────────┐
│ CREATE FUNCTION ... LANGUAGE C              │
│ AS 'MODULE_PATHNAME', 'impl_function';      │
│                                             │
│ // C++ 实现：                                │
│ Datum impl_function(PG_FUNCTION_ARGS) {     │
│   // 1. 提取参数 (PG_GETARG_*)               │
│   // 2. SPI 执行 SQL 操作元信息表             │
│   // 3. 直接调用 Iceberg SDK C 函数           │
│   // 4. 构造 JSONB 返回值 (PG_RETURN_JSONB_P) │
│ }                                           │
└─────────────────────────────────────────────┘

方式三：混合（SQL + C++ 混编）
┌─────────────────────────────────────────────┐
│ -- 函数体中既有 SQL DML 又有 C++ 内联逻辑     │
│ -- 通常难以维护，不推荐                        │
└─────────────────────────────────────────────┘
```

### 2.2 对比分析

| 维度 | 方式一（纯 SQL） | 方式二（纯 C++）★ | 方式三（混合） |
|------|-----------------|-------------------|---------------|
| **元信息表操作** | SQL DML 直接写，透明 | 通过 SPI 执行 SQL | 两者混用 |
| **JSON 构造** | PL/SQL 中构造 JSON 较繁琐 | C++ 中构造 JSON 方便 | 分散在两处 |
| **分页逻辑** | PL/SQL 实现 offset/token 困难 | C++ 中编码/解码 token 容易 | 不一致 |
| **错误处理** | RAISE 抛 JSON 格式错误消息 | C++ 中 `ereport()` 抛 JSON 错误消息 | 风格不统一 |
| **SDK 调用** | 需注册为独立 SQL 函数 | 直接 C 函数调用 | 两者混用 |
| **调试** | SQL 层可见 | 需 C 调试器 | 最困难 |
| **可维护性** | 中等（SDK 调用需跳转） | **好**（逻辑集中一处） | 差（逻辑分散） |

### 2.3 决策：采用方式二（纯 C++ / LANGUAGE C + SPI）

**理由：**

1. **JSONB 返回值是统一需求**：14 个函数全部返回 JSONB。C++ 中可以方便地构造 JSON 对象（通过 `jsonb_build_object` 的 C 等价接口或直接构造 Jsonb 结构），而 PL/pgSQL 中复杂的嵌套 JSON 构造代码冗长且难以维护。

2. **分页逻辑更自然**：`list_namespaces` 和 `list_tables` 需要处理 `p_page_token`（opaque token 的编码/解码）和 `OFFSET/LIMIT` 计算。这些逻辑在 C++ 中实现更清晰。

3. **错误消息格式统一**：所有错误需要输出 Iceberg REST API 格式的 JSON 消息（`{"type":"...","message":"...","stack":[]}`）。C++ 中通过 `ereport(ERROR, ...)` 统一构造这类消息更容易保持格式一致。

4. **Iceberg SDK 是 C 接口**：SDK 暴露的是 C 函数，C++ 代码可直接调用，无需为每个 SDK 函数包装一层 SQL 函数。

5. **逻辑内聚**：每个 SQL 函数的完整实现逻辑（参数校验 → 元信息表操作 → SDK 操作 → 返回构造）集中在一个 C++ 函数中，便于理解、测试和维护。

6. **SPI 事务保证**：C++ SPI 操作自动参与外围 SQL 调用的事务（详见第 3 章），不需要额外处理。

### 2.4 实现模式

每个 SQL 函数遵循统一的实现模式：

```sql
-- SQL 函数声明（仅为骨架，实际逻辑由 C++ 实现）
CREATE OR REPLACE FUNCTION iceberg_create_namespace(
    p_namespace  TEXT,
    p_properties JSONB DEFAULT NULL
) RETURNS JSONB
LANGUAGE C VOLATILE STRICT
AS 'MODULE_PATHNAME', 'iceberg_create_namespace_impl';
```

```cpp
// C++ 实现（文件：iceberg_sql_funcs.cpp）
PG_FUNCTION_INFO_V1(iceberg_create_namespace_impl);
Datum iceberg_create_namespace_impl(PG_FUNCTION_ARGS) {
    // 1. 提取参数
    text *p_namespace = PG_GETARG_TEXT_PP(0);
    Jsonb *p_properties = PG_ARGISNULL(1) ? NULL : PG_GETARG_JSONB_P(1);

    // 2. 参数校验（校验失败 → ereport(ERROR, ...)）

    // 3. SPI 操作元信息表（在当前事务中）
    SPI_execute_with_args("SELECT 1 FROM iceberg_catalog.namespaces WHERE namespace = $1", ...);

    // 4. 调用 Iceberg SDK（直接 C 函数调用）
    IcebergSDK_CreateNamespaceLocation(location, &errorMsg);

    // 5. SPI 写入元信息表
    SPI_execute_with_args("INSERT INTO iceberg_catalog.namespaces ...", ...);

    // 6. 构造 JSONB 返回值
    PG_RETURN_JSONB_P(result);
}
```

---

## 3. 事务语义——C++ SPI 操作的事务保证

### 3.1 核心结论

> **C++ 实现函数中的所有 SPI 操作，100% 在调用 SQL 函数的外层事务中执行。**

### 3.2 原理

PostgreSQL/OpenGauss 的事务绑定在 **backend（会话）** 级别，而非函数调用级别：

```
客户端                                          OpenGauss Backend
  │                                                   │
  ├── BEGIN; ────────────────────────────────────────►│  分配 TransactionId
  │                                                   │  创建 ResourceOwner
  │                                                   │  建立事务快照 (snapshot)
  │                                                   │
  ├── SELECT iceberg_create_namespace(...); ─────────►│  ★ 函数在此事务中执行
  │                                                   │
  │         ┌─────────────────────────────────────┐   │
  │         │ C++ 实现函数                          │   │
  │         │                                       │   │
  │         │ SPI_execute("SELECT ...")             │   │  ← 使用当前 TransactionId
  │         │   → 读取元信息表                       │   │  ← 使用当前 snapshot
  │         │   → 获取行锁 (FOR UPDATE)              │   │  ← 注册到当前 ResourceOwner
  │         │                                       │   │
  │         │ SPI_execute("INSERT INTO ...")        │   │  ← 写入当前事务的 WAL
  │         │   → 写入元信息表                       │   │  ← 标记当前事务的 CID
  │         │                                       │   │
  │         │ IcebergSDK_WriteTableMetadata(...)    │   │  ← S3 写入（非事务性）
  │         │   → 写入 S3                           │   │
  │         │                                       │   │
  │         │ ereport(ERROR, ...)                   │   │  ← 触发事务 ABORT
  │         │   → 所有 SPI 写入回滚                  │   │  ← S3 写入成孤儿文件
  │         └─────────────────────────────────────┘   │
  │                                                   │
  ├── COMMIT; ──────────────────────────────────────►│  SPI 写入 + 元信息表变更原子提交
  │                                                   │
```

### 3.3 关键机制

1. **SPI 不创建新事务**：`SPI_execute()` / `SPI_execute_with_args()` 内部调用 `GetCurrentTransactionId()` 获取当前事务 ID，而非 `AssignTransactionId()` 分配新事务。

2. **ResourceOwner 追踪**：SPI 获取的锁（如 `FOR UPDATE` 行锁）、分配的内存、持有的 buffer pin 都注册到当前事务的 `ResourceOwner` 上。事务结束时统一释放。

3. **CommandCounterIncrement**：SPI 内部的 DML 会正确推进 `cid`（Command ID），保证同一事务中后续 SPI 查询能看到先前的变更。

4. **错误传播**：C++ 函数中调用 `ereport(ERROR, ...)` 触发 `longjmp` 回到外层 executor，executor 调用 `AbortCurrentTransaction()` 回滚事务中的所有变更（包括所有 SPI 操作）。

### 3.4 常见误区澄清

| 误区 | 事实 |
|------|------|
| "C++ SPI 会开独立事务" | SPI 始终使用当前事务。除非显式调用 `BeginInternalSubTransaction()`，否则不会创建子事务。 |
| "SPI 的 INSERT 在函数返回前不可见" | SPI 内的 DML 通过 `CommandCounterIncrement` 推进 CID，同一事务中后续 SPI 查询可以看到。但对外部会话不可见，直到 COMMIT。 |
| "方式一（纯 SQL）的事务保证比方式二好" | 两者完全相同——都使用同一事务上下文。方式一的 SQL DML 和方式二的 SPI DML 在事务层面没有任何区别。 |
| "S3 写入在事务中" | **不在**。S3 是外部系统，不支持 XA/两阶段提交。一致性由元信息表的原子 UPDATE 保证（见第 8 章）。 |

---

## 4. 系统上下文

### 4.1 双引擎读写架构

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
│  │  │ SPI 访问  │  │ SDK 调用  │  │ JSON 构造 │                   │  │
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

### 4.2 模块交互总览

每个 SQL 自定义函数的实现涉及以下模块的交互：

| 模块 | 职责 | 交互方式 |
|------|------|---------|
| **元信息表** | 持久化 Catalog 对象映射、metadata 指针、高频摘要缓存 | C++ SPI 执行 SQL |
| **Iceberg SDK** | metadata JSON 构造/解析、schema/snapshot/partition 操作 | C++ 直接调用 C 函数 |
| **对象存储** | 读写 `metadata.json`、manifest 文件等 | Iceberg SDK 内部封装 S3 API |
| **DDL 管理模块** | 创建/删除 delta 表和 FDW 外表 | C 函数调用（非本设计展开范围） |

---

## 5. Iceberg SDK 接口诉求

SQL 自定义函数不直接拼装 metadata JSON，通过 Iceberg SDK 提供的 C 接口完成元数据操作。由于采用纯 C++ 实现，SDK 接口在 C++ 实现函数中直接调用，无需包装为独立 SQL 函数。

### 5.1 接口清单

```c
/*────────────────────────────────────────────────────────────
 * 元数据读写
 *────────────────────────────────────────────────────────────*/

/* 从 S3 读取 metadata.json 并解析为 TableMetadata 结构 */
IcebergTableMetadata *IcebergSDK_ReadTableMetadata(const char *metadataLocation);

/* 基于空模板构造初始 TableMetadata */
IcebergTableMetadata *IcebergSDK_CreateTableMetadata(
    const char *table_uuid,
    const char *table_location,
    IcebergSchema  *schema,
    IcebergPartitionSpec *partitionSpec,
    IcebergSortOrder     *writeOrder,
    IcebergProperties    *properties);

/* 将 TableMetadata 序列化为 JSON 并写入 S3，返回新的 metadata_location */
char *IcebergSDK_WriteTableMetadata(
    IcebergTableMetadata *metadata,
    const char *baseLocation);

/*────────────────────────────────────────────────────────────
 * Namespace / 对象存储路径操作
 *────────────────────────────────────────────────────────────*/

/* 确保 namespace 在对象存储上的基础路径可用 */
bool IcebergSDK_CreateNamespaceLocation(const char *namespaceBaseLocation, char **errorMsg);

/* 清理 namespace 在对象存储上的 marker（best-effort） */
void IcebergSDK_DeleteNamespaceLocation(const char *namespaceBaseLocation);

/*────────────────────────────────────────────────────────────
 * Schema 操作
 *────────────────────────────────────────────────────────────*/

/* 解析 schema JSONB → IcebergSchema */
IcebergSchema *IcebergSDK_ParseSchema(Jsonb *schemaJson);

/* 在现有 Schema 基础上追加一个列，返回新的 Schema */
IcebergSchema *IcebergSDK_AddColumnToSchema(
    IcebergSchema *baseSchema,
    const char    *columnName,
    const char    *columnType,
    const char    *columnDoc,
    int           *outNewFieldId);

/* 将 IcebergSchema 序列化为 JSONB */
Jsonb *IcebergSDK_SchemaToJsonb(IcebergSchema *schema);

/*────────────────────────────────────────────────────────────
 * Snapshot 操作
 *────────────────────────────────────────────────────────────*/

/* 构造 Snapshot 结构 */
IcebergSnapshot *IcebergSDK_BuildSnapshot(
    int64_t  snapshotId,
    int64_t  timestampMs,
    const char *manifestList,
    IcebergSnapshotSummary *summary,
    int      schemaId,
    int64_t  parentSnapshotId);

/* 基于 base metadata 应用 requirements + updates，返回新 TableMetadata */
IcebergTableMetadata *IcebergSDK_ApplyUpdates(
    IcebergTableMetadata *base,
    Jsonb *requirements,
    Jsonb *updates);

/*────────────────────────────────────────────────────────────
 * 工具
 *────────────────────────────────────────────────────────────*/

/* 校验 Iceberg 类型字符串是否合法（含 fixed(L) 检测） */
bool IcebergSDK_ValidateType(const char *typeString, char **errorMsg);

/* 生成新的 UUID */
char *IcebergSDK_GenerateUUID(void);

/* 生成新的 Snapshot ID */
int64_t IcebergSDK_NewSnapshotId(void);
```

### 5.2 接口与函数的对应关系

| SDK 接口 | 使用该接口的 SQL 函数 |
|----------|---------------------|
| `CreateNamespaceLocation` | `create_namespace` |
| `DeleteNamespaceLocation` | `drop_namespace` |
| `ReadTableMetadata` | `load_table`, `add_column`, `commit_table` |
| `CreateTableMetadata` | `create_table` |
| `WriteTableMetadata` | `create_table`, `commit_table`, `add_column` |
| `ParseSchema` | `create_table`, `add_column` |
| `AddColumnToSchema` | `add_column` |
| `SchemaToJsonb` | `load_table` |
| `BuildSnapshot` | `commit_table` |
| `ApplyUpdates` | `commit_table`, `add_column` |
| `ValidateType` | `create_table`, `add_column` |
| `GenerateUUID` | `create_table` |
| `NewSnapshotId` | `commit_table` |

---

## 6. 元信息表操作诉求

所有元信息表的读写通过 C++ 实现函数中的 **SPI** 执行 SQL 完成。SPI 操作与外围 SQL 调用处于同一事务（参见第 3 章）。

### 6.1 表操作映射

| 元信息表 | 读（SPI SELECT） | 写（SPI INSERT/UPDATE/DELETE） |
|----------|-----------------|-------------------------------|
| `namespaces` | `list_namespaces`, `load_namespace`, `is_namespace_existed` | `create_namespace`, `drop_namespace`, `update_namespace_properties` |
| `tables_internal` | `list_tables`, `load_table`, `is_table_existed` | `create_table`, `drop_table`, `commit_table`, `rename_table`, `add_column` |
| `table_schemas` | `load_table` | `create_table`, `add_column` |
| `snapshots` | `load_table` | `create_table`, `commit_table` |
| `partition_specs` | `load_table` | `create_table` |
| `iceberg_tables`（视图） | （供 Spark/PyIceberg JDBC Catalog 查询） | — |

### 6.2 关键约束

1. **`tables_internal.metadata_location` 是线性一致点**：所有元数据变更最终通过更新此字段完成。commit_table 的原子性依赖 `UPDATE ... WHERE metadata_location = ?` 实现乐观锁。

2. **table_uuid 外键级联**：`table_schemas`、`snapshots`、`partition_specs` 通过 `table_uuid` 关联 `tables_internal`，`ON DELETE CASCADE` 保证删表时自动清理。

3. **namespace 外键约束**：`tables_internal` 的 `namespace` 引用 `namespaces.namespace`，`ON DELETE RESTRICT` 阻止在有子表的情况下删除 namespace。

4. **table_name 唯一性**：同一 `(namespace, table_name)` 在 `tables_internal` 中唯一（PRIMARY KEY 约束）。

---

## 7. 函数实现设计

以下按复杂度分层描述 14 个函数的实现。每个函数包含 **流程图** 和关键操作说明。

### 流程图符号说明

```
┌──────────────────────┐
│ 操作步骤              │  矩形：处理步骤（标注 SPI/SDK/内部计算）
└──────────┬───────────┘
           │
           ▼
     ╔══════════╗
     ║  条件?    ║          菱形：判断/分支
     ╚══════╤═══╝
        ┌───┴───┐
        ▼       ▼
      是       否
       │       │
       ▼       ▼
   ┌──────┐ ┌──────────┐
   │ ...  │ │ RAISE    │  六边形：错误抛出 (ereport ERROR)
   └──────┘ │ P000X    │
            └──────────┘

   ┌─────────────┐
   │ RETURN ...  │          圆角矩形：返回
   └─────────────┘
```

---

### 7.1 简单查询类（仅 SPI 读操作）

#### 7.1.1 is_namespace_existed

```
is_namespace_existed(p_namespace TEXT) → JSONB
```

```
                 ┌──────────────┐
                 │ 提取参数      │
                 │ p_namespace  │
                 └──────┬───────┘
                        │
                        ▼
                 ╔══════════════╗
                 ║ NULL 或空串?  ║
                 ╚══════╤═══════╝
                    ┌───┴───┐
                    ▼       ▼
                   是      否
                    │       │
                    ▼       ▼
              ┌──────────┐ ┌─────────────────────────────────┐
              │ RAISE    │ │ SPI: SELECT 1                    │
              │ P0001    │ │ FROM iceberg_catalog.namespaces  │
              │ 参数无效  │ │ WHERE namespace = $1             │
              └──────────┘ └──────────────┬──────────────────┘
                                          │
                                          ▼
                                   ╔══════════╗
                                   ║ 查到记录?  ║
                                   ╚══════╤═══╝
                                      ┌───┴───┐
                                      ▼       ▼
                                     是      否
                                      │       │
                                      ▼       ▼
                              ┌───────────────────┐
                              │ RETURN            │
                              │ {"exists": true}  │
                              └───────────────────┘
                                      │       │
                                      │       ▼
                                      │ ┌───────────────────┐
                                      │ │ RETURN            │
                                      │ │ {"exists": false} │
                                      │ └───────────────────┘
                                      ▼
                                    结束
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | 检查 p_namespace 非 NULL 且非空字符串 |
| 存在性检查 | SPI | `SELECT 1 FROM iceberg_catalog.namespaces WHERE namespace = $1` |
| 返回 | 内部 | 存在返回 `{"exists": true}`，不存在返回 `{"exists": false}` |

---

#### 7.1.2 is_table_existed

```
is_table_existed(p_namespace TEXT, p_table TEXT) → JSONB
```

```
            ┌─────────────────────────┐
            │ 提取参数                  │
            │ p_namespace, p_table     │
            └───────────┬─────────────┘
                        │
                        ▼
                 ╔══════════════╗
                 ║ 任一为 NULL   ║
                 ║ 或空串?       ║
                 ╚══════╤═══════╝
                    ┌───┴───┐
                    ▼       ▼
                   是      否
                    │       │
                    ▼       ▼
              ┌──────────┐ ┌─────────────────────────────────┐
              │ RAISE    │ │ SPI: SELECT 1                    │
              │ P0001    │ │ FROM iceberg_catalog              │
              │ 参数无效  │ │   .tables_internal               │
              └──────────┘ │ WHERE namespace = $1              │
                           │   AND table_name = $2            │
                           └──────────────┬──────────────────┘
                                          │
                                          ▼
                                   ╔══════════╗
                                   ║ 查到记录?  ║
                                   ╚══════╤═══╝
                                      ┌───┴───┐
                                      ▼       ▼
                                     是      否
                                      │       │
                                      ▼       ▼
                              ┌───────────────────┐
                              │ RETURN            │
                              │ {"exists": true}  │
                              └───────────────────┘
                                      │       │
                                      │       ▼
                                      │ ┌───────────────────┐
                                      │ │ RETURN            │
                                      │ │ {"exists": false} │
                                      │ └───────────────────┘
                                      ▼
                                    结束
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | 检查 p_namespace、p_table 非 NULL 且非空字符串 |
| 存在性检查 | SPI | `SELECT 1 FROM iceberg_catalog.tables_internal WHERE namespace = $1 AND table_name = $2` |
| 返回 | 内部 | 同 is_namespace_existed 模式 |

---

#### 7.1.3 load_namespace

```
load_namespace(p_namespace TEXT) → JSONB
```

```
            ┌──────────────┐
            │ 提取参数      │
            │ p_namespace  │
            └──────┬───────┘
                   │
                   ▼
            ╔══════════════╗
            ║ NULL 或空串?  ║
            ╚══════╤═══════╝
               ┌───┴───┐
               ▼       ▼
              是      否
               │       │
               ▼       ▼
         ┌──────────┐ ┌─────────────────────────────────┐
         │ RAISE    │ │ SPI: SELECT namespace, properties │
         │ P0001    │ │ FROM iceberg_catalog.namespaces  │
         │ 参数无效  │ │ WHERE namespace = $1             │
         └──────────┘ └──────────────┬──────────────────┘
                                     │
                                     ▼
                              ╔══════════╗
                              ║ 查到记录?  ║
                              ╚══════╤═══╝
                                 ┌───┴───┐
                                 ▼       ▼
                                是      否
                                 │       │
                                 ▼       ▼
                    ┌─────────────────────┐ ┌──────────┐
                    │ 内部: 构造 JSONB     │ │ RAISE    │
                    │ jsonb_build_object(  │ │ P0004    │
                    │   'namespace', ...,  │ │ 资源不存在│
                    │   'properties', ...) │ └──────────┘
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │ RETURN JSONB        │
                    └─────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | 检查 p_namespace |
| 查询 | SPI | `SELECT namespace, properties FROM iceberg_catalog.namespaces WHERE namespace = $1` |
| 返回构造 | 内部 | `{"namespace": ["xxx"], "properties": {...}}` |

---

#### 7.1.4 list_namespaces

```
list_namespaces(p_parent TEXT DEFAULT NULL, p_page_size INTEGER DEFAULT 1000, p_page_token TEXT DEFAULT NULL) → JSONB
```

```
         ┌───────────────────────────────────────────┐
         │ 提取参数                                    │
         │ p_parent, p_page_size, p_page_token        │
         └──────────────────┬────────────────────────┘
                            │
                            ▼
                     ╔══════════════╗
                     ║ p_page_size  ║
                     ║ < 1 ?        ║
                     ╚══════╤═══════╝
                        ┌───┴───┐
                        ▼       ▼
                       是      否
                        │       │
                        ▼       ▼
                  ┌──────────┐ ╔══════════════╗
                  │ RAISE    │ ║ p_parent 非空?║
                  │ P0001    │ ╚══════╤═══════╝
                  └──────────┘    ┌───┴───┐
                                  ▼       ▼
                                 是      否
                                  │       │
                                  ▼       ▼
                           ╔═══════════╗  (跳过父级检查)
                           ║ p_parent   ║    │
                           ║ 存在?      ║    │
                           ║ (SPI)      ║    │
                           ╚══════╤════╝    │
                              ┌───┴───┐     │
                              ▼       ▼     │
                             是      否     │
                              │       │     │
                              ▼       ▼     │
                             (继续) RAISE   │
                                    P0004   │
                                     │      │
                                     ▼      ▼
                          ┌─────────────────────────────────┐
                          │ 内部: 解码 p_page_token → offset  │
                          │ (若 NULL 则 offset = 0)           │
                          └──────────────┬──────────────────┘
                                         │
                                         ▼
                          ┌─────────────────────────────────┐
                          │ SPI: SELECT namespace            │
                          │ FROM iceberg_catalog.namespaces  │
                          │ ORDER BY namespace               │
                          │ LIMIT p_page_size + 1            │
                          │ OFFSET offset                    │
                          └──────────────┬──────────────────┘
                                         │
                                         ▼
                          ┌─────────────────────────────────┐
                          │ 内部: 判断是否有下一页             │
                          │ 若结果数 > p_page_size:           │
                          │   next_token = encode(offset+size)│
                          │   截断结果到 p_page_size           │
                          │ 否则: next_token = NULL           │
                          └──────────────┬──────────────────┘
                                         │
                                         ▼
                          ┌─────────────────────────────────┐
                          │ RETURN jsonb_build_object(       │
                          │   'namespaces', [...],           │
                          │   'next-page-token', token)      │
                          └─────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | p_page_size >= 1 |
| 父级检查 | SPI | 若 p_parent 非空：`SELECT 1 FROM namespaces WHERE namespace = $1`，不存在 → P0004 |
| 分页查询 | SPI | `SELECT namespace FROM namespaces ORDER BY namespace LIMIT size+1 OFFSET offset` |
| 分页编码 | 内部 | LIMIT size+1 判断是否有下页；token = base64(offset+size) |
| 返回 | 内部 | `{"namespaces": [...], "next-page-token": ...}` |

---

#### 7.1.5 list_tables

```
list_tables(p_namespace TEXT, p_page_size INTEGER DEFAULT 1000, p_page_token TEXT DEFAULT NULL) → JSONB
```

```
         ┌───────────────────────────────────────────┐
         │ 提取参数                                    │
         │ p_namespace, p_page_size, p_page_token     │
         └──────────────────┬────────────────────────┘
                            │
                            ▼
                     ╔══════════════╗
                     ║ 参数校验      ║
                     ║ namespace非空?║
                     ║ page_size>=1? ║
                     ╚══════╤═══════╝
                        ┌───┴───┐
                        ▼       ▼
                       否      是
                        │       │
                        ▼       ▼
                  ┌──────────┐ ┌─────────────────────────────────┐
                  │ RAISE    │ │ SPI: SELECT 1 FROM namespaces    │
                  │ P0001    │ │ WHERE namespace = $1             │
                  └──────────┘ │ → 不存在则 P0004                 │
                               └──────────────┬──────────────────┘
                                              │
                                              ▼
                               ┌─────────────────────────────────┐
                               │ 内部: 解码 p_page_token → offset  │
                               └──────────────┬──────────────────┘
                                              │
                                              ▼
                               ┌─────────────────────────────────┐
                               │ SPI: SELECT namespace, table_name │
                               │ FROM tables_internal             │
                               │ WHERE namespace = $1             │
                               │ ORDER BY table_name              │
                               │ LIMIT p_page_size + 1            │
                               │ OFFSET offset                    │
                               └──────────────┬──────────────────┘
                                              │
                                              ▼
                               ┌─────────────────────────────────┐
                               │ 内部: 判断下页 + 构造 identifiers │
                               │ [{"namespace": [ns], "name": t}]  │
                               └──────────────┬──────────────────┘
                                              │
                                              ▼
                               ┌─────────────────────────────────┐
                               │ RETURN ListTablesResponse JSONB  │
                               └─────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | p_namespace 非空，p_page_size >= 1 |
| namespace 检查 | SPI | `SELECT 1 FROM namespaces WHERE namespace = $1` → 不存在 P0004 |
| 分页查询 | SPI | `SELECT namespace, table_name FROM tables_internal WHERE namespace = $1 ORDER BY table_name LIMIT size+1 OFFSET offset` |
| 返回 | 内部 | `{"identifiers": [...], "next-page-token": ...}` |

---

### 7.2 简单写入类（SPI 读写 + 简单逻辑）

#### 7.2.1 create_namespace

```
create_namespace(p_namespace TEXT, p_properties JSONB DEFAULT NULL) → JSONB
```

```
         ┌──────────────────────────────────────┐
         │ 提取参数: p_namespace, p_properties    │
         └──────────────────┬───────────────────┘
                            │
                            ▼
                     ╔══════════════╗
                     ║ p_namespace   ║
                     ║ NULL / 空串?  ║
                     ╚══════╤═══════╝
                        ┌───┴───┐
                        ▼       ▼
                       是      否
                        │       │
                        ▼       ▼
                  ┌──────────┐ ╔══════════════╗
                  │ RAISE    │ ║ p_properties  ║
                  │ P0001    │ ║ 格式合法?     ║
                  └──────────┘ ╚══════╤═══════╝
                                   ┌───┴───┐
                                   ▼       ▼
                                  否      是
                                   │       │
                                   ▼       ▼
                             ┌──────────┐ ┌─────────────────────────────┐
                             │ RAISE    │ │ SPI: SELECT 1 FROM namespaces│
                             │ P0001    │ │ WHERE namespace = $1         │
                             └──────────┘ └──────────────┬──────────────┘
                                                         │
                                                         ▼
                                                  ╔══════════════╗
                                                  ║ 已存在?       ║
                                                  ╚══════╤═══════╝
                                                     ┌───┴───┐
                                                     ▼       ▼
                                                    是      否
                                                     │       │
                                                     ▼       ▼
                                               ┌──────────┐ ┌─────────────────────────────┐
                                               │ RAISE    │ │ 内部: 确定 namespace 基础路径 │
                                               │ P0005    │ │ 若有 location 属性→直接使用    │
                                               │ 已存在    │ │ 否则→DefaultPrefix/namespace  │
                                               └──────────┘ └──────────────┬──────────────┘
                                                                           │
                                                                           ▼
                                                                ┌─────────────────────────────┐
                                                                │ SDK: CreateNamespaceLocation │
                                                                │ (namespaceBaseLocation)      │
                                                                │ → 失败则 P0009               │
                                                                └──────────────┬──────────────┘
                                                                               │
                                                                               ▼
                                                                ┌─────────────────────────────┐
                                                                │ SPI: INSERT INTO namespaces  │
                                                                │ (namespace, properties)      │
                                                                │ VALUES ($1, COALESCE($2,'{}))│
                                                                │ 若用户未指定 location,         │
                                                                │ 将 baseLocation 写入 properties│
                                                                └──────────────┬──────────────┘
                                                                               │
                                                                               ▼
                                                                ┌─────────────────────────────┐
                                                                │ RETURN CreateNamespaceResponse│
                                                                │ {"namespace": [...],         │
                                                                │  "properties": {...}}        │
                                                                └─────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | p_namespace 非空；p_properties 若非 NULL 须为合法 JSONB object |
| 存在性检查 | SPI | `SELECT 1 FROM namespaces WHERE namespace = $1` → 已存在 P0005 |
| 确定路径 | 内部 | 若 p_properties 含 `location` → 使用；否则 `{DefaultPrefix}/{namespace}` |
| S3 可达性 | SDK | `IcebergSDK_CreateNamespaceLocation(baseLocation)` → 失败 P0009 |
| 写入 | SPI | `INSERT INTO namespaces (namespace, properties) VALUES (ns, props)` |
| 返回 | 内部 | `{"namespace": [ns], "properties": ...}` |

---

#### 7.2.2 drop_namespace

```
drop_namespace(p_namespace TEXT) → JSONB
```

```
            ┌──────────────┐
            │ 提取参数      │
            │ p_namespace  │
            └──────┬───────┘
                   │
                   ▼
            ╔══════════════╗
            ║ NULL 或空串?  ║
            ╚══════╤═══════╝
               ┌───┴───┐
               ▼       ▼
              是      否
               │       │
               ▼       ▼
         ┌──────────┐ ┌─────────────────────────────────┐
         │ RAISE    │ │ SPI: SELECT 1 FROM namespaces    │
         │ P0001    │ │ WHERE namespace = $1             │
         └──────────┘ │ → 不存在则 P0004                 │
                      └──────────────┬──────────────────┘
                                     │
                                     ▼
                              ╔══════════════╗
                              ║ 存在?         ║
                              ╚══════╤═══════╝
                                 ┌───┴───┐
                                 ▼       ▼
                                否      是
                                 │       │
                                 ▼       ▼
                           ┌──────────┐ ┌──────────────────────────────┐
                           │ RAISE    │ │ SPI: SELECT 1                │
                           │ P0004    │ │ FROM tables_internal         │
                           └──────────┘ │ WHERE namespace = $1 LIMIT 1 │
                                        └──────────────┬───────────────┘
                                                       │
                                                       ▼
                                                ╔══════════════╗
                                                ║ 有子表?       ║
                                                ╚══════╤═══════╝
                                                   ┌───┴───┐
                                                   ▼       ▼
                                                  是      否
                                                   │       │
                                                   ▼       ▼
                                             ┌──────────┐ ┌──────────────────────────────┐
                                             │ RAISE    │ │ SPI: DELETE FROM namespaces  │
                                             │ P0005    │ │ WHERE namespace = $1         │
                                             │ 非空     │ └──────────────┬───────────────┘
                                             └──────────┘                │
                                                                         ▼
                                                              ┌──────────────────────────────┐
                                                              │ SDK: DeleteNamespaceLocation │
                                                              │ (best-effort, 忽略失败)       │
                                                              └──────────────┬───────────────┘
                                                                             │
                                                                             ▼
                                                              ┌──────────────────────────────┐
                                                              │ RETURN {"success": true}     │
                                                              └──────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | p_namespace 非空 |
| 存在性检查 | SPI | `SELECT 1 FROM namespaces WHERE namespace = $1` → 不存在 P0004 |
| 子表检查 | SPI | `SELECT 1 FROM tables_internal WHERE namespace = $1 LIMIT 1` → 有子表 P0005 |
| 删除 | SPI | `DELETE FROM namespaces WHERE namespace = $1` |
| S3 清理 | SDK | `IcebergSDK_DeleteNamespaceLocation(...)` (best-effort) |
| 返回 | 内部 | `{"success": true}` |

---

#### 7.2.3 rename_table

```
rename_table(p_source_namespace, p_source_table, p_dest_namespace, p_dest_table TEXT) → JSONB
```

```
 ┌───────────────────────────────────────────────┐
 │ 提取参数: src_ns, src_tbl, dst_ns, dst_tbl    │
 └──────────────────┬────────────────────────────┘
                    │
                    ▼
             ╔══════════════╗
             ║ 任一为 NULL   ║
             ║ 或空串?       ║
             ╚══════╤═══════╝
                ┌───┴───┐
                ▼       ▼
               是      否
                │       │
                ▼       ▼
          ┌──────────┐ ┌─────────────────────────────────┐
          │ RAISE    │ │ SPI: SELECT 1 FROM tables_internal│
          │ P0001    │ │ WHERE namespace=$1 AND table=$2   │
          └──────────┘ │ (检查源表存在)                    │
                       │ → 不存在则 P0004                 │
                       └──────────────┬──────────────────┘
                                      │
                                      ▼
                       ┌─────────────────────────────────┐
                       │ SPI: SELECT 1 FROM namespaces    │
                       │ WHERE namespace = dest_ns        │
                       │ → 不存在则 P0004                 │
                       └──────────────┬──────────────────┘
                                      │
                                      ▼
                       ┌─────────────────────────────────┐
                       │ SPI: SELECT 1 FROM tables_internal│
                       │ WHERE namespace=$3 AND table=$4   │
                       │ → 已存在则 P0005                 │
                       └──────────────┬──────────────────┘
                                      │
                                      ▼
                       ┌─────────────────────────────────┐
                       │ SPI: UPDATE tables_internal      │
                       │ SET namespace = dest_ns,         │
                       │     table_name = dest_tbl        │
                       │ WHERE namespace = src_ns         │
                       │   AND table_name = src_tbl       │
                       └──────────────┬──────────────────┘
                                      │
                                      ▼
                       ┌─────────────────────────────────┐
                       │ DDL管理模块: 更新外表和delta表信息 │
                       └──────────────┬──────────────────┘
                                      │
                                      ▼
                       ┌─────────────────────────────────┐
                       │ RETURN {"success": true}         │
                       └─────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | 所有参数非空 |
| 源表检查 | SPI | `SELECT 1 FROM tables_internal WHERE namespace=src_ns AND table_name=src_tbl` → 不存在 P0004 |
| 目标NS检查 | SPI | `SELECT 1 FROM namespaces WHERE namespace=dest_ns` → 不存在 P0004 |
| 目标冲突 | SPI | `SELECT 1 FROM tables_internal WHERE namespace=dest_ns AND table_name=dest_tbl` → 已存在 P0005 |
| 更新 | SPI | `UPDATE tables_internal SET namespace=dest_ns, table_name=dest_tbl WHERE namespace=src_ns AND table_name=src_tbl` |
| DDL | DDL模块 | 更新外表和 delta 表信息 |
| 返回 | 内部 | `{"success": true}` |

---

#### 7.2.4 update_namespace_properties

```
update_namespace_properties(p_namespace TEXT, p_removals JSONB DEFAULT NULL, p_updates JSONB DEFAULT NULL) → JSONB
```

```
 ┌───────────────────────────────────────────────┐
 │ 提取参数: p_namespace, p_removals, p_updates   │
 └──────────────────┬────────────────────────────┘
                    │
                    ▼
             ╔══════════════╗
             ║ 参数校验:      ║
             ║ ns非空?       ║
             ║ removals/updates║
             ║ 不同时为空?    ║
             ║ 无交集?       ║
             ╚══════╤═══════╝
                ┌───┴───┐
                ▼       ▼
               否      是
                │       │
                ▼       ▼
          ┌──────────┐ ┌──────────────────────────────────┐
          │ RAISE    │ │ SPI: SELECT properties            │
          │ P0001/6  │ │ FROM namespaces                   │
          └──────────┘ │ WHERE namespace = $1 FOR UPDATE   │
                       │ → 不存在则 P0004                  │
                       └──────────────┬───────────────────┘
                                      │
                                      ▼
                       ┌──────────────────────────────────┐
                       │ 内部: 计算新 properties            │
                       │ - removals[]: 移除当前properties  │
                       │   中的指定键 (不在的→missing[])    │
                       │ - updates[]: 设置/更新指定键值     │
                       └──────────────┬───────────────────┘
                                      │
                                      ▼
                       ┌──────────────────────────────────┐
                       │ SPI: UPDATE namespaces            │
                       │ SET properties = newProperties    │
                       │ WHERE namespace = $1              │
                       └──────────────┬───────────────────┘
                                      │
                                      ▼
                       ┌──────────────────────────────────┐
                       │ RETURN UpdateNamespaceProperties  │
                       │ Response: {updated,removed,missing}│
                       └──────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | ns 非空；removals/updates 至少一个非空；removals ∩ updates = ∅ |
| 锁定读取 | SPI | `SELECT properties FROM namespaces WHERE namespace=$1 FOR UPDATE` → 不存在 P0004 |
| 计算 | 内部 | 基于 currentProperties，移除 removals 中的键，设置/新增 updates 中的键 |
| 更新 | SPI | `UPDATE namespaces SET properties = new WHERE namespace = $1` |
| 返回 | 内部 | `{"updated": [...], "removed": [...], "missing": [...]}` |

---

### 7.3 复杂写入类（SPI + SDK + 复杂逻辑）

#### 7.3.1 create_table

```
create_table(
    p_namespace TEXT, p_table_name TEXT, p_schema JSONB,
    p_location TEXT DEFAULT NULL, p_partition_spec JSONB DEFAULT NULL,
    p_write_order JSONB DEFAULT NULL, p_stage_create BOOLEAN DEFAULT FALSE,
    p_properties JSONB DEFAULT NULL
) → JSONB
```

```
┌──────────────────────────────────────────────────────────────────┐
│ 提取全部 8 个参数                                                  │
└────────────────────────────┬─────────────────────────────────────┘
                             │
                             ▼
                      ╔══════════════╗
                      ║ 参数校验:      ║──RAISE P0001──►
                      ║ ns/name非空?  ║  参数无效
                      ║ schema格式合法?║
                      ║ fields类型校验 ║
                      ║ (ValidateType) ║
                      ╚══════╤═══════╝
                             │ 通过
                             ▼
                ┌────────────────────────────────┐
                │ SPI: SELECT 1 FROM namespaces  │
                │ WHERE namespace = p_namespace  │──RAISE P0004──►
                └───────────────┬────────────────┘  Namespace不存在
                                │ 存在
                                ▼
                ┌────────────────────────────────────────────────┐
                │ SPI: SELECT 1 FROM tables_internal             │
                │ WHERE namespace=$1 AND table_name=$2           │──RAISE P0005──►
                └───────────────┬────────────────────────────────┘  表已存在
                                │ 不存在
                                ▼
                ┌────────────────────────────────────────────────┐
                │ 内部: 确定 table_location                       │
                │ p_location 有值? → 直接使用                      │
                │ 否则: 从namespace properties读location           │
                │   → 有: {nsLocation}/{table_name}               │
                │   → 无: {DefaultPrefix}/{ns}/{table_name}      │
                │ 校验 S3 URI 格式                                │──RAISE P0001──►
                └───────────────┬────────────────────────────────┘
                                │
                                ▼
                ┌────────────────────────────────────────────────┐
                │ SDK: GenerateUUID() → table_uuid                │
                │ SDK: ParseSchema(p_schema) → IcebergSchema      │
                │ SDK: ParsePartitionSpec(p_partition_spec)        │
                │ SDK: CreateTableMetadata(                       │
                │   uuid, location, schema, partition,            │
                │   writeOrder, properties) → TableMetadata       │
                └───────────────┬────────────────────────────────┘
                                │
                                ▼
                ┌────────────────────────────────────────────────┐
                │ SDK: WriteTableMetadata(metadata, tableLocation)│──RAISE P0009──►
                │ → metadataLocation (S3路径)                     │  S3写入失败
                └───────────────┬────────────────────────────────┘
                                │
                                ▼
                ┌────────────────────────────────────────────────┐
                │ SPI: INSERT INTO tables_internal                │
                │ (relid, namespace, table_name, table_uuid,      │
                │  metadata_location, table_location,             │
                │  last_column_id, current_schema_id,             │
                │  current_snapshot_id, default_spec_id)          │
                └───────────────┬────────────────────────────────┘
                                │
                                ▼
                ┌────────────────────────────────────────────────┐
                │ SPI: INSERT INTO table_schemas                  │
                │ (table_uuid, schema_id, field_position,         │
                │  field_id, field_name, field_required,         │
                │  field_type, field_doc)                         │
                │ ← 展开 schema 的每个 field                       │
                └───────────────┬────────────────────────────────┘
                                │
                                ▼
                        ╔══════════════╗
                        ║ 有分区?       ║
                        ╚══════╤═══════╝
                           ┌───┴───┐
                           ▼       ▼
                          是      否
                           │       │
                           ▼       ▼
              ┌───────────────────────┐ (跳过)
              │ SPI: INSERT INTO      │  │
              │ partition_specs (...) │  │
              │ ← 展开分区 spec        │  │
              └───────────┬───────────┘  │
                          │              │
                          ▼              ▼
              ┌────────────────────────────────────────┐
              │ DDL管理模块:                               │
              │ - 创建 delta 表 (本地 Heap/Ustore)          │
              │ - 创建 FDW 外表                             │
              └───────────────┬────────────────────────────┘
                              │
                              ▼
              ┌────────────────────────────────────────┐
              │ RETURN LoadTableResult JSONB            │
              │ (metadata-location, metadata, config)   │
              └────────────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部+SDK | ns/name 非空；schema type="struct"；遍历 fields `ValidateType` 拒绝 unsupported |
| 检查 NS | SPI | `SELECT 1 FROM namespaces WHERE namespace = $1` |
| 检查冲突 | SPI | `SELECT 1 FROM tables_internal WHERE namespace=$1 AND table_name=$2` |
| 确定路径 | 内部+SPI | 优先级：p_location → ns.properties.location → DefaultPrefix |
| 构造元数据 | SDK | `GenerateUUID` + `ParseSchema` + `CreateTableMetadata` |
| 写 S3 | SDK | `WriteTableMetadata` → metadata_location |
| 写入元信息表 | SPI | INSERT 到 tables_internal, table_schemas, partition_specs |
| 创建存储 | DDL模块 | 创建 delta 表和 FDW 外表 |
| 返回 | 内部 | LoadTableResult 格式 JSONB |

**设计要点**：SDK 写 S3（步骤 WriteTableMetadata）先于 SPI INSERT（步骤写元信息表）。原因：如果 SPI INSERT 先成功、S3 写再失败，元信息表会指向不存在的 S3 文件。反过来，S3 写成功但 SPI INSERT 失败（事务回滚），S3 文件成为孤儿（由 vacuum 清理）——这是更安全的顺序。

---

#### 7.3.2 load_table

```
load_table(p_namespace TEXT, p_table TEXT) → JSONB
```

```
        ┌──────────────────────────┐
        │ 提取参数                   │
        │ p_namespace, p_table      │
        └────────────┬─────────────┘
                     │
                     ▼
              ╔══════════════╗
              ║ 任一为 NULL   ║────RAISE P0001──►
              ║ 或空串?       ║
              ╚══════╤═══════╝
                     │ 否
                     ▼
        ┌─────────────────────────────────────┐
        │ SPI: SELECT metadata_location,       │
        │      table_uuid, table_location,     │
        │      last_column_id, ...            │
        │ FROM tables_internal                │──RAISE P0004──►
        │ WHERE namespace=$1 AND table=$2     │  表/NS不存在
        └──────────────┬──────────────────────┘
                       │ 存在
                       ▼
        ┌─────────────────────────────────────┐
        │ SDK: ReadTableMetadata(             │
        │   metadata_location)                │──RAISE P0009──►
        │ → TableMetadata 结构                │  SDK读取失败
        └──────────────┬──────────────────────┘
                       │
                       ▼
        ┌─────────────────────────────────────┐
        │ 内部: 将 TableMetadata 序列化为       │
        │ LoadTableResult JSONB:               │
        │   metadata-location                  │
        │   metadata: {                        │
        │     format-version, table-uuid,      │
        │     location, schemas[],            │
        │     partition-specs[], snapshots[], │
        │     current-schema-id, ...}          │
        │   config: {} (凭证注入模块填充)        │
        └──────────────┬──────────────────────┘
                       │
                       ▼
        ┌─────────────────────────────────────┐
        │ RETURN JSONB (LoadTableResult)       │
        └─────────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | ns/table 非空 |
| 获取指针 | SPI | `SELECT metadata_location, ... FROM tables_internal WHERE ...` |
| 读取元数据 | SDK | `ReadTableMetadata(metadata_location)` — 从 S3 读取并解析 metadata.json |
| 构造返回 | 内部 | 将 TableMetadata 序列化为完整的 LoadTableResult JSONB |
| 返回 | 内部 | JSONB 包含 metadata-location, metadata, config |

---

#### 7.3.3 drop_table

```
drop_table(p_namespace TEXT, p_table TEXT, p_purge BOOLEAN DEFAULT FALSE) → JSONB
```

```
        ┌──────────────────────────────────┐
        │ 提取参数                           │
        │ p_namespace, p_table, p_purge     │
        └────────────┬─────────────────────┘
                     │
                     ▼
              ╔══════════════╗
              ║ 参数校验       ║────RAISE P0001──►
              ║ ns/name非空?  ║
              ╚══════╤═══════╝
                     │ 否
                     ▼
        ┌──────────────────────────────────────┐
        │ SPI: SELECT metadata_location,        │
        │      table_uuid, table_location       │
        │ FROM tables_internal                 │──RAISE P0004──►
        │ WHERE namespace=$1 AND table=$2       │  表不存在
        │ FOR UPDATE                           │
        └──────────────┬───────────────────────┘
                       │ 存在
                       ▼
                ╔══════════════╗
                ║ p_purge?      ║
                ╚══════╤═══════╝
                   ┌───┴───┐
                   ▼       ▼
                  是      否
                   │       │
                   ▼       ▼
          ┌──────────────────────────┐ (跳过 purge)
          │ RAISE P0008              │  │
          │ purge 功能暂不实现        │  │
          └──────────────────────────┘  │
                                        │
                                        ▼
                          ┌──────────────────────────────────────┐
                          │ DDL管理模块:                           │
                          │ - 删除 FDW 外表                       │
                          │ - 删除 delta 表 (本地 Heap/Ustore)     │
                          └──────────────┬───────────────────────┘
                                         │
                                         ▼
                          ┌──────────────────────────────────────┐
                          │ SPI: DELETE FROM tables_internal      │
                          │ WHERE namespace=$1 AND table=$2       │
                          │ (ON DELETE CASCADE 自动清理子表)       │
                          └──────────────┬───────────────────────┘
                                         │
                                         ▼
                          ┌──────────────────────────────────────┐
                          │ RETURN {"success": true}              │
                          └──────────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | ns/table 非空 |
| 锁定读取 | SPI | `SELECT ... FROM tables_internal WHERE ... FOR UPDATE` |
| purge 判断 | 内部 | p_purge=TRUE → P0008（暂不实现） |
| DDL | DDL模块 | 删除外表和 delta 表 |
| 删除 | SPI | `DELETE FROM tables_internal WHERE ...`（CASCADE 清理子表） |
| 返回 | 内部 | `{"success": true}` |

---

#### 7.3.4 commit_table

```
commit_table(p_namespace TEXT, p_table TEXT, p_requirements JSONB, p_updates JSONB) → JSONB
```

```
┌──────────────────────────────────────────────────────────┐
│ 提取参数: p_namespace, p_table, p_requirements, p_updates │
└──────────────────────────┬───────────────────────────────┘
                           │
                           ▼
                    ╔══════════════╗
                    ║ 参数校验:      ║
                    ║ 所有非NULL/空? ║──RAISE P0001──►
                    ║ updates仅含    ║  参数无效
                    ║ add-snapshot?  ║
                    ╚══════╤═══════╝
                           │ 通过
                           ▼
              ┌───────────────────────────────────────────┐
              │ SPI: SELECT metadata_location,             │
              │      table_uuid, table_location,           │
              │      current_snapshot_id                  │
              │ FROM tables_internal                      │──RAISE P0004──►
              │ WHERE namespace=$1 AND table=$2           │  表不存在
              │ FOR UPDATE  ← 行锁，串行化并发写             │
              └──────────────┬────────────────────────────┘
                             │ 锁定成功
                             ▼
              ┌───────────────────────────────────────────┐
              │ SDK: ReadTableMetadata(metadata_location)  │──RAISE P0009──►
              │ → currentMetadata (TableMetadata 结构)      │  SDK读取失败
              └──────────────┬────────────────────────────┘
                             │
                             ▼
              ┌───────────────────────────────────────────┐
              │ SDK: ApplyUpdates(                         │
              │   currentMetadata,                         │
              │   p_requirements,                          │──RAISE P0005──►
              │   p_updates)                               │  校验失败/冲突
              │ → newMetadata (TableMetadata 结构)          │
              │   (内部校验 requirements + 追加 snapshot)    │
              └──────────────┬────────────────────────────┘
                             │ 成功
                             ▼
              ┌───────────────────────────────────────────┐
              │ SDK: WriteTableMetadata(                   │──RAISE P0009──►
              │   newMetadata, table_location)             │  S3写入失败
              │ → newMetadataLocation                      │
              └──────────────┬────────────────────────────┘
                             │
                             ▼
              ┌───────────────────────────────────────────┐
              │ SPI: UPDATE tables_internal                │
              │ SET metadata_location = newLocation,        │
              │     previous_metadata_location = oldLocation│
              │     current_snapshot_id = newSnapshotId    │
              │ WHERE namespace = $1                       │
              │   AND table_name = $2                      │──RAISE P0005──►
              │   AND metadata_location = oldLocation       │  并发冲突
              │   ← 乐观锁: rowcount==0 → 并发冲突          │  (其他会话抢先)
              └──────────────┬────────────────────────────┘
                             │ rowcount == 1 (成功)
                             ▼
              ┌───────────────────────────────────────────┐
              │ SPI: INSERT INTO snapshots                 │
              │ (table_uuid, snapshot_id, schema_id,       │
              │  timestamp_ms, manifest_list,              │
              │  total_records)                            │
              │ ← 缓存新 snapshot 摘要                      │
              └──────────────┬────────────────────────────┘
                             │
                             ▼
              ┌───────────────────────────────────────────┐
              │ RETURN CommitTableResponse JSONB            │
              │ {metadata-location, metadata: {...}}       │
              └───────────────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部 | 所有参数非空；updates 中每个 element.action="add-snapshot" |
| 锁定读取 | SPI | `SELECT ... FROM tables_internal WHERE ... FOR UPDATE` — 行锁保证串行化 |
| 读取元数据 | SDK | `ReadTableMetadata(metadata_location)` — 从 S3 获取当前元数据 |
| 应用变更 | SDK | `ApplyUpdates(base, requirements, updates)` — 校验 requirements + 应用 updates |
| 写 S3 | SDK | `WriteTableMetadata(newMetadata, location)` — 写新 metadata.json |
| 原子更新 | SPI | `UPDATE ... WHERE metadata_location = oldLocation` — 乐观锁，rowcount==0 → P0005 |
| 缓存摘要 | SPI | `INSERT INTO snapshots (...)` — 便于高频查询 |
| 返回 | 内部 | CommitTableResponse JSONB |

**核心一致性保障**：`UPDATE ... WHERE metadata_location = oldLocation` 是系统的原子性锚点。即使 S3 写入成功，若元信息表 UPDATE 失败（被其他事务抢先），整个事务回滚——S3 上的 metadata.json 成为孤儿文件，由后续 vacuum 清理。

---

#### 7.3.5 add_column

```
add_column(p_namespace TEXT, p_table TEXT, p_column_name TEXT, p_column_type TEXT, p_column_doc TEXT DEFAULT NULL) → JSONB
```

```
┌───────────────────────────────────────────────────────────────┐
│ 提取参数: p_namespace, p_table, p_column_name,                 │
│           p_column_type, p_column_doc                          │
└──────────────────────────┬────────────────────────────────────┘
                           │
                           ▼
                    ╔══════════════╗
                    ║ 参数校验:      ║
                    ║ ns/name/col非空║──RAISE P0001──►
                    ║ ValidateType   ║  参数无效/不支持类型
                    ║ (拒绝 fixed(L))║
                    ╚══════╤═══════╝
                           │ 通过
                           ▼
              ┌────────────────────────────────────────────────┐
              │ SPI: SELECT table_uuid, metadata_location,      │
              │      table_location, current_schema_id,         │
              │      current_snapshot_id, last_column_id        │
              │ FROM tables_internal                           │──RAISE P0004──►
              │ WHERE namespace=$1 AND table=$2                │  表不存在
              │ FOR UPDATE                                     │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ SDK: ReadTableMetadata(metadata_location)       │──RAISE P0009──►
              │ → currentMetadata                              │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ 内部: 根据 current_schema_id 获取当前 schema      │
              │ 检查列名是否已存在 → 重复则 P0001                  │
              └──────────────┬─────────────────────────────────┘
                             │ 列名不重复
                             ▼
              ┌────────────────────────────────────────────────┐
              │ SDK: AddColumnToSchema(                         │
              │   currentSchema,                                │
              │   columnName, columnType, columnDoc,            │
              │   &newFieldId)                                  │
              │ → newSchema (+ newFieldId)                      │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ 内部: 自动构造 requirements 和 updates            │
              │                                                 │
              │ requirements = [                                │
              │   {"type":"assert-table-uuid", "uuid":"..."},   │
              │   {"type":"assert-ref-snapshot-id",             │
              │    "ref":"main", "snapshot-id":...},            │
              │   {"type":"assert-current-schema-id", ...},     │
              │   {"type":"assert-last-assigned-field-id", ...} │
              │ ]                                               │
              │                                                 │
              │ updates = [                                     │
              │   {"action":"add-schema", "schema":newSchema},  │
              │   {"action":"set-current-schema",               │
              │    "schema-id":-1}                              │
              │ ]                                               │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ SDK: ApplyUpdates(                              │
              │   currentMetadata,                              │
              │   requirements,                                 │──RAISE P0005──►
              │   updates)                                      │  并发冲突
              │ → newMetadata                                  │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ SDK: WriteTableMetadata(                        │──RAISE P0009──►
              │   newMetadata, table_location)                  │  S3写入失败
              │ → newMetadataLocation                          │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ SPI: UPDATE tables_internal                     │
              │ SET metadata_location = newLocation,            │
              │     previous_metadata_location = oldLocation,   │
              │     current_schema_id = newSchemaId,            │──RAISE P0005──►
              │     last_column_id = newLastColumnId            │  并发冲突
              │ WHERE namespace=$1 AND table=$2                │
              │   AND metadata_location = oldLocation           │
              └──────────────┬─────────────────────────────────┘
                             │ rowcount == 1
                             ▼
              ┌────────────────────────────────────────────────┐
              │ SPI: INSERT INTO table_schemas                  │
              │ (table_uuid, schema_id, field_position,         │
              │  field_id, field_name, field_required,         │
              │  field_type, field_doc)                         │
              │ ← 新 schema 的每个 field 一行                    │
              └──────────────┬─────────────────────────────────┘
                             │
                             ▼
              ┌────────────────────────────────────────────────┐
              │ RETURN CommitTableResponse JSONB                │
              └────────────────────────────────────────────────┘
```

**关键操作说明：**

| 步骤 | 类型 | 操作 |
|------|------|------|
| 参数校验 | 内部+SDK | ns/name/col 非空；`ValidateType(colType)` 拒绝 unsupported |
| 锁定读取 | SPI | `SELECT ... FROM tables_internal WHERE ... FOR UPDATE` |
| 读取元数据 | SDK | `ReadTableMetadata(metadata_location)` |
| 列名冲突 | 内部 | 在 current schema 中检查列名是否重复 |
| 扩展 Schema | SDK | `AddColumnToSchema(...)` — 追加列，生成新 field_id |
| 构造 reqs | 内部 | 自动生成 4 个 requirements（table-uuid, ref-snapshot-id, schema-id, field-id） |
| 应用变更 | SDK | `ApplyUpdates(base, requirements, updates)` |
| 写 S3 | SDK | `WriteTableMetadata(...)` |
| 原子更新 | SPI | `UPDATE ... WHERE metadata_location = oldLocation` — 乐观锁 |
| 缓存 Schema | SPI | `INSERT INTO table_schemas (...)` — 每个 field 一行 |
| 返回 | 内部 | CommitTableResponse JSONB |

**设计要点**：`add_column` 与 `commit_table` 的差异在于：
1. `add_column` 不暴露 requirements 参数——requirements 由实现层自动生成
2. `add_column` 操作 `table_schemas` 表（插入新 schema 的字段）
3. `commit_table` 操作 `snapshots` 表（插入 snapshot 摘要）

---

## 8. 事务与一致性

### 8.1 一致性模型

系统元数据的一致性由 **元信息表** + **OpenGauss 事务机制** 保障。C++ 实现函数中的所有 SPI 操作（元信息表读写）与外围查询处于同一事务（详见第 3 章）。

```
客户端调用 SQL 函数
   │
   ├── OpenGauss 事务开始 (或已在事务中)
   │
   ├── ① C++ 实现函数开始
   │      │
   │      ├── ①-a SPI: SELECT/FOR UPDATE 元信息表（事务性，共享事务快照）
   │      │
   │      ├── ①-b Iceberg SDK: 写入 metadata.json 到 S3（非事务性）
   │      │
   │      └── ①-c SPI: INSERT/UPDATE 元信息表（事务性）
   │            └── 这是系统的一致性锚点
   │
   ├── ② DDL管理模块: 创建/更新 delta 表和外表（事务性）
   │
   └── 事务 COMMIT / ROLLBACK
         │
         ├── COMMIT → 元信息表变更可见，metadata_location 指向新元数据
         └── ROLLBACK → 元信息表回滚，S3 上的 metadata.json 成为孤儿文件
```

### 8.2 孤儿文件处理

S3 写入成功但元信息表更新失败（事务回滚）时，新的 metadata.json 文件成为"孤儿"。由后台 vacuum 进程按 `metadata-log` 反向追踪有效文件后清理。

### 8.3 并发控制策略

| 场景 | 机制 |
|------|------|
| 同一表的并发 commit | `SELECT ... FOR UPDATE` + `UPDATE WHERE metadata_location = oldLocation` |
| namespace 删 + 表创建 | `FOREIGN KEY ... ON DELETE RESTRICT` |
| 同一 namespace 下表名冲突 | `PRIMARY KEY (namespace, table_name)` |
| read-after-write | 元信息表在同一事务内提交，读取方可见 |

### 8.4 与 Spark 侧一致性交互

```
Spark (REST/JDBC Catalog Client)
   │
   ├── REST API → REST 适配组件 → SQL 函数
   │      │                           │
   │      │   HTTP 请求含 requirements │  C++ 实现函数内:
   │      │   (assert-ref-snapshot-id) │   SPI + SDK 在同一事务
   │      │                           │
   │      └── HTTP 200 (LoadTableResponse)
   │
   └── Spark 写入 Data/Delete File 到 S3
         │
         └── commit_table (add-snapshot)
               │
               └── 事务内: SDK S3 写 + SPI 元信息表 UPDATE
```

---

## 9. 错误码映射

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

---

## 10. 待确认项

1. **Delta 表和 FDW 外表的管理**：当前设计假设有独立的 DDL 管理模块负责。需确认 SQL 函数是否直接调用该模块，还是通过回调机制。

2. **Iceberg SDK 的成熟度**：当前接口定义基于 pg_lake 的 SDK 模式假设。需确认 SDK 是否已实现本文所列全部接口，或需分阶段交付。

3. **`p_stage_create` 实现时机**：当前标记为暂不实现。需确认何时需要支持 Stage Create 事务模式。

4. **External 表处理**：`tables_external` 由 Spark JDBC Catalog 直连时使用，SQL 函数设计不涉及此表，所有函数仅操作 `tables_internal`。

5. **S3 凭证注入**：`load_table` 返回的 `config` 字段需包含 S3 访问凭证。需确认凭证由哪个模块注入（SQL 函数自身还是独立的凭证代理）。

6. **SPI 连接管理**：需确认 `SPI_connect()` / `SPI_finish()` 的调用时机。建议在 C++ 实现函数入口调用 `SPI_connect()`，退出前调用 `SPI_finish()`。若出现 ereport(ERROR) longjmp，需确保 ResourceOwner 正确清理（OpenGauss 内核自动处理）。

7. **JSONB 构造接口**：需确认 C++ 层构造 JSONB 的具体方式：
   - 方案 A：通过 SPI 调用 `jsonb_build_object()` SQL 函数
   - 方案 B：直接构造 `Jsonb` 结构（需链接 JSONB 内部 API）
   - 建议采用方案 A（简单可靠），性能敏感场景考虑方案 B
