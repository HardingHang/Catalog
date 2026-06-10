# Iceberg Metadata SQL Function 实现设计

## 1. 目标

本文档为 SQL 自定义函数层（对应 `iceberg_metadata_sql_func_def_design.md` 中定义的 14 个函数）做实现设计，基于 **Iceberg v3** 规范。明确事务语义下的 SQL Function 实现逻辑及并发安全性。

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

## 3. Iceberg SDK 接口诉求

### 3.1 架构说明：JNI 调用链与 ereport 安全性

Iceberg SDK 的实际实现是 **Java**（Apache Iceberg 官方库），通过 **JNI** 被 C++ 层调用。SQL 自定义函数不直接接触 JNI——中间有一层 C++ 头文件定义 API，其实现负责 JNI 调用。

```
SQL 自定义函数 (C++)
       │
       ├── 调用 Iceberg SDK接口
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

因此 C++ Wrapper 层遵循以下流程：

1. 发起 JNI 调用 → Java Iceberg SDK 执行实际操作
2. JNI 调用完全返回（此时栈上已无 JNI 帧）
3. 检查 Java 异常状态
   - 无异常 → 将结果返回给 SQL 函数层
   - 有异常 → 提取错误消息 → 清理 Java 异常状态 → 由 SQL 函数层通过 ereport 抛出（此时 longjmp 安全，不跨越 JNI 帧）

**核心约束**：ereport 只能在 JNI 完全返回后触发，不能跨越 JNI 栈帧。

#### 3.1.1 错误处理约定：通用错误消息 + SQL 层格式化

SDK 和元信息模块不仅被 SQL 自定义函数调用，也可能被其他模块（REST 适配器、后台任务等）调用。因此接口**不应**在 `ereport` 中硬编码 Iceberg REST API 的 JSON 错误格式。

**约定**：所有可失败的 SDK/META 方法通过 `char **error_msg` 输出参数返回**纯文本错误描述**。由调用方（SQL 函数层）负责将其包装为 Iceberg REST API JSON 格式并调用 `ereport`。

```
┌──────────────────────────────────────────────────────────────┐
│ SQL 自定义函数层                                              │
│                                                               │
│   调用 SDK/META 接口                                           │
│   检查返回的错误消息                                           │
│   若非空 → 包装为 Iceberg REST API JSON 格式后 ereport         │
│                         ↑                                     │
│              纯文本 → JSON 格式化                              │
└────────────────────────┬─────────────────────────────────────┘
                         │ error_msg = "Failed to read..."
                         │
┌────────────────────────┴───────────────────────────────────┐
│ SDK / META 层（通用，不感知 JSON 格式）                        │
│   *error_msg = "Failed to read metadata from S3: timeout"    │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 IcebergCatalog — 功能诉求

Catalog 入口对象，SQL 函数层需要其提供以下能力：

**生命周期管理：**
- 通过仓库路径（warehouse location）初始化 Catalog 实例，支持关闭/释放

**Namespace S3 操作：**
- 解析 S3 路径并创建 namespace marker，返回解析后的 S3 location
- 清理 S3 marker（best-effort）

**Table 操作：**
- 从 S3 读取 metadata JSON 并解析为 Table 对象（需传入 metadata_location）
- 创建表：内部完成 UUID 生成 → schema 解析 → 确定 table_location → 构造 metadata → 写 S3。可接受 location_hint（NULL 时由 SDK 推导）、partition_spec（NULL 表示无分区）、write_order（NULL 表示无排序）、properties（NULL 表示空属性）
- 清理 S3 数据（best-effort）
- 重命名表涉及的 S3 路径迁移（预留，当前可为空操作）

**类型校验：**
- 校验 Iceberg 类型字符串（如 `decimal(P,S)`、`fixed(L)` 等）是否合法，失败时返回错误描述

### 3.3 IcebergTable — 功能诉求

Table 对象，SQL 函数层需要其提供以下能力：

**基本信息查询：**
- 获取 table UUID
- 获取 table S3 路径（由 SDK 确定）
- 获取当前 metadata JSON 的 S3 路径
- 获取 next-row-id（Iceberg v3）

**Metadata 访问：**
- 获取完整 metadata JSON（含 schemas、snapshots、partition-specs 等）

**辅助信息：**
- 获取 current_schema_id、last_column_id、default_partition_spec_id

**Schema 操作：**
- 获取当前 schema 对象
- 追加列（传入列名、类型、文档），返回新 schema 及新分配的 field ID

**Commit：**
- 应用 requirements + updates → 构造新 metadata → 写入 S3，返回新 metadata_location

**Snapshot 访问：**
- 获取当前 snapshot 对象

### 3.4 IcebergSchema — 功能诉求

Schema 只读数据对象，提供：schema ID 查询、JSON 序列化、字段数量/名称/类型/ID 的逐一访问。

### 3.5 IcebergSnapshot — 功能诉求

Snapshot 只读数据对象，提供：snapshot ID、时间戳、manifest list 路径、关联 schema ID、父 snapshot ID、summary JSON 的访问。

### 3.6 关键设计点

| 设计点 | 说明 |
|--------|------|
| **JNI 安全** | C++ Wrapper 层在 JNI 调用**返回后**才检查错误并填充 `*error_msg`，确保 `ereport` 调用的 `longjmp` 不跨越 JNI 栈帧 |
| **通用错误消息** | 所有可失败的 SDK 方法通过 `char **error_msg` 返回**纯文本**错误描述。SQL 函数层负责将其包装为 Iceberg REST API JSON 格式后调用 `ereport`。这使 SDK 可被其他模块复用 |
| **路径封装** | SDK 内部处理所有路径解析，SQL 函数只通过数据对象读取结果 |
| **SDK 最小化** | SDK 仅包含需要 Iceberg 语义或 S3 访问的操作。纯元信息表查询由元信息模块负责 |
| **命名空间简化** | Namespace 无需封装为对象——元数据在 META 中，SDK 只负责 S3 路径 |
| **生命周期** | Catalog 实例为会话级单例，Table 对象在使用后释放 |

---

## 4. 元信息模块接口诉求

SQL 自定义函数通过**元信息模块**操作元信息表。本章描述 SQL 自定义函数视角下需要的功能诉求——仅定义行为语义，不涉及具体接口签名。

> **实现归属**：以下功能的实现属于元信息模块，不在本设计范围内。本设计仅约定功能契约。

> **错误处理约定**（与 SDK 一致，见 3.1.1）：可失败的 META 操作通过 `char **error_msg` 返回**纯文本**错误描述。SQL 函数层负责将其包装为 Iceberg REST API JSON 格式并 `ereport`。存在性检查类操作（如"是否存在"）仅返回 true/false，不抛错。写入类操作在失败时设置 `*error_msg`。

> **事务语义**：事务语义由**元信息模块**内部保证。SQL 自定义函数调用的所有 `iceberg_meta_*` 接口均在当前 SQL 函数的事务上下文中执行——元信息模块的底层实现（SPI 或直接表访问）不改变此保证。

### 4.1 命名空间操作

- **存在性检查**：查询指定 namespace 是否存在（不抛错，返回 true/false）
- **读取信息**：读取 namespace 的详细信息（名称、properties），未找到时返回 NULL
- **创建**：写入 namespace 记录，已存在则返回冲突错误（P0005）
- **删除**：删除 namespace 记录，不存在则返回 P0004
- **更新 properties**：对 namespace properties 执行 removals（JSONB 数组）和 updates（JSONB object），返回 `{"updated": [...], "removed": [...], "missing": [...]}` 格式的 JSON 结果
- **分页列出**：按 parent 和 page_token 分页列出 namespace，返回 `{"identifiers": [[<命名空间层级>...], ...], "next-page-token": "<分页令牌>"}` 格式的 JSON 结果
- **检查子表**：检查 namespace 下是否存在表（用于 drop_namespace 前置校验）

### 4.2 表操作

- **存在性检查**：查询指定表是否存在（不抛错，返回 true/false）
- **读取元信息**：读取表的完整元信息记录，未找到时返回 P0004
- **读取并加锁**：读取表元信息并加行锁（用于 commit_table / add_column / drop_table 等写操作）
- **创建记录**：写入表记录（包含 relid、table_uuid、metadata_location、table_location 等字段），冲突时返回 P0005
- **更新记录**：更新表元信息，以旧 metadata_location 为乐观锁条件（不匹配则返回 P0005），同时更新 snapshot_id、schema_id、last_column_id
- **删除记录**：删除表记录，不存在则返回 P0004
- **分页列出**：按 namespace、page_size、page_token 分页列出表，返回 `{"identifiers": [{"namespace": [...], "name": "<表名>"}, ...], "next-page-token": "<分页令牌>"}` 格式的 JSON 结果
- **重命名**：更新表名记录。源不存在→P0004，目标已存在→P0005，目标 NS 不存在→P0004

### 4.3 接口约定

| 约定项 | 说明 |
|--------|------|
| **内存管理** | 元信息模块返回的指针由调用方自行释放 |
| **错误处理** | 存在性检查类操作仅返回 true/false，不抛错。查询操作在"未找到"时返回 NULL。写入类操作在失败时 `ereport(ERROR, ...)` 并携带正确的 SQLSTATE（P0001~P0009）和 JSON 格式错误消息 |
| **事务** | 所有操作与 SQL 自定义函数处于同一事务（元信息模块内部保证） |
| **乐观锁** | 表更新操作以旧 metadata_location 为乐观锁条件，不匹配时 ereport(P0005) |

---

## 5. 函数实现逻辑

以下为 14 个 SQL 自定义函数的详细实现设计。每个函数包含功能说明、入参含义、返回值格式及分步骤实现流程。实现中通过元信息模块（简称 META）操作元信息表，通过 Iceberg SDK（简称 SDK）完成 S3 上的元数据读写。SDK/META 接口失败时返回纯文本错误描述，SQL 函数层负责将其包装为 Iceberg REST API JSON 格式后通过 ereport 抛出。

### 5.0 SQL 函数与 Iceberg REST API 映射

以下表格列出 14 个 SQL 自定义函数与 [Iceberg REST Catalog API](https://github.com/apache/iceberg/blob/main/open-api/rest-catalog-open-api.yaml) 中对应接口的映射关系。

| SQL 函数 | HTTP 方法与路径 | operationId | 说明 |
|---------|---------------|------------|------|
| `is_namespace_existed` | `HEAD /v1/{prefix}/namespaces/{namespace}` | `namespaceExists` | 检查命名空间是否存在，返回 204（存在）或 404（不存在） |
| `is_table_existed` | `HEAD /v1/{prefix}/namespaces/{namespace}/tables/{table}` | `tableExists` | 检查表是否存在，返回 204（存在）或 404（不存在） |
| `load_namespace` | `GET /v1/{prefix}/namespaces/{namespace}` | `loadNamespaceMetadata` | 加载命名空间的元数据属性 |
| `list_namespaces` | `GET /v1/{prefix}/namespaces` | `listNamespaces` | 分页列出命名空间，支持 parent 参数过滤子级 |
| `list_tables` | `GET /v1/{prefix}/namespaces/{namespace}/tables` | `listTables` | 分页列出指定命名空间下的表 |
| `create_namespace` | `POST /v1/{prefix}/namespaces` | `createNamespace` | 创建命名空间，请求体包含 namespace 和 properties |
| `drop_namespace` | `DELETE /v1/{prefix}/namespaces/{namespace}` | `dropNamespace` | 删除命名空间，要求命名空间为空（无子表） |
| `update_namespace_properties` | `POST /v1/{prefix}/namespaces/{namespace}/properties` | `updateProperties` | 更新命名空间属性，请求体包含 removals 和 updates |
| `rename_table` | `POST /v1/{prefix}/tables/rename` | `renameTable` | 重命名表，请求体包含 source 和 destination 标识 |
| `create_table` | `POST /v1/{prefix}/namespaces/{namespace}/tables` | `createTable` | 创建表，支持 `stage-create` 参数 |
| `load_table` | `GET /v1/{prefix}/namespaces/{namespace}/tables/{table}` | `loadTable` | 加载表元数据和配置信息 |
| `drop_table` | `DELETE /v1/{prefix}/namespaces/{namespace}/tables/{table}` | `dropTable` | 删除表，支持 `purgeRequested` 参数 |
| `commit_table` | `POST /v1/{prefix}/namespaces/{namespace}/tables/{table}` | `updateTable` | 提交表变更（requirements + updates），最终提交和阶段创建均使用此接口 |
| `add_column` | `POST /v1/{prefix}/namespaces/{namespace}/tables/{table}` | `updateTable` | REST API 无独立加列接口，通过 `updateTable` 提交 `add-schema` 和 `set-current-schema` 更新实现 |

> **注**：上述映射对应 Iceberg REST Catalog 规范中已定义的接口。14 个 SQL 函数是这些 REST API 在 PostgreSQL/OpenGauss 自定义函数层的封装，扩展了参数校验、DDL 管理模块调用、元信息表持久化等本地逻辑。

### 5.1 is_namespace_existed

**功能**：检查指定命名空间是否存在。

**接口参数**：

- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。

**返回值**：JSONB 格式，包含 `exists` 字段（布尔类型），true 表示命名空间存在，false 表示不存在。

**返回值样例**：
```json
{"exists": true}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：通过 META 查询元信息表中是否存在该命名空间的记录，将结果包装为 `{"exists": true}` 或 `{"exists": false}` 的 JSONB 格式返回。

### 5.2 is_table_existed

**功能**：检查指定命名空间下的指定表是否存在。

**接口参数**：

- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_table`：表名称（TEXT 类型）。不能为 NULL 或空字符串。

**返回值**：JSONB 格式，包含 `exists` 字段（布尔类型），true 表示表存在，false 表示不存在。

**返回值样例**：
```json
{"exists": true}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：校验入参 p_table 是否为 NULL 或空字符串。若是，则报P0001错误，提示"table must not be empty"；
步骤3：通过 META 查询元信息表中是否存在该表的记录，将结果包装为 `{"exists": true}` 或 `{"exists": false}` 的 JSONB 格式返回。

### 5.3 load_namespace

**功能**：加载指定命名空间的详细信息。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。

**返回值**：JSONB 格式，包含命名空间的名称和 properties（属性键值对）。若命名空间不存在则报错。

**返回值样例**：
```json
{
  "namespace": ["<命名空间层级1>", "<命名空间层级2>"],
  "properties": {
    "location": "s3://<bucket>/<warehouse>/<命名空间路径>",
    "<自定义属性key>": "<自定义属性value>"
  }
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：通过 META 读取指定命名空间的元信息记录。若记录不存在，则报P0004错误，提示"namespace not found"；
步骤3：将读取到的命名空间名称和 properties 包装为 JSONB 格式返回。

### 5.4 list_namespaces

**功能**：分页列出命名空间。

**接口参数**：
- `p_parent`：父级命名空间名称（TEXT 类型，可选，默认为 NULL）。若指定，则只列出该父级下的子命名空间；若为 NULL，则列出顶层命名空间。
- `p_page_size`：每页返回的最大记录数（INT 类型，可选，默认为 1000）。必须大于等于 1。
- `p_page_token`：分页令牌（TEXT 类型，可选，默认为 NULL）。用于获取下一页结果，首次查询时为 NULL。

**返回值**：JSONB 格式，包含命名空间列表（identifiers 数组）和下一页分页令牌（next-page-token）。

**返回值样例**：
```json
{
  "identifiers": [
    ["<命名空间层级1>", "<子命名空间1>"],
    ["<命名空间层级1>", "<子命名空间2>"]
  ],
  "next-page-token": "<下一页令牌，无更多结果时该字段省略>"
}
```

**实现逻辑**：
步骤1：校验入参 p_page_size 是否小于 1。若是，则报P0001错误，提示"pageSize must be >= 1"；
步骤2：若 p_parent 不为空，通过 META 检查该父级命名空间是否存在。若不存在，则报P0004错误，提示"namespace not found"；
步骤3：通过 META 按 parent 和 page_token 分页查询元信息表，将查询结果解析为JSON 格式返回。

### 5.5 list_tables

**功能**：分页列出指定命名空间下的表。

**接口参数**：

- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_page_size`：每页返回的最大记录数（INT 类型，可选，默认为 1000）。必须大于等于 1。
- `p_page_token`：分页令牌（TEXT 类型，可选，默认为 NULL）。用于获取下一页结果，首次查询时为 NULL。

**返回值**：JSONB 格式，包含表列表（identifiers 数组）和下一页分页令牌（next-page-token）。

**返回值样例**：
```json
{
  "identifiers": [
    {"namespace": ["<命名空间层级1>"], "name": "<表名1>"},
    {"namespace": ["<命名空间层级1>"], "name": "<表名2>"}
  ],
  "next-page-token": "<下一页令牌，无更多结果时该字段省略>"
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：校验入参 p_page_size 是否小于 1。若是，则报P0001错误，提示"pageSize must be >= 1"；
步骤3：通过 META 检查指定命名空间是否存在。若不存在，则报P0004错误，提示"namespace not found"；
步骤4：通过 META 按 namespace、page_size 和 page_token 分页查询元信息表，将查询结果解析为 JSON 格式返回。

### 5.6 create_namespace

**功能**：创建新的命名空间。采用"先写元信息表、后写 S3"的顺序，利用元信息表主键约束仲裁并发冲突，避免产生 S3 孤儿文件。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_properties`：命名空间属性（JSONB 类型，可选，默认为 NULL）。若指定，必须为 JSONB object 格式。可包含 "location" key 用于指定 S3 存储路径。

**返回值**：JSONB 格式，包含创建后的命名空间名称和完整的 properties（含 SDK 返回的 location 字段）。

**返回值样例**：
```json
{
  "namespace": ["<命名空间层级1>", "<命名空间层级2>"],
  "properties": {
    "location": "s3://<bucket>/<warehouse>/<命名空间路径>",
    "<自定义属性key>": "<自定义属性value>"
  }
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：若 p_properties 不为 NULL，校验其是否为合法的 JSONB object 格式。若不是，则报P0001错误，提示"properties must be a valid JSONB object"；
步骤3：先写元信息表（利用主键约束仲裁并发冲突，详见第6章）。将 p_properties 转为字符串（NULL 则转为 "{}"）。通过 META 检查该命名空间是否已在元信息表中存在，若已存在则报P0005错误，提示"namespace already exists"。若不存在，则通过 META 写入命名空间记录。若用户已在 p_properties 中指定了 location，则直接使用该值写入；否则先用临时值占位，待 SDK 返回实际路径后再更新；
步骤4：通过 SDK 创建 namespace（解析 S3 路径并创建 marker）。若 SDK 返回错误消息，则将其包装为 ServiceUnavailable JSON 格式后报P0009错误，提示SDK返回的错误信息。若此步骤失败，事务将回滚，步骤3 写入的元信息表记录自动撤销；
步骤5：若 p_properties 中不包含 "location" key，则通过 META 更新命名空间 properties，将 SDK 返回的 location 写入；
步骤6：通过 META 重新读取命名空间信息，将名称和完整的 properties 包装为 JSONB 格式返回。

### 5.7 drop_namespace

**功能**：删除指定命名空间。先检查并删除元信息表记录，再通过 SDK 清理 S3 上的 marker（best-effort）。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。

**返回值**：JSONB 格式，包含 `success` 字段（布尔类型），固定为 true。

**返回值样例**：
```json
{"success": true}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：通过 META 检查指定命名空间是否存在。若不存在，则报P0004错误，提示"namespace not found"；
步骤3：通过 META 检查该命名空间下是否仍存在表。若存在表，则报P0005错误，提示"namespace is not empty, cannot be dropped"；
步骤4：通过 META 删除该命名空间的元信息记录；
步骤5：通过 SDK 清理该命名空间在 S3 上对应的 marker（best-effort 操作，失败不影响整体结果）；
步骤6：返回 `{"success": true}`。

### 5.8 update_namespace_properties

**功能**：更新命名空间的属性（properties）。支持同时删除和添加属性，两者不能有交集。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_removals`：待删除的属性 key 列表（JSONB 类型，可选，默认为 NULL）。若指定，必须为 JSONB 数组格式。
- `p_updates`：待添加或更新的属性键值对（JSONB 类型，可选，默认为 NULL）。若指定，必须为 JSONB object 格式。

**返回值**：JSONB 格式，包含三个数组字段：`updated`（已成功更新的 key 列表）、`removed`（已成功删除的 key 列表）、`missing`（待删除但原 properties 中不存在的 key 列表）。

**返回值样例**：
```json
{
  "updated": ["<已更新的属性key1>", "<已更新的属性key2>"],
  "removed": ["<已删除的属性key1>"],
  "missing": ["<待删除但不存在的属性key>"]
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 是否为 NULL 或空字符串。若是，则报P0001错误，提示"namespace must not be empty"；
步骤2：若 p_removals 和 p_updates 同时为 NULL，则报P0001错误，提示"at least one of removals or updates must be specified"；
步骤3：若 p_removals 不为 NULL，校验其是否为合法的 JSONB 数组格式。若不是，则报P0001错误，提示"removals must be a valid JSONB array"；
步骤4：若 p_updates 不为 NULL，校验其是否为合法的 JSONB object 格式。若不是，则报P0001错误，提示"updates must be a valid JSONB object"；
步骤5：检查 p_removals 和 p_updates 是否存在交集（即同一个 key 既在删除列表中又在更新列表中）。若存在交集，则报P0006错误，提示"removals and updates must not have common keys"；
步骤6：将 p_removals 和 p_updates 转为字符串（NULL 分别转为 "[]" 和 "{}"），通过 META 执行 properties 更新操作；
步骤7：将 META 返回的更新结果（含 updated、removed、missing 三个数组）解析为 JSONB 格式返回。

### 5.9 rename_table

**功能**：重命名表。将源表从源命名空间移动到目标命名空间并更名为目标表名。

**接口参数**：
- `p_src_ns`：源命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_src_table`：源表名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_dst_ns`：目标命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_dst_table`：目标表名称（TEXT 类型）。不能为 NULL 或空字符串。

**返回值**：JSONB 格式，包含 `success` 字段（布尔类型），固定为 true。

**返回值样例**：
```json
{"success": true}
```

**实现逻辑**：
步骤1：校验四个入参是否存在 NULL 或空字符串。若有任一参数不符合要求，则报P0001错误，提示"namespace and table name must not be empty"；
步骤2：通过 META 检查源表是否存在。若不存在，则报P0004错误，提示"table not found"；
步骤3：通过 META 检查目标命名空间是否存在。若不存在，则报P0004错误，提示"namespace not found"；
步骤4：通过 META 检查目标表名在目标命名空间下是否已存在。若已存在，则报P0005错误，提示"table already exists"；
步骤5：通过 META 执行表重命名操作（更新元信息表中的表名和命名空间字段）；
步骤6：通过 SDK 执行 S3 路径迁移（预留给未来实现，标准 Iceberg 场景下当前为空操作）。若 SDK 返回错误消息，则将其包装为 ServiceUnavailable JSON 格式后报P0009错误，提示SDK返回的错误信息；
步骤7：返回 `{"success": true}`。

### 5.10 create_table

**功能**：创建新的 Iceberg 表。依次完成 Schema 校验、SDK 创建表（生成 UUID 并写 S3）、DDL 模块创建存储、元信息表写入。元信息表的主键约束为最终并发仲裁点。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_table_name`：表名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_schema`：表的 Schema 定义（JSONB 类型，必填）。type 必须为 "struct"，其中每个字段的 Iceberg 类型需通过 SDK 校验合法性。
- `p_location`：表的 S3 存储基路径（TEXT 类型，可选，默认为 NULL）。若为 NULL，由 SDK 自动推导。
- `p_partition_spec`：分区规范（JSONB 类型，可选，默认为 NULL）。NULL 表示无分区。
- `p_write_order`：写入排序规范（JSONB 类型，可选，默认为 NULL）。NULL 表示无排序。
- `p_stage_create`：是否为分阶段创建（BOOL 类型，可选，默认为 FALSE）。
- `p_properties`：表属性（JSONB 类型，可选，默认为 NULL）。NULL 表示空属性。

**返回值**：JSONB 格式，包含完整的 metadata JSON（含 schemas、snapshots、partition-specs、properties 等）。

**返回值样例**：
```json
{
  "metadata-location": "s3://<bucket>/<warehouse>/<namespace>/<table>/metadata/v1.metadata.json",
  "metadata": {
    "format-version": 2,
    "table-uuid": "<SDK生成的UUID>",
    "location": "s3://<bucket>/<warehouse>/<namespace>/<table>",
    "last-sequence-number": <序列号>,
    "last-updated-ms": <更新时间戳>,
    "last-column-id": <SDK返回的last_column_id>,
    "current-schema-id": <SDK返回的current_schema_id>,
    "schemas": [{"type": "struct", "schema-id": <ID>, "fields": [<字段列表>]}],
    "default-spec-id": <SDK返回的default_partition_spec_id>,
    "partition-specs": [<分区规范列表>],
    "default-sort-order-id": <SDK返回的default_sort_order_id>,
    "sort-orders": [<排序规范列表>],
    "properties": {<表属性键值对>},
    "current-snapshot-id": -1,
    "snapshots": [],
    "snapshot-log": [],
    "metadata-log": []
  },
  "config": {<配置信息>}
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 和 p_table_name 是否为 NULL 或空字符串。若有任一不符合，则报P0001错误，提示"namespace and table name must not be empty"；
步骤2：执行 Schema 校验。若 p_schema 的 type 字段不为 "struct"，则报P0001错误，提示"schema type must be 'struct'"。遍历 p_schema.fields 中的每个字段，通过 SDK 校验其 Iceberg 类型字符串是否合法；若任一字段类型不合法，则报P0001错误，提示SDK返回的类型校验错误信息；
步骤3：执行业务检查。通过 META 检查指定命名空间是否存在，若不存在则报P0004错误，提示"namespace not found"。通过 META 检查该命名空间下是否已存在同名表，若已存在则报P0005错误，提示"table already exists"；
步骤4：通过 SDK 创建表。将 p_schema、p_partition_spec、p_write_order、p_properties 转为字符串（NULL 转为 NULL 指针传入）。SDK 内部完成 UUID 生成、S3 路径解析、metadata JSON 构造和 S3 写入。若 SDK 返回错误消息，则将其包装为 ServiceUnavailable JSON 格式后报P0009错误，提示SDK返回的错误信息；
步骤5：调用 DDL 管理模块创建 delta 表及 FDW 外表（传入命名空间名、表名、SDK 返回的 table_uuid），获取系统分配的 relid；
步骤6：写入元信息表，利用主键 (namespace, table_name) 仲裁并发冲突（详见 6.2）。通过 META 写入表记录，包含以下字段：relid（DDL 模块返回）、table_uuid、metadata_location、table_location、last_column_id、current_schema_id、default_partition_spec_id（以上由 SDK 返回）、previous_metadata_location（初始为 NULL）、current_snapshot_id（初始为 -1，表示尚无快照）；
步骤7：通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，将 metadata JSON 包装为 JSONB 格式返回。

### 5.11 load_table

**功能**：加载指定表的完整元数据信息。先从元信息表获取 metadata_location 指针，再通过 SDK 从 S3 读取并解析 metadata JSON。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_table`：表名称（TEXT 类型）。不能为 NULL 或空字符串。

**返回值**：JSONB 格式，包含完整的 metadata JSON（含 schemas、snapshots、partition-specs、properties 以及 Iceberg v3 的 next-row-id 等）。

**返回值样例**：
```json
{
  "metadata-location": "s3://<bucket>/<warehouse>/<namespace>/<table>/metadata/v<N>.metadata.json",
  "metadata": {
    "format-version": 2,
    "table-uuid": "<UUID>",
    "location": "s3://<bucket>/<warehouse>/<namespace>/<table>",
    "last-sequence-number": <序列号>,
    "last-updated-ms": <时间戳>,
    "last-column-id": <最大列ID>,
    "current-schema-id": <当前schema ID>,
    "schemas": [<schema列表>],
    "default-spec-id": <默认partition-spec ID>,
    "partition-specs": [<partition-spec列表>],
    "default-sort-order-id": <默认sort-order ID>,
    "sort-orders": [<sort-order列表>],
    "properties": {<表属性>},
    "current-snapshot-id": <当前快照ID>,
    "snapshots": [<快照列表>],
    "snapshot-log": [<快照日志>],
    "metadata-log": [<元数据日志>],
    "next-row-id": <Iceberg v3 next-row-id>
  },
  "config": {<配置信息>}
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 和 p_table 是否存在 NULL 或空字符串。若有任一不符合，则报P0001错误，提示"namespace and table name must not be empty"；
步骤2：通过 META 读取表元信息记录。若记录不存在，则报P0004错误，提示"table not found"；
步骤3：通过 SDK 加载表对象（传入步骤2 获取的 metadata_location，SDK 从 S3 读取并解析 metadata JSON，包含 next-row-id）。若 SDK 返回错误消息，则将其包装为 ServiceUnavailable JSON 格式后报P0009错误，提示SDK返回的错误信息；
步骤4：通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，将 metadata JSON 包装为 JSONB 格式返回。

### 5.12 drop_table

**功能**：删除指定的表。依次执行元信息表加锁读取、DDL 模块删除存储、元信息表记录删除、SDK 清理 S3 数据。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_table`：表名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_purge`：是否同时清理 S3 上的数据文件（BOOL 类型，可选，默认为 FALSE）。当前版本暂不支持，若传入 TRUE 则报P0008错误，提示"purge not yet implemented"。

**返回值**：JSONB 格式，包含 `success` 字段（布尔类型），固定为 true。

**返回值样例**：
```json
{"success": true}
```

**实现逻辑**：
步骤1：校验入参 p_namespace 和 p_table 是否存在 NULL 或空字符串。若有任一不符合，则报P0001错误，提示"namespace and table name must not be empty"；
步骤2：若 p_purge 为 TRUE，则报P0008错误，提示"purge not yet implemented"；
步骤3：通过 META 读取表元信息记录并加行锁（FOR UPDATE）。若记录不存在，则报P0004错误，提示"table not found"；
步骤4：调用 DDL 管理模块删除该表对应的 delta 表和 FDW 外表（传入命名空间名、表名、步骤3 获取的 table_uuid）；
步骤5：通过 META 删除表元信息记录（元信息模块内部通过 ON DELETE CASCADE 处理关联的快照、schema 等记录）；
步骤6：通过 SDK 清理该表在 S3 上对应的数据文件（best-effort 操作，失败不影响整体结果）；
步骤7：返回 `{"success": true}`。

### 5.13 commit_table

**功能**：提交表的变更。将快照（snapshot）等更新应用到表上，通过行锁和乐观锁双重机制保证并发安全。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL。
- `p_table`：表名称（TEXT 类型）。不能为 NULL。
- `p_requirements`：提交前置条件列表（JSONB 类型，必填）。用于乐观锁校验，确保表在读取后未被其他事务修改。
- `p_updates`：提交的更新操作列表（JSONB 类型，必填）。当前仅支持 action 为 "add-snapshot" 的更新。

**返回值**：JSONB 格式，包含提交后的完整 metadata JSON（含新追加的 snapshot）。

**返回值样例**：
```json
{
  "metadata-location": "s3://<bucket>/<warehouse>/<namespace>/<table>/metadata/v<N+1>.metadata.json",
  "metadata": {
    "format-version": 2,
    "table-uuid": "<UUID>",
    "location": "s3://<bucket>/<warehouse>/<namespace>/<table>",
    "last-sequence-number": <更新后的序列号>,
    "last-updated-ms": <新时间戳>,
    "last-column-id": <最大列ID>,
    "current-schema-id": <当前schema ID>,
    "schemas": [<schema列表>],
    "default-spec-id": <partition-spec ID>,
    "partition-specs": [<partition-spec列表>],
    "default-sort-order-id": <sort-order ID>,
    "sort-orders": [<sort-order列表>],
    "properties": {<表属性>},
    "current-snapshot-id": <新追加的snapshot ID>,
    "snapshots": [
      ...,
      {"snapshot-id": <新snapshot ID>, "sequence-number": <新序列号>, ...}
    ],
    "snapshot-log": [..., {"snapshot-id": <新snapshot ID>, "timestamp-ms": <时间戳>}],
    "metadata-log": [..., {"metadata-file": "<新metadata路径>", "timestamp-ms": <时间戳>}]
  },
  "config": {<配置信息>}
}
```

**实现逻辑**：
步骤1：校验四个入参是否存在 NULL。若有任一为 NULL，则报P0001错误，提示"parameters must not be null"；
步骤2：校验 p_updates 数组中每个元素的 action 字段是否均为 "add-snapshot"。若存在其他 action 类型，则报P0001错误，提示"only 'add-snapshot' action is supported"；
步骤3：通过 META 读取表元信息记录并加行锁（FOR UPDATE），串行化同一表的并发 commit 操作。若记录不存在，则报P0004错误，提示"table not found"；
步骤4：通过 SDK 加载表对象（传入步骤3 获取的 metadata_location，SDK 从 S3 读取并解析 metadata JSON）。若 SDK 返回错误消息，则将其包装为 ServiceUnavailable JSON 格式后报P0009错误，提示SDK返回的错误信息；
步骤5：通过 SDK 提交表变更（内部三步合一：应用 requirements → 追加 snapshot → 写新 metadata JSON 到 S3）。将 p_requirements 和 p_updates 转为字符串传给 SDK，SDK 返回新的 metadata_location。若 SDK 返回错误消息（如 requirements 校验不通过），则将其包装为 CommitFailedException JSON 格式后报P0005错误，提示SDK返回的提交失败信息；
步骤6：更新元信息表，以旧 metadata_location 为乐观锁条件（WHERE metadata_location = 旧值）。先从 p_updates 中提取新 snapshot ID，再通过 META 更新表记录。若在步骤3 和本步骤之间 metadata_location 被其他事务修改，乐观锁条件不匹配，则报P0005错误，提示"Commit conflict: the table has been modified concurrently"；
步骤7：通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，将 metadata JSON 包装为 JSONB 格式返回。

### 5.14 add_column

**功能**：向现有表添加新列。自动构造 requirements 和 updates，通过 SDK 扩展 schema 并提交，最后更新元信息表。与 commit_table 使用相同的行锁和乐观锁并发控制机制。

**接口参数**：
- `p_namespace`：命名空间名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_table`：表名称（TEXT 类型）。不能为 NULL 或空字符串。
- `p_column_name`：新列的名称（TEXT 类型）。不能为 NULL 或空字符串，且不能与表中已有列名重复。
- `p_column_type`：新列的 Iceberg 类型字符串（TEXT 类型，如 "int"、"string"、"decimal(10,2)" 等）。不能为 NULL 或空字符串，需通过 SDK 校验合法性。
- `p_column_doc`：新列的文档注释（TEXT 类型，可选，默认为 NULL）。

**返回值**：JSONB 格式，包含添加列后的完整 metadata JSON（含更新后的 schemas 列表和新 schema ID）。

**返回值样例**：
```json
{
  "metadata-location": "s3://<bucket>/<warehouse>/<namespace>/<table>/metadata/v<N+1>.metadata.json",
  "metadata": {
    ...,
    "last-column-id": <新分配的field ID>,
    "current-schema-id": <新schema ID>,
    "schemas": [
      ...,
      {
        "schema-id": <新schema ID>,
        "type": "struct",
        "fields": [
          ...<原有字段>,
          {"id": <新field ID>, "name": "<新列名>", "type": "<新列类型>", "required": false, "doc": "<新列文档>"}
        ]
      }
    ],
    ...,
    "current-snapshot-id": <快照ID不变>,
    ...
  },
  "config": {<配置信息>}
}
```

**实现逻辑**：
步骤1：校验入参 p_namespace、p_table、p_column_name、p_column_type 是否存在 NULL 或空字符串。若有任一不符合，则报P0001错误，提示"namespace, table, column name and column type must not be empty"；
步骤2：通过 SDK 校验 p_column_type 是否为合法的 Iceberg 类型字符串。若不合法，则报P0001错误，提示SDK返回的类型校验错误信息；
步骤3：通过 META 读取表元信息记录并加行锁（FOR UPDATE）。若记录不存在，则报P0004错误，提示"table not found"；
步骤4：通过 SDK 加载表对象（传入步骤3 获取的 metadata_location）。若 SDK 返回错误消息，则将其包装为 ServiceUnavailable JSON 格式后报P0009错误，提示SDK返回的错误信息；
步骤5：通过 SDK 获取当前 schema，检查 p_column_name 是否与已有字段重名。若存在同名字段，则报P0001错误，提示"column already exists"；
步骤6：通过 SDK 追加列（传入列名、类型和文档注释），获取 SDK 分配的新 field ID 和新 schema 对象。新 schema ID = 当前 schema ID + 1；
步骤7：自动构造提交所需的 requirements 和 updates。requirements 包含四项断言：assert-table-uuid（表的 UUID 匹配）、assert-ref-snapshot-id（当前快照 ID 匹配，ref 为 "main"）、assert-current-schema-id（当前 schema ID 匹配）、assert-last-assigned-field-id（最后分配的 field ID 匹配），全部取值自步骤3 获取的元信息。updates 包含两项操作：add-schema（新 schema JSON 及新的 last-column-id）和 set-current-schema（新 schema ID）；
步骤8：通过 SDK 提交表变更（将 requirements 和 updates 转为字符串，SDK 内部应用变更并写新 metadata JSON 到 S3）。若 SDK 返回错误消息，则将其包装为 CommitFailedException JSON 格式后报P0005错误，提示SDK返回的提交失败信息；
步骤9：更新元信息表（乐观锁：WHERE metadata_location = 旧值）。通过 META 更新表记录，更新字段包括 current_schema_id（新 schema ID）和 last_column_id（新 field ID）。snapshot 保持不变。若乐观锁条件不匹配，则报P0005错误，提示"Commit conflict: the table has been modified concurrently"；
步骤10：通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，将 metadata JSON 包装为 JSONB 格式返回。

## 6 并发调用和原子性分析

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

### 6.1 各函数并发分析

| 函数 | 写顺序 | 并发机制 | 安全？ |
|------|--------|---------|--------|
| `create_namespace` | **通过 META 写入 → 通过 SDK 创建 namespace** | 通过 META 写入时 PK `(namespace)` 约束仲裁冲突。后到达的请求阻塞等待→前一个 COMMIT 后→PK 冲突→P0005，未调用 SDK | ✅ |
| `drop_namespace` | **通过 META 删除 → 通过 SDK 清理 namespace** | 通过 META 删除在事务中。通过 SDK 清理为 best-effort | ✅ |
| `update_namespace_properties` | 纯 META（行锁 → 更新） | 行锁串行化。不涉及 SDK | ✅ |
| `rename_table` | **通过 META 更新 → (可选) 通过 SDK 迁移 S3 路径** | 通过 META 的 PK 约束仲裁目标表名冲突。通过 SDK 迁移为可选预留 | ✅ |
| `create_table` | 通过 SDK 创建表 → DDL 模块创建存储 → **通过 META 写入** | 通过 META 写入时 PK `(namespace, table_name)` 是最终冲突仲裁点。由于 `table_uuid` 和 `relid` 分别由 SDK 和 DDL 生成，通过 META 写入必须在 SDK+DDL 之后。并发场景下两个请求都会执行 SDK S3 写入，但仅一个通过 META 写入。孤儿 S3 文件的概率性风险可接受（详见 6.2） | ✅ |
| `drop_table` | **通过 META 读取并加锁 → DDL 模块 → 通过 META 删除 → (best-effort) 通过 SDK 清理** | FOR UPDATE 锁 + DELETE 原子性。通过 SDK 清理为 best-effort | ✅ |
| `commit_table` | **通过 META 读取并加锁 → 通过 SDK 加载表 → 通过 SDK 提交变更 → 通过 META 更新（乐观锁）** | FOR UPDATE 行锁串行化同一表的并发 commit。SDK S3 写在持有锁期间完成。通过 META 更新时乐观锁作为防御层 | ✅ |
| `add_column` | 同 `commit_table` | 同 `commit_table` | ✅ |

### 6.2 `create_table` 并发分析

`create_table` 需要 SDK 生成的 `table_uuid`/`metadata_location` 和 DDL 模块生成的 `relid`，通过 META 写入必须在两者之后。

```
时间轴 →
Request A: ──通过 SDK 创建表──┬──DDL 模块创建存储──┬──通过 META 写入表记录──┬──COMMIT──
                              │                   │      (PK仲裁)           │
Request B: ──通过 SDK 创建表──┴──DDL 模块创建存储──┴──通过 META 写入表记录──┴──PK冲突→P0005
                                                        (B已执行SDK+DDL)
```

- SDK 和 DDL 在 META 写入之前执行，并发请求可能都完成 S3 写入和本地表创建
- META 写入时的 PK `(namespace, table_name)` 是最终冲突仲裁——只有一个请求成功
- 失败的请求：事务回滚 → DDL 创建的表自动删除（openGauss DDL 是事务性的）→ SDK 写入的 S3 metadata 成为孤儿文件
- **孤儿风险可接受**：同一表的并发创建是极端低概率事件，即使发生也只有一个孤儿 metadata JSON 文件（几 KB），不影响系统正确性
- `create_namespace` 无此问题：通过 META 写入先于 SDK，并发请求在 META 层即被阻塞

### 6.3 `commit_table` / `add_column` 的行锁串行化

```
Request A: ──通过 META 读取并加锁──┬──通过 SDK 加载─┬──通过 SDK 提交变更(S3写)──┬──通过 META 更新──┬──COMMIT──
              (获取行锁)           │               │                          │                 │
Request B: ──通过 META 读取并加锁──┴── [阻塞] ─────┴── [阻塞] ────────────────┴── [阻塞] ───────┴── A COMMIT后获取锁，重新读取最新 metadata_location
```

- 通过 META 读取表元信息并加行锁，串行化同一表的并发操作
- SDK S3 写在持有锁期间完成——其他请求被阻塞，不会并发写同一表的 S3
- 通过 META 更新表记录时的乐观锁提供额外防御层

### 6.4 SDK 失败的回滚保证

| 函数 | 失败场景 | 结果 |
|------|---------|------|
| `create_namespace` | 通过 META 写入成功 → 通过 SDK 创建失败 | ✅ 事务回滚，META 写入撤销，SDK 写入失败无残留 |
| `create_table` | 通过 SDK 创建成功 → DDL 创建成功 → 通过 META 写入冲突 | ⚠️ 事务回滚，DDL 表自动删除（DDL 事务性），S3 残留孤儿 metadata JSON（低概率，可接受） |
| `create_table` | 通过 SDK 创建成功 → DDL 创建失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON |
| `commit_table` | 通过 META 加锁成功 → 通过 SDK 提交成功 → 通过 META 更新（乐观锁）失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON（行锁下极少发生） |
| 所有函数 | 通过 META 操作成功 → 通过 SDK 操作失败 | ✅ 事务回滚，META 变更撤销，SDK 写入失败无残留 |

> **孤儿文件**：仅在 SDK S3 写入成功后 META/DDL 操作失败的极端场景产生（`create_table` 并发冲突、`commit_table` 乐观锁失败等）。孤儿文件处理不在本文档范围内。

---

## 7. 错误码映射

与接口设计文档一致，SQL 函数统一使用 `P0001`~`P0009` SQLSTATE：

| SQLSTATE | HTTP | 语义 | C++ 抛出方式 |
|----------|------|------|-------------|
| P0001 | 400 | 参数无效 | `ereport(ERROR, errcode(ERRCODE_P0001), errmsg("{...}"))` |
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

