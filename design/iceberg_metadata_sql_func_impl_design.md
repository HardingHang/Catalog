# Iceberg Metadata SQL Function 实现设计

## 1. 文档说明

### 1.1 目标

本文档为 SQL 自定义函数层（对应 `iceberg_metadata_sql_func_def_design.md` 中定义的 14 个函数）做实现设计，基于 **Iceberg v3** 规范。明确：

1. **职责边界**：SQL 自定义函数、元信息表、Iceberg SDK 三层各自的职责范围及边界规则
2. **设计原则与实现方式**：SPI vs 纯 SQL vs 直接表访问的对比分析及 SPI 选型理由
3. **事务语义**：C++ 元信息模块操作如何保证与外围 SQL 调用处于同一事务
4. 每个 SQL 函数对**元信息模块**的接口诉求（通过 iceberg_meta_* 调用）
5. 每个 SQL 函数对 **Iceberg SDK** 的接口诉求（C 函数直接调用）
6. **Iceberg v3 next-row-id**：`next-row-id` 在 metadata JSON 中由 SDK 管理，`load_table` 返回时包含
7. **所有函数的流程图**：完整描述实现逻辑

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
| **元信息模块** | 封装元信息表 CRUD 操作的 C++ 模块，对 SQL 自定义函数暴露 `iceberg_meta_*` 接口。实现由其他同事负责，不在本文档范围内 |
| **Iceberg SDK** | 实际实现为 Java（Apache Iceberg 官方库），通过 JNI 被 C++ Wrapper 层调用。SQL 函数通过 `catalog.h`/`table.h` 头文件定义的 C++ 类访问 |
| **JNI Wrapper** | C++ 层，实现第 6 章定义的 `IcebergCatalog`/`IcebergTable` 等类。负责 JNI 调用和 Java 异常→`ereport` 转换（JNI 返回后安全触发） |
| **LANGUAGE C** | SQL 函数声明方式，函数体由一个 C/C++ 共享库中的符号实现 |
| **伪代码中的 META.xxx()** | 表示调用元信息模块接口（第 7 章定义），纯元信息表操作 |
| **伪代码中的 catalog->/table->** | 表示调用 Iceberg SDK 的 JNI Wrapper C++ 类方法（第 6 章定义） |

### 1.4 Iceberg v3 与 next-row-id 背景

本系统基于 **Iceberg v3** 规范实现。v3 相比 v2 的关键变化之一是为表引入了 `next-row-id` 字段。

**`next-row-id` 的作用：**

Iceberg v3 中，每个表在 metadata 中维护一个单调递增的 `next-row-id` 计数器。当 Spark/Flink 等引擎写入数据时，通过原子递增 `next-row-id` 预留一段 row ID 范围，为表内的每一行分配全局唯一的标识符。这解决了：
1. **主键唯一性**：行级 ID 在表内全局唯一，支持去重和 Upsert
2. **Row-level delete**：v3 的 position delete 通过 row ID 精确定位要删除的行
3. **变更数据捕获 (CDC)**：row ID 提供稳定的行标识

**SQL 自定义函数层的职责：**

SQL 自定义函数不直接操作 `next-row-id`（递增逻辑由写入引擎负责），但需要：
- **建表时初始化**：`create_table` 通过 SDK 将 `next-row-id` 初始化为 0（SDK 写入 metadata JSON）
- **load_table 返回**：`load_table` 返回的 metadata JSON 中包含 `next-row-id`（SDK 从 S3 读取后通过 `GetNextRowId()` 提供）
- **不落表**：`next-row-id` 不存储在元信息表中（`metadata_table_design.md` §4.3 标记为"不落表，按需识别"），外部组件按需从 metadata JSON 解析

---

## 2. 设计原则与实现方式选择

### 2.1 候选方式总览

SQL 自定义函数的实现有四种可选方式：

```
方式一：纯 SQL（LANGUAGE plpgsql）
┌─────────────────────────────────────────────┐
│ CREATE FUNCTION ... LANGUAGE plpgsql        │
│ AS $$                                       │
│ BEGIN                                       │
│   SELECT ... FROM iceberg_catalog.namespaces │
│   INSERT INTO iceberg_catalog.namespaces ... │
│   -- 调用 C++ 子函数（注册为独立 SQL 函数）     │
│   SELECT iceberg_sdk_write_metadata(...)     │
│ END;                                        │
│ $$;                                         │
└─────────────────────────────────────────────┘

方式二：LANGUAGE C + SPI ★ 推荐
┌─────────────────────────────────────────────┐
│ CREATE FUNCTION ... LANGUAGE C              │
│ AS 'MODULE_PATHNAME', 'impl_function';      │
│                                             │
│ // C++ 实现：                                │
│ Datum impl_function(PG_FUNCTION_ARGS) {     │
│   SPI_connect();                            │
│   // 1. 提取参数 (PG_GETARG_*)               │
│   // 2. SPI_execute_with_args(...) 操作元信息表│
│   // 3. 直接调用 Iceberg SDK C 函数           │
│   // 4. 构造 JSONB 返回值 (PG_RETURN_JSONB_P) │
│   SPI_finish();                             │
│ }                                           │
└─────────────────────────────────────────────┘

方式三：LANGUAGE C + 直接表访问（heap_* API）
┌─────────────────────────────────────────────┐
│ CREATE FUNCTION ... LANGUAGE C              │
│ AS 'MODULE_PATHNAME', 'impl_function';      │
│                                             │
│ // C++ 实现：                                │
│ Datum impl_function(PG_FUNCTION_ARGS) {     │
│   // 1. 提取参数 (PG_GETARG_*)               │
│   // 2. heap_insert/table_beginscan 等       │
│   //    直接操作元信息表 tuple               │
│   // 3. 直接调用 Iceberg SDK C 函数           │
│   // 4. 构造 JSONB 返回值 (PG_RETURN_JSONB_P) │
│ }                                           │
└─────────────────────────────────────────────┘

方式四：混合（SQL + C++ 混编）
┌─────────────────────────────────────────────┐
│ -- 函数体中既有 SQL DML 又有 C++ 内联逻辑     │
│ -- 通常难以维护，不推荐                        │
└─────────────────────────────────────────────┘
```

### 2.2 全面对比分析

#### 2.2.1 主要维度对比

| 维度 | 方式一（纯 SQL） | 方式二（LANGUAGE C + SPI）★ | 方式三（LANGUAGE C + heap_*） | 方式四（混合） |
|------|-----------------|---------------------------|------------------------------|---------------|
| **元信息表操作** | SQL DML 直接写 | SPI 执行 SQL → 自动约束/索引/FK | heap_* API → 手动索引/FK | 两者混用 |
| **SQL 解析开销** | 无额外开销 | 每次 SPI 调用经 SQL 解析/规划/执行 | 无 | 不一致 |
| **索引维护** | 自动 | 自动（SPI 内 SQL 引擎处理） | **手动（高风险）** | 不一致 |
| **FK 级联** | 自动 | 自动 | **手动逐表实现** | 不一致 |
| **JSON 构造** | 繁琐（PL/SQL 嵌套 JSON） | C++ 中方便 | C++ 中方便 | 分散在两处 |
| **分页逻辑** | PL/SQL 实现 offset/token 困难 | C++ 中编码/解码 token 清晰 | C++ 中需手动排序+截取 | 不一致 |
| **错误处理** | RAISE 抛 JSON 错误 | C++ 中 `ereport()` 统一格式 | C++ 中 `ereport()` 统一格式 | 风格不统一 |
| **SDK 调用** | 需注册为独立 SQL 函数 | 直接 C 函数调用 | 直接 C 函数调用 | 两者混用 |
| **调试** | SQL 层可见，易调试 | SQL 日志可见 + C 调试器 | 仅 C 调试器，数据不可见 | 最困难 |
| **代码量** | 中等 | **较少**（SPI = 1~3 行/操作） | 多（需封装层 + 索引代码） | 最多 |
| **逻辑内聚** | 差（跨 SQL/C 边界） | **好**（一个 C++ 函数） | **好**（一个 C++ 函数） | 差 |
| **可维护性** | 中等 | **好** | 差（索引维护脆弱） | 最差 |

#### 2.2.2 SPI 的问题与代价

SPI 是方式二的核心依赖。以下客观列出 SPI 在实际工程中的问题和代价，以及在本项目中的影响评估：

| # | SPI 问题 | 详细说明 | 本项目影响 |
|---|---------|---------|-----------|
| 1 | **SQL 解析/规划开销** | 每次 `SPI_execute()` 调用完整走 SQL 解析→分析→规划→执行流水线。对于简单 CRUD，解析开销可能超过实际 IO | **低**。元信息表操作频率低（建表/删表/提交是管理操作，非热路径），SQL 解析开销可忽略 |
| 2 | **内存上下文管理** | SPI 使用独立内存上下文（`SPI_processed`、`SPI_tuptable`）。`SPI_finish()` 后所有 SPI 分配的内存被释放，开发者需注意不持有悬空指针 | **中**。需在每个函数入口/出口正确调用 `SPI_connect()`/`SPI_finish()`，并在 `SPI_finish()` 前复制需要的数据 |
| 3 | **错误处理差异** | `SPI_execute()` 返回 `SPI_ERROR` 等状态码而非抛异常。若 SQL 失败（如约束冲突），需检查 `SPI_result` 后手动 `ereport(ERROR)` | **低**。封装 SPI 调用的 helper 函数可统一处理，上层代码无需逐处检查 |
| 4 | **非原子性** | 多个 `SPI_execute()` 调用之间不是原子的。若第 2 个 SPI 调用失败，第 1 个的变更不会自动回滚（除非在子事务中） | **低**。所有 SPI 调用在同一事务中，`ereport(ERROR)` 的 `longjmp` 触发 `AbortCurrentTransaction()` 回滚全部变更 |
| 5 | **CCI 不可见** | SPI 内部的 DML 自动推进 `CommandCounterIncrement`，同一事务后续 SPI 可见。但 SPI 之外的 C 代码修改（如 SDK 内部操作）对 SPI 不可见 | **低**。SDK 操作（S3 读写）不修改元信息表，不存在此场景 |
| 6 | **FROM 子句限制** | SPI 不支持某些非 SQL 的查询（如直接扫描系统表），但本项目只操作普通用户表 | **无影响** |
| 7 | **调试复杂性** | 错误发生在 SQL 层与 C 层的边界，堆栈可能不连续。需同时检查 C 日志和 SQL 日志 | **低-中**。建议封装统一的 SPI helper 层，在 helper 中记录详细的错误上下文 |

#### 2.2.3 直接表访问（方式三）的额外代价

作为对比，方式三虽然避免了 SPI 开销，但引入了更大的代价：

| # | 直接表访问问题 | 详细说明 |
|---|-------------|---------|
| 1 | **索引手动维护** | 每次 INSERT/UPDATE/DELETE 后必须遍历 `RelationGetIndexList`，为每个索引调用 `FormIndexDatum` + `index_insert`（带 `UNIQUE_CHECK_YES`）。若遗漏或顺序错误，PK/Unique 约束形同虚设 |
| 2 | **FK 级联手动实现** | `ON DELETE CASCADE` / `ON DELETE RESTRICT` 需逐表编码。`drop_table` 需手动扫描 `table_schemas`、`snapshots`、`partition_specs` 并逐行删除 |
| 3 | **CCI 手动调用** | 每次写入后需手动 `CommandCounterIncrement()`，否则后续 scan 看不到刚写入的 tuple |
| 4 | **分页排序手动实现** | `list_namespaces`/`list_tables` 的 `ORDER BY + LIMIT + OFFSET` 需全扫描→Tuplesort→截取窗口 |
| 5 | **API 稳定性风险** | heap_*/table_* API 是内核内部接口，大版本升级可能变化 |
| 6 | **代码量膨胀** | 每个 CRUD 操作需要 20~50 行 vs SPI 的 1~3 行 |

### 2.3 决策：采用方式二（LANGUAGE C + SPI）

**SPI 的问题真实存在，但在本项目场景下影响可控。**直接表访问（方式三）引入的索引维护风险、FK 级联复杂度、代码膨胀代价远超 SPI 的解析开销。

**核心理由：**

1. **SPI 的 SQL 解析开销对元数据管理可忽略**：元信息表操作是管理操作（建表/删表/提交），不是高频热路径。即使每次多花 0.1ms 在 SQL 解析上，对用户体验无影响。

2. **SPI 自动处理约束/索引/FK 是决定性优势**：元信息表有 5 张表、多个 PK/FK/Unique 约束。SPI 下这些都自动生效——开发效率和正确性远高于手动维护。

3. **JSONB 返回值在 C++ 中构造更自然**：排除方式一（纯 SQL）。

4. **Iceberg SDK 是 C 接口**：LANGUAGE C 可直接调用，无需为每个 SDK 函数包装 SQL 函数。排除方式一。

5. **逻辑内聚**：每个函数的完整逻辑（校验→表操作→SDK→返回）集中在一个 C++ 函数中。

6. **代码简洁**：SPI 方式每个表操作只需 1~3 行 SQL，直接表访问需要 20~50 行。

7. **调试友好**：SPI 方式下，`log_statement = 'all'` 即可在 SQL 日志中看到所有元信息表操作，方便问题排查。

### 2.4 实现模式

每个 SQL 函数遵循统一的实现模式：

```sql
-- SQL 函数声明
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
    // 1. 提取参数 (PG_GETARG_*)
    // 2. 参数校验（校验失败 → ereport(ERROR, ...)）

    // 3. META 检查（纯元信息表操作，不经过 SDK/JNI）
    if (iceberg_meta_NamespaceExists(p_namespace))
        ereport(ERROR, ...);

    // 4. SDK 创建 namespace（JNI → Java Iceberg）
    //    C++ Wrapper 在 JNI 返回后检查异常 → ereport
    //    ★ SQL 函数不拼路径，全部由 SDK 处理
    IcebergCatalog *catalog = GetIcebergCatalog();
    char *nsLocation = catalog->CreateNamespace(name, props);

    // 5. 元信息表写入
    iceberg_meta_InsertNamespace(name, props_with_location);
    pfree(nsLocation);

    // 6. 构造 JSONB 返回值 (PG_RETURN_JSONB_P)
}
```

> **模块边界**：
> - 元信息模块接口（`iceberg_meta_*`）的实现不在本文档范围内——由元信息模块设计文档负责
> - SDK 实际为 Java，通过 JNI 调用。C++ Wrapper 层负责 JNI 异常→`ereport` 转换（仅在 JNI 返回后触发 `ereport`）
> - SQL 函数不区分操作是 META 还是 SDK——只按接口契约调用，不关心底层实现

---

## 3. 事务语义

> 事务语义由**元信息模块**内部保证。SQL 自定义函数调用的所有 `iceberg_meta_*` 接口均在当前 SQL 函数的事务上下文中执行——元信息模块的底层实现（SPI 或直接表访问）不改变此保证。
>
> 一致性细节见第 9 章。S3 写入不在事务中，一致性由元信息表的原子操作保证。

---

## 4. 职责边界

本章明确 SQL 自定义函数层、元信息表层、Iceberg SDK 层三者的职责边界，防止 Iceberg 语义泄漏到 SQL 函数层。

### 4.1 分层职责总览

```
┌───────────────────────────────────────────────────────────────┐
│                    SQL 自定义函数层                             │
│                                                                 │
│  负责：                                                         │
│  • 参数校验（NULL/空串检测、基本格式检查）                         │
│  • 流程编排（元信息模块 ↔ SDK 调用的顺序与条件）              │
│  • JSONB 返回值构造（将 SDK/META 结果组织为 API 响应格式）         │
│  • 错误处理与错误消息格式化（Iceberg REST API JSON 格式 ereport）  │
│  • 分页逻辑（page_token 编解码、offset/limit 计算）              │
│  • 业务规则（存在性检查、冲突检测、并发控制）                      │
│  • 事务内的 META + SDK 操作协调（详见第 3 章）                               │
│                                                                 │
│  ✗ 禁止：                                                       │
│  • 拼接 S3 路径字符串                                           │
│  • 知道 metadata 文件命名规则                                    │
│  • 硬编码默认位置前缀（DefaultPrefix）                            │
│  • 直接操作 metadata JSON 内部字段                               │
│  • 校验 Iceberg 复杂类型（fixed、decimal 参数等）                  │
└────────────────────────────┬────────────────────────────────────┘
                             │
           元信息模块  │     C 函数调用
               ┌─────────────┴─────────────┐
               ▼                           ▼
┌────────────────────────────┐ ┌──────────────────────────────────┐
│       元信息表层            │ │          Iceberg SDK 层            │
│                             │ │                                   │
│  负责：                      │ │  负责：                            │
│  • 持久化 Catalog 对象映射   │ │  • Iceberg 表元数据语义             │
│  • metadata 指针存储         │ │    (schema/snapshot/partition/     │
│    (metadata_location 等)   │ │     sort order/properties)         │
│  • 约束强制执行              │ │  • metadata JSON 序列化/反序列化    │
│    (PK/FK/Unique/Check)    │ │  • S3 路径/位置约定                 │
│  • 高频摘要缓存              │ │    (namespace 路径、table 路径、    │
│    (schema fields,          │ │     metadata 文件路径、             │
│     snapshot summaries)    │ │     manifest 路径等)                │
│                             │ │  • Iceberg 类型系统校验             │
│  ✗ 禁止：                    │ │    (含 fixed(L)、decimal 参数等)   │
│  • 调用 Iceberg SDK         │ │  • Schema/Snapshot/Partition 操作  │
│  • 访问 S3                  │ │  • UUID/SnapshotId 生成             │
│  • 解析 metadata JSON       │ │  • S3 对象读写                     │
│                             │ │                                   │
│                             │ │  ✗ 禁止：                          │
│                             │ │  • 访问元信息表（不关心 DB Schema）   │
│                             │ │  • 知道 SQL 函数参数格式            │
│                             │ │  • 知道 API 返回值 JSON 格式        │
│                             │ │  • 参与数据库事务管理               │
└────────────────────────────┘ └──────────────────────────────────┘
```

### 4.2 关键边界：路径/位置的职责归属

路径拼写是最容易越界的地方。核心原则：**SQL 函数传递语义标识（namespace 名、table 名），SDK 返回具体路径。SQL 函数只存储路径字符串，不构造路径字符串。**

| 场景 | ✗ 错误（越界） | ✓ 正确（边界清晰） |
|------|----------------|-------------------|
| **Namespace 路径确定** | SQL 函数计算 `{DefaultPrefix}/{namespace}` | SQL 调用 `SDK.ResolveNamespaceLocation(ns, properties)`，SDK 内部解析并返回路径 |
| **Table 路径确定** | SQL 函数计算 `{nsLocation}/{tableName}` | SQL 将 `location_hint`（用户指定或 NULL）传给 `SDK.CreateTableMetadata(...)`，SDK 确定最终 `table_location`，SQL 从返回的 metadata 中读取 |
| **Metadata 文件路径** | SQL 函数知道 `v{N}.metadata.json` 命名规则 | SDK 独占。SQL 只存储 SDK 返回的 `metadata_location` 字符串 |
| **S3 URI 格式校验** | SQL 函数校验 S3 URI | SDK 内部校验，非法格式返回错误 |
| **默认前缀 fallback** | SQL 函数硬编码 `DefaultPrefix` | SDK 内部持有部署级配置 |

### 4.3 数据流向

以 `create_table` 为例说明路径信息的正确流向：

```
SQL 函数                                     Iceberg SDK (JNI → Java)
  │                                                 │
  │ ① META.GetNamespace(ns) → properties            │
  │    (namespace 存在性由 META 验证)                 │
  │                                                 │
  ├── catalog->CreateTable(ns, tbl, schema, ───────►│
  │         location_hint, partition, ...)           │ ② SDK 内部（Java）：
  │                                                 │    · 解析 namespace S3 路径
  │       返回 IcebergTable*                         │    · 生成 UUID
  │       table->GetTableUUID()                      │    · 解析 schema
  │       table->GetTableLocation()  ← SDK 解析     │    · 确定 table_location
  │       table->GetMetadataLocation()← SDK 生成    │      (hint?? → nsLocation/tblName)
  │       table->GetNextRowId()      ← 初始化为 0   │    · 构造 metadata JSON
  │◄────────────────────────────────────────────────┤    · 写入 S3
  │                                                 │
  │ ③ META.InsertTable(...)                         │
  │    存储 table->GetTableLocation() 等             │
  │    ← 所有路径值来自 SDK 对象 getter，SQL 不拼接    │
```

**关键点**：
1. SQL 函数全程不拼接任何 S3 路径字符串。所有路径来自 `IcebergTable` getter 方法
2. Namespace 的元数据（名称、属性）由 META 管理；SDK 只处理 S3 路径解析
3. SDK 实际实现为 Java（JNI 调用），C++ Wrapper 层在 JNI 返回后才调用 `ereport`，保证安全

### 4.4 边界规则清单

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

### 4.5 判断标准

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

## 5. 系统上下文

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

### 4.2 模块交互总览

每个 SQL 自定义函数的实现涉及以下模块的交互：

| 模块 | 职责 | 交互方式 |
|------|------|---------|
| **元信息表** | 持久化 Catalog 对象映射、metadata 指针、高频摘要缓存 | C++ 调用 iceberg_meta_* 接口 |
| **Iceberg SDK** | metadata JSON 构造/解析、schema/snapshot/partition 操作 | C++ 直接调用 C 函数 |
| **对象存储** | 读写 `metadata.json`、manifest 文件等 | Iceberg SDK 内部封装 S3 API |
| **DDL 管理模块** | 创建/删除 delta 表和 FDW 外表 | C 函数调用（非本设计展开范围） |

---

## 6. Iceberg SDK 接口诉求

### 6.0 架构说明：JNI 调用链与 ereport 安全性

Iceberg SDK 的实际实现是 **Java**（Apache Iceberg 官方库），通过 **JNI** 被 C++ 层调用。SQL 自定义函数不直接接触 JNI——中间有一层 C++ 头文件定义 API，其实现负责 JNI 调用。

```
SQL 自定义函数 (C++)
       │
       ├── 调用 iceberg_sdk/catalog.h 等头文件定义的 C++ 方法
       │
       ▼
C++ Wrapper 层 (JNI 实现)
       │
       ├── JNI 调用 → Java Iceberg SDK
       │
       ▼
Java Iceberg SDK (实际实现)
       │
       └── S3 读写
```

**ereport 安全性分析：**

`ereport(ERROR, ...)` 在 PostgreSQL/OpenGauss 内部通过 `longjmp` 跳转到 executor 的错误处理器。关键约束：`longjmp` **不能跨越 JNI 栈帧**——若在 JNI 调用链内部触发 `longjmp`，JVM 内部状态将不一致（局部引用未释放、异常未清理等）。

因此 C++ Wrapper 层遵循以下规则：

```
C++ Wrapper 层安全模式：
┌──────────────────────────────────────────────────────────┐
│                                                          │
│  JNIEnv *env = ...;                                      │
│  jobject result = env->CallStaticObjectMethod(...);      │
│                                                          │
│  // ★ JNI 调用已完全返回，栈上无 JNI 帧                    │
│                                                          │
│  if (env->ExceptionCheck()) {                            │
│      jstring jmsg = getJavaExceptionMessage(env);        │
│      env->ExceptionClear();  // 清理 Java 异常            │
│      const char *msg = env->GetStringUTFChars(jmsg);     │
│      ereport(ERROR, errcode(ERRCODE_P0009),              │
│              errmsg("{\"type\":\"ServiceUnavailable\",\"message\":\"%s\"}", msg)); │
│      // ↑ 安全：JNI 已返回，longjmp 不跨越 JNI 帧          │
│  }                                                       │
│                                                          │
│  return result;                                          │
└──────────────────────────────────────────────────────────┘
```

**结论**：`ereport` 可以工作，但只能在 JNI 调用**完全返回后**触发。C++ Wrapper 层的实现负责这个安全约束。

#### 6.0.1 错误处理约定：通用错误消息 + SQL 层格式化

SDK 和元信息模块（第 7 章）不仅被 SQL 自定义函数调用，也可能被其他模块（REST 适配器、后台任务等）调用。因此接口**不应**在 `ereport` 中硬编码 Iceberg REST API 的 JSON 错误格式。

**约定**：所有可失败的 SDK/META 方法通过 `char **error_msg` 输出参数返回**纯文本错误描述**。由调用方（SQL 函数层）负责将其包装为 Iceberg REST API JSON 格式并调用 `ereport`。

```
┌──────────────────────────────────────────────────────────────┐
│ SQL 自定义函数层                                              │
│                                                               │
│   char *error_msg = NULL;                                     │
│   table = catalog->LoadTable(ns, tbl, mdl, &error_msg);      │
│   if (error_msg != NULL)                                      │
│       ereport(P0009, iceberg_error_json("ServiceUnavailable", │
│                                          error_msg));         │
│                         ↑                                     │
│              纯文本 → JSON 格式化                              │
└──────────────────────────┬───────────────────────────────────┘
                           │ error_msg = "Failed to read..."
                           │
┌──────────────────────────┴───────────────────────────────────┐
│ SDK / META 层（通用，不感知 JSON 格式）                        │
│   *error_msg = "Failed to read metadata from S3: timeout"    │
└──────────────────────────────────────────────────────────────┘
```

### 6.1 职责范围

Iceberg SDK 负责**需要 Iceberg 语义或 S3 访问**的操作。

| 操作类型 | 由谁负责 | 原因 |
|----------|---------|------|
| 存在性检查 / 分页列表 / namespace CRUD | **META** | 只查元信息表，不涉及 S3 |
| `rename_table` | **META** + **SDK** | META 更新元信息表记录；若需同步更新 S3 路径则需 SDK（详见下方说明） |
| S3 路径解析 + marker 创建 | **SDK** | 路径约定和 S3 操作是 Iceberg 语义 |
| metadata JSON 构造/读写 | **SDK** | metadata 格式是 Iceberg 语义 |
| Schema 操作 / Commit | **SDK** | schema 演进和 snapshot 管理是 Iceberg 语义 |
| 类型校验 | **SDK** | Iceberg 类型系统 |

> **关于 `rename_table`**：标准 Iceberg 规范中表名是 catalog 概念（存储在 catalog 而非 metadata JSON 中），因此重命名只需更新元信息表。但若部署要求 S3 路径随表名同步变更（如 `{ns}/{table_name}`），则需 SDK 参与 S3 文件迁移。当前设计中 SDK 的 `RenameTable` 接口预留此能力，是否实际调用由实现阶段决定。

### 6.2 头文件结构

```
iceberg_sdk/
├── catalog.h    ← IcebergCatalog：入口 + namespace S3 操作 + table 创建/加载
├── table.h      ← IcebergTable：表元数据 + schema 操作 + commit
├── schema.h     ← IcebergSchema：schema 结构（只读）
├── snapshot.h   ← IcebergSnapshot：snapshot 结构（只读）
└── types.h      ← 通用类型
```

> **注意**：没有 `namespace.h`。Namespace 的元数据（名称、属性）存储在元信息表中，由元信息模块管理。SDK 只处理 namespace 的 **S3 路径操作**（解析路径、创建/清理 marker），这些操作通过 `IcebergCatalog` 的 `CreateNamespace`/`DropNamespace` 方法完成，以 `char*`（location 字符串）返回结果，不封装为对象。

### 6.3 catalog.h — IcebergCatalog

```cpp
class IcebergCatalog {
public:
    // ── 工厂方法 ──
    // *error_msg: 失败时返回纯文本错误描述（调用方 pfree 释放）。NULL 表示成功
    static IcebergCatalog* Open(const char *warehouse_location, char **error_msg);
    void Close();

    // ── Namespace S3 操作 ──
    // 解析 S3 路径 + 创建 marker。返回解析后的 S3 location（pfree 释放）
    char* CreateNamespace(const char *namespace_name,
                           const char *properties_json,
                           char **error_msg);

    // 清理 S3 marker（best-effort，不通过 error_msg 报错）
    void  DropNamespace(const char *namespace_name);

    // ── Table 操作 ──
    // 从 S3 读取 metadata JSON 并解析为 IcebergTable 对象
    IcebergTable* LoadTable(const char *namespace_name,
                             const char *table_name,
                             const char *metadata_location,
                             char **error_msg);

    // 创建表：UUID → schema 解析 → 确定 table_location → 构造 metadata → 写 S3
    IcebergTable* CreateTable(const char *namespace_name,
                               const char *table_name,
                               const char *schema_json,
                               const char *location_hint,        // NULL → SDK 推导
                               const char *partition_spec_json,  // NULL → 无分区
                               const char *write_order_json,     // NULL → 无排序
                               const char *properties_json,      // NULL → {}
                               char **error_msg);

    // S3 清理（best-effort）
    void  DropTable(const char *namespace_name, const char *table_name);

    // 重命名表涉及的 S3 路径迁移（预留，当前实现可为空操作）
    void  RenameTable(const char *src_ns, const char *src_table,
                       const char *dst_ns, const char *dst_table,
                       char **error_msg);

    // ── 类型校验 ──
    // 返回 false 时 *error_msg 包含详细错误信息
    bool  ValidateType(const char *type_string, char **error_msg);
};
```

### 6.4 table.h — IcebergTable

```cpp
class IcebergTable {
public:
    // ── 基本信息（只读 getter）──
    const char* GetTableUUID();
    const char* GetTableLocation();       // SDK 确定的 table S3 路径
    const char* GetMetadataLocation();    // 当前 metadata JSON 的 S3 路径
    int64_t     GetNextRowId();           // Iceberg v3

    // ── Metadata ──
    const char* GetMetadataJson();        // 完整 metadata JSON（含 schemas/snapshots/partition-specs）

    // ── 辅助字段 ──
    int         GetCurrentSchemaId();
    int         GetLastColumnId();
    int         GetDefaultPartitionSpecId();

    // ── Schema 操作 ──
    IcebergSchema* GetCurrentSchema();
    // 追加列，返回新 schema。out_new_field_id 返回新分配的 field ID
    IcebergSchema* AddColumn(const char *column_name,
                              const char *column_type,
                              const char *column_doc,
                              int     *out_new_field_id);

    // ── Commit ──
    // 应用 requirements + updates → 构造新 metadata → 写入 S3
    // 返回新 metadata_location。失败时 *error_msg 含纯文本描述
    const char* CommitTable(const char *requirements_json,
                             const char *updates_json,
                             char **error_msg);

    // ── Snapshot 访问 ──
    IcebergSnapshot* GetCurrentSnapshot();
};
```

### 6.5 辅助类型（只读数据对象）

```cpp
// schema.h
class IcebergSchema {
public:
    int         GetSchemaId();
    const char* GetSchemaJson();     // 序列化为 JSON
    int         GetFieldCount();
    const char* GetFieldName(int index);
    const char* GetFieldType(int index);
    int         GetFieldId(int index);
};

// snapshot.h
class IcebergSnapshot {
public:
    int64_t     GetSnapshotId();
    int64_t     GetTimestampMs();
    const char* GetManifestList();
    int         GetSchemaId();
    int64_t     GetParentSnapshotId();
    const char* GetSummaryJson();
};
```

### 6.6 接口与 SQL 函数的对应关系

| SDK 类/方法 | 使用该接口的 SQL 函数 | 说明 |
|-------------|---------------------|------|
| `Catalog::Open` | 所有需要 SDK 的函数（会话级单例） | 初始化 S3 连接 |
| `Catalog::CreateNamespace` | `create_namespace` | S3 路径解析 + marker 创建 |
| `Catalog::DropNamespace` | `drop_namespace` | S3 marker 清理（best-effort） |
| `Catalog::LoadTable` | `load_table`, `commit_table`, `add_column` | 从 S3 读取 metadata JSON |
| `Catalog::CreateTable` | `create_table` | UUID + schema 解析 + metadata 构造 + S3 写 |
| `Catalog::DropTable` | `drop_table` | S3 清理（best-effort） |
| `Catalog::RenameTable` | `rename_table` | S3 路径迁移（预留） |
| `Catalog::ValidateType` | `create_table`, `add_column` | Iceberg 类型校验 |
| `Table::GetTableUUID` | `create_table` | |
| `Table::GetTableLocation` | `create_table` | 存元信息表 |
| `Table::GetMetadataLocation` | `create_table`, `load_table`, `commit_table`, `add_column` | |
| `Table::GetNextRowId` | `create_table`, `load_table` | Iceberg v3 |
| `Table::GetMetadataJson` | `load_table`, `create_table` | LoadTableResult 的 metadata 字段 |
| `Table::GetCurrentSchemaId` | `create_table`, `commit_table`, `add_column` | |
| `Table::GetLastColumnId` | `create_table`, `add_column` | |
| `Table::GetDefaultPartitionSpecId` | `create_table` | |
| `Table::GetCurrentSchema` | `add_column` | 检查列名冲突 |
| `Table::AddColumn` | `add_column` | 扩展 schema |
| `Table::CommitTable` | `commit_table`, `add_column` | requirements + updates + S3 写 |
| `Table::GetCurrentSnapshot` | `commit_table` | 缓存 snapshot 摘要 |

> **不在 SDK 中的操作**（由元信息模块负责）：`NamespaceExists`、`TableExists`、`NamespaceHasTables`、`LoadNamespace`、`ListNamespaces`、`ListTables`、`UpdateNamespaceProperties`、`GetNamespace`。这些操作只涉及元信息表 CRUD，不需要 Iceberg SDK。
>
> **跨 META + SDK 的操作**：`RenameTable` — META 负责更新元信息表记录，SDK 预留 S3 路径迁移能力（当前可为空操作）。

### 6.7 关键设计点

| 设计点 | 说明 |
|--------|------|
| **JNI 安全** | C++ Wrapper 层在 JNI 调用**返回后**才检查错误并填充 `*error_msg`，确保 `ereport` 调用的 `longjmp` 不跨越 JNI 栈帧 |
| **通用错误消息** | 所有可失败的 SDK 方法通过 `char **error_msg` 返回**纯文本**错误描述。SQL 函数层负责将其包装为 Iceberg REST API JSON 格式后调用 `ereport`。这使 SDK 可被其他模块复用 |
| **路径封装** | `CreateNamespace` 和 `CreateTable` 内部处理所有路径解析。SQL 函数只通过 getter 读取结果 |
| **SDK 最小化** | SDK 仅包含需要 Iceberg 语义或 S3 访问的操作。纯元信息表查询由元信息模块负责 |
| **命名空间简化** | Namespace 无需封装为对象——元数据在 META 中，SDK 只负责 S3 路径。`CreateNamespace` 直接返回 `char*` location |
| **生命周期** | `IcebergCatalog` 为会话级单例。`IcebergTable` 对象在使用后 `delete` 释放 |

---

## 7. 元信息模块接口诉求

SQL 自定义函数通过**元信息模块**（由其他同事在独立的元信息模块设计文档中实现）提供的 C++ 接口操作元信息表。本章定义 SQL 自定义函数视角下需要的接口签名——仅定义函数名、参数和返回值，不涉及具体实现（SPI/直接表访问等）。

> **实现归属**：以下接口的实现属于元信息模块，不在本设计范围内。本设计仅约定接口契约。

> **错误处理约定**（与 SDK 一致，见 6.0.1）：可失败的 META 方法通过 `char **error_msg` 返回**纯文本**错误描述。SQL 函数层通过 `META_CHECK` 宏将其包装为 Iceberg REST API JSON 格式并 `ereport`。
> ```cpp
> void META_CHECK(bool ok, char *error_msg, const char *iceberg_type, const char *sqlstate) {
>     if (error_msg != NULL)
>         ereport(ERROR, errcode(sqlstate),
>                 errmsg("{\"type\":\"%s\",\"message\":\"%s\",\"stack\":[]}",
>                        iceberg_type, error_msg));
> }
> ```
> 返回 `bool` 的函数（如 `NamespaceExists`）不通过 `error_msg` 报错——仅返回 true/false，由调用方决定是否 `ereport`。写入类函数在失败时设置 `*error_msg` 并返回 false/NULL。

### 7.1 数据结构

```cpp
// Namespace 信息
typedef struct MetaNamespaceInfo {
    char *namespace_name;    // namespace 名称
    char *properties;        // properties JSONB 字符串
} MetaNamespaceInfo;

// Table 元信息（对应 tables_internal 一行，字段与 metadata_table_design.md 对齐）
typedef struct MetaTableInfo {
    Oid      relid;                       // REGCLASS NOT NULL，本地 relation OID（DDL 创建后填入）
    char    *table_uuid;                  // UUID NOT NULL
    char    *metadata_location;           // TEXT NOT NULL
    char    *previous_metadata_location;  // TEXT，可 NULL
    char    *table_location;              // TEXT NOT NULL
    int      last_column_id;              // INT NOT NULL
    int      current_schema_id;           // INT，可 NULL
    int64_t  current_snapshot_id;         // BIGINT，可 NULL
    int      default_spec_id;             // INT，可 NULL
    // 注意：next-row-id 不落表（metadata_table_design.md §4.3 标记"不落表，按需识别"），
    // 仅在 load_table 返回的 metadata JSON 中通过 SDK GetNextRowId() 提供
} MetaTableInfo;
```

### 7.2 命名空间操作接口

```cpp
// 检查 namespace 是否存在（不抛错）
bool iceberg_meta_NamespaceExists(const char *namespace_name);

// 读取 namespace 信息。未找到返回 NULL
MetaNamespaceInfo* iceberg_meta_GetNamespace(const char *namespace_name);

// 创建 namespace。已存在则 ereport(P0005)
void iceberg_meta_InsertNamespace(const char *namespace_name,
                                   const char *properties_json);

// 删除 namespace。不存在则 ereport(P0004)
void iceberg_meta_DeleteNamespace(const char *namespace_name);

// 更新 namespace properties（removals 和 updates 均为 JSONB 数组字符串）
// 返回结果 JSON：{"updated": [...], "removed": [...], "missing": [...]}
char* iceberg_meta_UpdateNamespaceProperties(const char *namespace_name,
                                               const char *removals_json,
                                               const char *updates_json);

// 分页列出 namespace。返回 ListNamespacesResponse JSON
char* iceberg_meta_ListNamespaces(const char *parent, int page_size,
                                    const char *page_token);

// 检查 namespace 下是否有表（用于 drop_namespace 前置检查）
bool iceberg_meta_NamespaceHasTables(const char *namespace_name);
```

### 7.3 表操作接口

```cpp
// 检查表是否存在（不抛错）
bool iceberg_meta_TableExists(const char *namespace_name,
                               const char *table_name);

// 读取表元信息。未找到则 ereport(P0004)
MetaTableInfo* iceberg_meta_GetTable(const char *namespace_name,
                                      const char *table_name);

// 读取表元信息并加行锁（用于 commit_table / add_column / drop_table）
MetaTableInfo* iceberg_meta_GetTableForUpdate(const char *namespace_name,
                                                const char *table_name);

// 创建表记录。冲突则 ereport(P0005)
void iceberg_meta_InsertTable(const char *namespace_name,
                               const char *table_name,
                               const MetaTableInfo *info);

// 更新表元信息（乐观锁：old_metadata_location 不匹配则 ereport(P0005)）
void iceberg_meta_UpdateTable(const char *namespace_name,
                               const char *table_name,
                               const char *old_metadata_location,
                               const char *new_metadata_location,
                               int64_t new_snapshot_id,
                               int new_schema_id,
                               int new_last_column_id);

// 删除表记录。不存在则 ereport(P0004)
void iceberg_meta_DeleteTable(const char *namespace_name,
                               const char *table_name);

// 分页列出表。返回 ListTablesResponse JSON
char* iceberg_meta_ListTables(const char *namespace_name, int page_size,
                                const char *page_token);

// 重命名表。源不存在→P0004，目标已存在→P0005，目标NS不存在→P0004
void iceberg_meta_RenameTable(const char *src_namespace,
                               const char *src_table,
                               const char *dst_namespace,
                               const char *dst_table);
```

### 7.4 缓存操作接口

```cpp
// 插入 schema 字段缓存（展开 schema.fields 为多行）
void iceberg_meta_InsertSchemaFields(const char *table_uuid,
                                      int schema_id,
                                      const char *fields_json);

// 插入 snapshot 摘要缓存
void iceberg_meta_InsertSnapshot(const char *table_uuid,
                                  int64_t snapshot_id,
                                  int schema_id,
                                  int64_t timestamp_ms,
                                  const char *manifest_list,
                                  int64_t total_records);

// 插入 partition spec 缓存
void iceberg_meta_InsertPartitionSpec(const char *table_uuid,
                                       int spec_id,
                                       const char *fields_json);
```

### 7.5 接口约定

| 约定项 | 说明 |
|--------|------|
| **内存管理** | 返回的指针（`MetaNamespaceInfo*`、`MetaTableInfo*`、`char*`）由调用方通过 `pfree()` 释放 |
| **错误处理** | 返回 `bool` 的函数仅返回 true/false，不抛错（用于存在性检查）。返回指针的查询函数在"未找到"时返回 NULL。写入类函数在失败时 `ereport(ERROR, ...)` 并携带正确的 SQLSTATE（P0001~P0009）和 JSON 格式错误消息 |
| **事务** | 所有接口操作与 SQL 自定义函数处于同一事务（元信息模块内部保证） |
| **乐观锁** | `UpdateTable` 的 `old_metadata_location` 参数实现乐观锁。不匹配时 `ereport(P0005, CommitFailedException)` |

---

## 8. 函数实现伪代码

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

### 8.1 is_namespace_existed

```
is_namespace_existed(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. return META.NamespaceExists(p_namespace)
        ? {"exists": true}
        : {"exists": false}
```

### 8.2 is_table_existed

```
is_table_existed(p_namespace TEXT, p_table TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. p_table 为 NULL 或空串 → ereport(P0001, "table must not be empty")
3. return META.TableExists(p_namespace, p_table)
        ? {"exists": true}
        : {"exists": false}
```

### 8.3 load_namespace

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

### 8.4 list_namespaces

```
list_namespaces(p_parent TEXT DEFAULT NULL, p_page_size INT DEFAULT 1000,
                p_page_token TEXT DEFAULT NULL) → JSONB

1. if p_page_size < 1 → ereport(P0001, "pageSize must be >= 1")
2. if p_parent 非空:
       if !META.NamespaceExists(p_parent) → ereport(P0004)
3. result = META.ListNamespaces(p_parent, p_page_size, p_page_token)
4. return json_parse(result)
```

### 8.5 list_tables

```
list_tables(p_namespace TEXT, p_page_size INT DEFAULT 1000,
            p_page_token TEXT DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. if p_page_size < 1 → ereport(P0001)
3. if !META.NamespaceExists(p_namespace) → ereport(P0004)
4. result = META.ListTables(p_namespace, p_page_size, p_page_token)
5. return json_parse(result)
```

### 8.6 create_namespace

```
create_namespace(p_namespace TEXT, p_properties JSONB DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. if p_properties 非 NULL 且非合法 JSONB object → ereport(P0001)

3. // ★ 先写元信息表（利用 PK 约束仲裁并发冲突，详见 8.15）
   //   若用户指定了 location 则直接使用；否则先用临时值占位，SDK 返回后再更新
   props_str = p_properties ? jsonb_to_cstring(p_properties) : "{}"
   if META.NamespaceExists(p_namespace) → ereport(P0005)
   META.InsertNamespace(p_namespace, props_str)

4. // SDK 解析 S3 路径 + 创建 marker
   // 若失败 → ereport → 事务回滚 → META INSERT 自动撤销
   error_msg = NULL
   nsLocation = catalog->CreateNamespace(p_namespace, props_str, &error_msg)
   SDK_CHECK(nsLocation, error_msg, "ServiceUnavailable")

5. // 若用户未指定 location，将 SDK 返回的路径更新到 properties
   if "location" not in p_properties:
       META.UpdateNamespaceProperties(p_namespace, "[]",
                                       json_set("{}", "location", nsLocation))
   pfree(nsLocation)

6. meta_info = META.GetNamespace(p_namespace)
7. return {"namespace": [meta_info.namespace_name],
           "properties": json_parse(meta_info.properties)}
```

### 8.7 drop_namespace

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

### 8.8 update_namespace_properties

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

### 8.9 rename_table

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

### 8.10 create_table

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

7. // 缓存 schema fields
   META.InsertSchemaFields(table->GetTableUUID(), 0,
                            table->GetCurrentSchema()->GetSchemaJson())

8. // 缓存 partition spec（有分区时展开 fields，无分区时写入占位记录 field_position=-1）
   if p_partition_spec:
       META.InsertPartitionSpec(table->GetTableUUID(), 0,
                                 jsonb_to_cstring(p_partition_spec.fields))
   else:
       META.InsertPartitionSpec(table->GetTableUUID(), 0, placeholder_record)

9. metadata_json = table->GetMetadataJson()
   delete table
   return {
       "metadata-location": table->GetMetadataLocation(),
       "metadata":          json_parse(metadata_json),
       "config":            {}
     }
```

### 8.11 load_table

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

### 8.12 drop_table

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

### 8.13 commit_table

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

7. // 缓存 snapshot 摘要
   snap = table->GetCurrentSnapshot()
   META.InsertSnapshot(info.table_uuid, snap->GetSnapshotId(),
                        snap->GetSchemaId(), snap->GetTimestampMs(),
                        snap->GetManifestList(),
                        json_extract_int(snap->GetSummaryJson(), "total-records"))

8. metadata_json = table->GetMetadataJson()
   delete table
   return {
       "metadata-location": newMdlLocation,
       "metadata":          json_parse(metadata_json)
     }
```

### 8.14 add_column

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

10. // 缓存新 Schema 字段
    META.InsertSchemaFields(info.table_uuid, newSchemaId,
                             newSchema->GetSchemaJson())

11. metadata_json = table->GetMetadataJson()
    delete table
    return {
        "metadata-location": newMdlLocation,
        "metadata":          json_parse(metadata_json)
      }
```


### 8.15 并发调用分析

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

#### 8.15.1 各函数并发分析

| 函数 | 写顺序 | 并发机制 | 安全？ |
|------|--------|---------|--------|
| `create_namespace` | **META INSERT → SDK CreateNamespace** | META INSERT 的 PK `(namespace)` 约束仲裁冲突。后到达的请求阻塞等待→前一个 COMMIT 后→PK 冲突→P0005，未调用 SDK | ✅ |
| `drop_namespace` | **META DELETE → SDK DropNamespace** | META DELETE 在事务中。SDK cleanup 为 best-effort | ✅ |
| `update_namespace_properties` | 纯 META（SELECT FOR UPDATE → UPDATE） | 行锁串行化。不涉及 SDK | ✅ |
| `rename_table` | **META Update → (可选) SDK RenameTable** | META 的 PK 约束仲裁目标表名冲突。SDK 为可选预留 | ✅ |
| `create_table` | SDK CreateTable → DDL CreateStorage → **META INSERT** | META INSERT 的 PK `(namespace, table_name)` 是最终冲突仲裁点。由于 `table_uuid` 和 `relid` 分别由 SDK 和 DDL 生成，META INSERT 必须在 SDK+DDL 之后。并发场景下两个请求都会执行 SDK S3 写入，但仅一个通过 META INSERT。孤儿 S3 文件的概率性风险可接受（详见 8.15.2） | ✅ |
| `drop_table` | **META GetTableForUpdate → DDL → META DeleteTable → (best-effort) SDK DropTable** | FOR UPDATE 锁 + DELETE 原子性。SDK 为 best-effort | ✅ |
| `commit_table` | **META GetTableForUpdate(FOR UPDATE) → SDK LoadTable → SDK CommitTable → META UpdateTable(乐观锁)** | FOR UPDATE 行锁串行化同一表的并发 commit。SDK S3 写在持有锁期间完成。META UpdateTable 的乐观锁（`WHERE metadata_location = old`）作为防御层 | ✅ |
| `add_column` | 同 `commit_table` | 同 `commit_table` | ✅ |

#### 8.15.2 `create_table` 并发分析

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
- `create_namespace` 无此问题：META INSERT 先于 SDK，并发请求在 META 层即被阻塞

#### 8.15.3 `commit_table` / `add_column` 的 FOR UPDATE 串行化

```
Request A: ──META GetTableForUpdate──┬──SDK Load──┬──SDK CommitTable(S3写)──┬──META UpdateTable──┬──COMMIT──
              (获取行锁)              │            │                         │                    │
Request B: ──META GetTableForUpdate──┴── [阻塞] ──┴── [阻塞] ──────────────┴── [阻塞] ──────────┴── A COMMIT后获取锁，重新读取最新 metadata_location
```

- `GetTableForUpdate` 内部 `SELECT ... FOR UPDATE` 获取行锁，串行化同一表的并发操作
- SDK S3 写在持有锁期间完成——其他请求被阻塞，不会并发写同一表的 S3
- `META UpdateTable` 的乐观锁（`WHERE metadata_location = old`）提供额外防御层

#### 8.15.4 SDK 失败的回滚保证

| 函数 | 失败场景 | 结果 |
|------|---------|------|
| `create_namespace` | META INSERT 成功 → SDK 失败 | ✅ 事务回滚，INSERT 撤销，SDK 写入失败无残留 |
| `create_table` | SDK 成功 → DDL 成功 → META INSERT 冲突 | ⚠️ 事务回滚，DDL 表自动删除（DDL 事务性），S3 残留孤儿 metadata JSON（低概率，可接受） |
| `create_table` | SDK 成功 → DDL 失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON |
| `commit_table` | FOR UPDATE 成功 → SDK CommitTable 成功 → META UpdateTable 乐观锁失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON（FOR UPDATE 下极少发生） |
| 所有函数 | META 成功 → SDK 失败 | ✅ 事务回滚，META 变更撤销，SDK 写入失败无残留 |

> **孤儿文件**：仅在 SDK S3 写入成功后 META/DDL 操作失败的极端场景产生（`create_table` 并发冲突、`commit_table` 乐观锁失败等）。孤儿文件处理不在本文档范围内（详见 9.2）。

---

## 9. 事务与一致性

### 9.1 一致性模型

系统元数据的一致性由 **元信息模块** + **OpenGauss 事务机制** 保障。SQL 自定义函数调用的所有 `iceberg_meta_*` 操作与外围查询处于同一事务（详见第 3 章）。

```
客户端调用 SQL 函数
   │
   ├── OpenGauss 事务开始 (或已在事务中)
   │
   ├── ① C++ 实现函数开始
   │      │
   │      ├── ①-a iceberg_meta_* 操作元信息表（事务性，元信息模块内部保证）
   │      │      └── 先写元信息表，利用 DB 事务机制仲裁冲突
   │      │
   │      └── ①-b Iceberg SDK: 写入 metadata.json 到 S3（非事务性）
   │             └── 若失败则事务回滚，元信息表变更自动撤销
   │
   ├── ② DDL管理模块: 创建/更新 delta 表和外表（事务性）
   │
   └── 事务 COMMIT / ROLLBACK
         │
         ├── COMMIT → 元信息表变更 + S3 文件同时生效，metadata_location 指向新元数据
         └── ROLLBACK → 元信息表变更撤销。若 S3 已写入（极少情况），该文件成为孤儿
```

### 9.2 孤儿文件处理

> **暂不考虑**。孤儿文件（S3 写入成功但事务回滚导致的残留 metadata.json 文件）的处理策略留待后续设计，不在本文档范围内。

### 9.3 并发控制策略

| 场景 | 机制 |
|------|------|
| 同一表的并发 commit | `SELECT ... FOR UPDATE` + `UPDATE WHERE metadata_location = oldLocation` |
| namespace 删 + 表创建 | `FOREIGN KEY ... ON DELETE RESTRICT` |
| 同一 namespace 下表名冲突 | `PRIMARY KEY (namespace, table_name)` |
| read-after-write | 元信息表在同一事务内提交，读取方可见 |

### 9.4 与 Spark 侧一致性交互

```
Spark (REST/JDBC Catalog Client)
   │
   ├── REST API → REST 适配组件 → SQL 函数
   │      │                           │
   │      │   HTTP 请求含 requirements │  C++ 实现函数内:
   │      │   (assert-ref-snapshot-id) │   META + SDK 在同一事务
   │      │                           │
   │      └── HTTP 200 (LoadTableResponse)
   │
   └── Spark 写入 Data/Delete File 到 S3
         │
         └── commit_table (add-snapshot)
               │
               └── 事务内: SDK S3 写 + META.UpdateTable
```

---

## 10. 错误码映射

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

## 11. 待确认项

1. **Delta 表和 FDW 外表的管理**：当前设计假设有独立的 DDL 管理模块负责。需确认 SQL 函数是否直接调用该模块，还是通过回调机制。

2. **Iceberg SDK 的成熟度**：当前接口定义基于 pg_lake 的 SDK 模式假设。需确认 SDK 是否已实现本文所列全部接口，或需分阶段交付。

3. **`p_stage_create` 实现时机**：当前标记为暂不实现。需确认何时需要支持 Stage Create 事务模式。

4. **External 表处理**：`tables_external` 由 Spark JDBC Catalog 直连时使用，SQL 函数设计不涉及此表，所有函数仅操作 `tables_internal`。

5. **S3 凭证注入**：`load_table` 返回的 `config` 字段需包含 S3 访问凭证。需确认凭证由哪个模块注入（SQL 函数自身还是独立的凭证代理）。

6. **元信息模块接口实现**：第 7 章定义的 `iceberg_meta_*` 接口需由元信息模块设计文档承接实现。需确认接口签名是否满足元信息模块的实现约束。

7. **JSONB 构造接口**：需确认 C++ 层构造 JSONB 的具体方式（方案 A：通过 SPI 调用 `jsonb_build_object()` SQL 函数；方案 B：直接构造 `Jsonb` 结构）。建议采用方案 A。

8. **next-row-id 的写入语义**：需确认 `next-row-id` 在 Iceberg v3 中的具体语义：
   - 初始值是 0 还是 1？
   - 递增粒度（每次 +1 还是批量预留一段？）
   - 当前设计：`next-row-id` 不落表（`metadata_table_design.md` §4.3），SQL 函数只在 `load_table` 的 metadata JSON 中通过 SDK `GetNextRowId()` 返回。确认是否满足 Spark JDBC Catalog 等外部组件的需要？

9. **孤儿文件处理**：暂不在本文档范围内，留待后续设计。
