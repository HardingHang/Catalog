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

### 4.1 命名空间操作

- **存在性检查**：查询指定 namespace 是否存在（不抛错，返回 true/false）
- **读取信息**：读取 namespace 的详细信息（名称、properties），未找到时返回 NULL
- **创建**：写入 namespace 记录，已存在则返回冲突错误（P0005）
- **删除**：删除 namespace 记录，不存在则返回 P0004
- **更新 properties**：对 namespace properties 执行 removals（JSONB 数组）和 updates（JSONB object），返回 `{"updated": [...], "removed": [...], "missing": [...]}` 格式的 JSON 结果
- **分页列出**：按 parent 和 page_token 分页列出 namespace，返回 ListNamespacesResponse JSON
- **检查子表**：检查 namespace 下是否存在表（用于 drop_namespace 前置校验）

### 4.2 表操作

- **存在性检查**：查询指定表是否存在（不抛错，返回 true/false）
- **读取元信息**：读取表的完整元信息记录，未找到时返回 P0004
- **读取并加锁**：读取表元信息并加行锁（用于 commit_table / add_column / drop_table 等写操作）
- **创建记录**：写入表记录（包含 relid、table_uuid、metadata_location、table_location 等字段），冲突时返回 P0005
- **更新记录**：更新表元信息，以旧 metadata_location 为乐观锁条件（不匹配则返回 P0005），同时更新 snapshot_id、schema_id、last_column_id
- **删除记录**：删除表记录，不存在则返回 P0004
- **分页列出**：按 namespace、page_size、page_token 分页列出表，返回 ListTablesResponse JSON
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

以下为 14 个 SQL 自定义函数的实现逻辑描述。步骤中通过元信息模块接口（简称 META）和 Iceberg SDK（简称 SDK）完成实际操作。SDK/META 接口失败时返回纯文本错误描述，SQL 函数层负责将其包装为 Iceberg REST API JSON 格式后 `ereport`。

### 5.1 is_namespace_existed

```
is_namespace_existed(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. 通过 META 查询 namespace 是否存在，返回 {"exists": true} 或 {"exists": false}
```

### 5.2 is_table_existed

```
is_table_existed(p_namespace TEXT, p_table TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. p_table 为 NULL 或空串 → ereport(P0001, "table must not be empty")
3. 通过 META 查询表是否存在，返回 {"exists": true} 或 {"exists": false}
```

### 5.3 load_namespace

```
load_namespace(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. 通过 META 读取 namespace 信息
   若未找到 → ereport(P0004, "namespace not found")
3. 返回 namespace 名称和 properties
```

### 5.4 list_namespaces

```
list_namespaces(p_parent TEXT DEFAULT NULL, p_page_size INT DEFAULT 1000,
                p_page_token TEXT DEFAULT NULL) → JSONB

1. if p_page_size < 1 → ereport(P0001, "pageSize must be >= 1")
2. if p_parent 非空:
       通过 META 检查 parent namespace 是否存在，不存在 → ereport(P0004)
3. 通过 META 分页列出 namespace，解析并返回结果 JSON
```

### 5.5 list_tables

```
list_tables(p_namespace TEXT, p_page_size INT DEFAULT 1000,
            p_page_token TEXT DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001, "namespace must not be empty")
2. if p_page_size < 1 → ereport(P0001)
3. 通过 META 检查 namespace 是否存在，不存在 → ereport(P0004)
4. 通过 META 分页列出表，解析并返回结果 JSON
```

### 5.6 create_namespace

```
create_namespace(p_namespace TEXT, p_properties JSONB DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. if p_properties 非 NULL 且非合法 JSONB object → ereport(P0001)

3. // ★ 先写元信息表（利用 PK 约束仲裁并发冲突，详见 5.15）
   //   若用户指定了 location 则直接使用；否则先用临时值占位，SDK 返回后再更新
   将 p_properties 转为字符串（NULL → "{}"）
   通过 META 检查 namespace 是否已存在，已存在 → ereport(P0005)
   通过 META 写入 namespace 记录

4. // SDK 解析 S3 路径 + 创建 marker
   // 若失败 → ereport → 事务回滚 → META INSERT 自动撤销
   通过 SDK 创建 namespace（解析 S3 路径 + 创建 marker）
   检查 SDK 返回的错误消息，非 NULL 时包装为 ServiceUnavailable JSON 格式并 ereport(P0009)

5. // 若用户未指定 location，将 SDK 返回的路径更新到 properties
   if p_properties 中无 "location" key:
       通过 META 更新 namespace properties，将 SDK 返回的 location 写入

6. 通过 META 重新读取 namespace 信息，返回 namespace 名称和 properties
```

### 5.7 drop_namespace

```
drop_namespace(p_namespace TEXT) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. 通过 META 检查 namespace 是否存在，不存在 → ereport(P0004)
3. 通过 META 检查 namespace 下是否有表，有表 → ereport(P0005)

4. 通过 META 删除 namespace 记录

5. // SDK 清理 S3 marker（best-effort）
   通过 SDK 清理 namespace 对应的 S3 marker

6. return {"success": true}
```

### 5.8 update_namespace_properties

```
update_namespace_properties(p_namespace TEXT, p_removals JSONB DEFAULT NULL,
                            p_updates JSONB DEFAULT NULL) → JSONB

1. p_namespace 为 NULL 或空串 → ereport(P0001)
2. if p_removals 和 p_updates 同时为 NULL → ereport(P0001)
3. removals: 非 NULL 则必须为 JSONB 数组，否则 P0001
4. updates:  非 NULL 则必须为 JSONB object，否则 P0001
5. if removals ∩ updates ≠ ∅ → ereport(P0006)

6. 将 removals 和 updates 转为字符串（NULL → "[]" / "{}"）
7. 通过 META 更新 namespace properties，解析并返回结果 JSON
```

### 5.9 rename_table

```
rename_table(p_src_ns TEXT, p_src_table TEXT,
             p_dst_ns TEXT, p_dst_table TEXT) → JSONB

1. 任一参数为 NULL 或空串 → ereport(P0001)
2. 通过 META 检查源表是否存在，不存在 → ereport(P0004)
3. 通过 META 检查目标 namespace 是否存在，不存在 → ereport(P0004)
4. 通过 META 检查目标表是否已存在，已存在 → ereport(P0005)

5. 通过 META 重命名表记录

6. // SDK：若需同步更新 S3 路径（预留，标准 Iceberg 中为空操作）
   通过 SDK 执行 S3 路径迁移
   检查 SDK 返回的错误消息，非 NULL 时包装为 ServiceUnavailable JSON 格式并 ereport(P0009)

7. return {"success": true}
```

### 5.10 create_table

```
create_table(p_namespace TEXT, p_table_name TEXT, p_schema JSONB,
             p_location TEXT DEFAULT NULL, p_partition_spec JSONB DEFAULT NULL,
             p_write_order JSONB DEFAULT NULL, p_stage_create BOOL DEFAULT FALSE,
             p_properties JSONB DEFAULT NULL) → JSONB

1. p_namespace/p_table_name 为 NULL 或空串 → ereport(P0001)

2. // Schema 校验
   if p_schema.type ≠ "struct" → ereport(P0001)
   遍历 p_schema.fields 中的每个字段：
       通过 SDK 校验字段类型是否合法，不合法 → ereport(P0001, 错误详情)

3. // 业务检查
   通过 META 检查 namespace 是否存在，不存在 → ereport(P0004)
   通过 META 检查表是否已存在，已存在 → ereport(P0005)

4. // SDK 创建表（内部：UUID 生成 → 路径解析 → metadata 构造 → S3 写）
   将 p_schema/p_partition_spec/p_write_order/p_properties 转为字符串（NULL → NULL 指针）
   通过 SDK 创建表
   检查 SDK 返回的错误消息，非 NULL 时包装为 ServiceUnavailable JSON 格式并 ereport(P0009)

5. // DDL 管理模块创建 delta 表和 FDW 外表
   调用 DDL 管理模块创建存储（传入 namespace、表名、SDK 返回的 table UUID），获取 relid

6. // 写元信息表（PK (namespace, table_name) 仲裁并发冲突）
   通过 META 写入表记录，包含：
   - relid（DDL 模块返回）
   - table_uuid / metadata_location / table_location / last_column_id /
     current_schema_id / default_partition_spec_id（SDK 返回）
   - previous_metadata_location = NULL
   - current_snapshot_id = -1（初始无 snapshot）

7. 通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，返回结果
```

### 5.11 load_table

```
load_table(p_namespace TEXT, p_table TEXT) → JSONB

1. 任一参数为 NULL 或空串 → ereport(P0001)

2. 通过 META 读取表元信息
   若未找到 → ereport(P0004, "table not found")

3. // SDK 加载表（从 S3 读取并解析 metadata JSON，含 next-row-id）
   通过 SDK 加载表（传入 META 返回的 metadata_location）
   检查 SDK 返回的错误消息，非 NULL 时包装为 ServiceUnavailable JSON 格式并 ereport(P0009)

4. 通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，返回结果
```

### 5.12 drop_table

```
drop_table(p_namespace TEXT, p_table TEXT, p_purge BOOL DEFAULT FALSE) → JSONB

1. 任一参数为 NULL 或空串 → ereport(P0001)
2. if p_purge → ereport(P0008, "purge not yet implemented")

3. 通过 META 读取表元信息并加行锁
   若未找到 → ereport(P0004, "table not found")

4. 调用 DDL 管理模块删除存储（传入 namespace、表名、META 返回的 table_uuid）

5. 通过 META 删除表记录（META 内部处理 ON DELETE CASCADE）

6. // SDK 清理（best-effort，若 purge 支持则清理数据文件）
   通过 SDK 清理表对应的 S3 数据

7. return {"success": true}
```

### 5.13 commit_table

```
commit_table(p_namespace TEXT, p_table TEXT,
             p_requirements JSONB, p_updates JSONB) → JSONB

1. 任一参数为 NULL → ereport(P0001)
2. 校验 p_updates 中每个 element.action 为 "add-snapshot" → 否则 P0001

3. 通过 META 读取表元信息并加行锁
   若未找到 → ereport(P0004, "table not found")

4. // SDK 加载表对象
   通过 SDK 加载表（传入 META 返回的 metadata_location）
   检查 SDK 返回的错误消息，非 NULL 时包装为 ServiceUnavailable JSON 格式并 ereport(P0009)

5. // SDK 应用 requirements + updates + 写 S3（三步合一）
   // 内部：应用 requirements → 追加 snapshot → 写新 metadata JSON 到 S3
   将 p_requirements 和 p_updates 转为字符串
   通过 SDK 提交表变更，返回新的 metadata_location
   检查 SDK 返回的错误消息，非 NULL 时包装为 CommitFailedException JSON 格式并 ereport(P0005)

6. // 更新元信息表（乐观锁：WHERE metadata_location = old）
   从 p_updates 中提取新 snapshot ID
   通过 META 更新表记录（传入旧 metadata_location 作为乐观锁）

7. 通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，返回结果
```

### 5.14 add_column

```
add_column(p_namespace TEXT, p_table TEXT,
           p_column_name TEXT, p_column_type TEXT,
           p_column_doc TEXT DEFAULT NULL) → JSONB

1. p_namespace/p_table/p_column_name/p_column_type 任一为 NULL 或空串 → ereport(P0001)
2. 通过 SDK 校验列类型是否合法，不合法 → ereport(P0001, 错误详情)

3. 通过 META 读取表元信息并加行锁
   若未找到 → ereport(P0004, "table not found")

4. // SDK 加载表对象
   通过 SDK 加载表（传入 META 返回的 metadata_location）
   检查 SDK 返回的错误消息，非 NULL 时包装为 ServiceUnavailable JSON 格式并 ereport(P0009)

5. // 检查列名冲突
   通过 SDK 获取当前 schema，若已存在同名字段 → ereport(P0001, "column already exists")

6. // SDK 扩展 Schema
   通过 SDK 追加列（传入列名、类型、文档），获取新 field ID 和新 schema
   新 schema ID = 当前 schema ID + 1

7. // 自动构造 requirements 和 updates
   requirements = [
       {"type":"assert-table-uuid",            "uuid": <META 返回的 table_uuid>},
       {"type":"assert-ref-snapshot-id",       "ref":"main",
                                                "snapshot-id": <META 返回的 current_snapshot_id>},
       {"type":"assert-current-schema-id",     "current-schema-id": <META 返回的 current_schema_id>},
       {"type":"assert-last-assigned-field-id","last-assigned-field-id": <META 返回的 last_column_id>}
   ]
   updates = [
       {"action":"add-schema",         "schema": <SDK 返回的新 schema JSON>,
                                        "last-column-id": <SDK 返回的新 field ID>},
       {"action":"set-current-schema", "schema-id": <新 schema ID>}
   ]

8. // SDK 应用变更 + 写 S3
   将 requirements 和 updates 转为字符串
   通过 SDK 提交表变更，返回新的 metadata_location
   检查 SDK 返回的错误消息，非 NULL 时包装为 CommitFailedException JSON 格式并 ereport(P0005)

9. // 更新元信息表（乐观锁：WHERE metadata_location = old）
   通过 META 更新表记录（传入旧 metadata_location 作为乐观锁）
   更新字段：current_schema_id = 新 schema ID，last_column_id = 新 field ID
   （snapshot 不变）

10. 通过 SDK 获取完整 metadata JSON，释放 SDK 表对象，返回结果
```

### 5.15 并发调用分析

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

#### 5.15.1 各函数并发分析

| 函数 | 写顺序 | 并发机制 | 安全？ |
|------|--------|---------|--------|
| `create_namespace` | **通过 META 写入 → 通过 SDK 创建 namespace** | 通过 META 写入时 PK `(namespace)` 约束仲裁冲突。后到达的请求阻塞等待→前一个 COMMIT 后→PK 冲突→P0005，未调用 SDK | ✅ |
| `drop_namespace` | **通过 META 删除 → 通过 SDK 清理 namespace** | 通过 META 删除在事务中。通过 SDK 清理为 best-effort | ✅ |
| `update_namespace_properties` | 纯 META（行锁 → 更新） | 行锁串行化。不涉及 SDK | ✅ |
| `rename_table` | **通过 META 更新 → (可选) 通过 SDK 迁移 S3 路径** | 通过 META 的 PK 约束仲裁目标表名冲突。通过 SDK 迁移为可选预留 | ✅ |
| `create_table` | 通过 SDK 创建表 → DDL 模块创建存储 → **通过 META 写入** | 通过 META 写入时 PK `(namespace, table_name)` 是最终冲突仲裁点。由于 `table_uuid` 和 `relid` 分别由 SDK 和 DDL 生成，通过 META 写入必须在 SDK+DDL 之后。并发场景下两个请求都会执行 SDK S3 写入，但仅一个通过 META 写入。孤儿 S3 文件的概率性风险可接受（详见 5.15.2） | ✅ |
| `drop_table` | **通过 META 读取并加锁 → DDL 模块 → 通过 META 删除 → (best-effort) 通过 SDK 清理** | FOR UPDATE 锁 + DELETE 原子性。通过 SDK 清理为 best-effort | ✅ |
| `commit_table` | **通过 META 读取并加锁 → 通过 SDK 加载表 → 通过 SDK 提交变更 → 通过 META 更新（乐观锁）** | FOR UPDATE 行锁串行化同一表的并发 commit。SDK S3 写在持有锁期间完成。通过 META 更新时乐观锁作为防御层 | ✅ |
| `add_column` | 同 `commit_table` | 同 `commit_table` | ✅ |

#### 5.15.2 `create_table` 并发分析

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

#### 5.15.3 `commit_table` / `add_column` 的行锁串行化

```
Request A: ──通过 META 读取并加锁──┬──通过 SDK 加载─┬──通过 SDK 提交变更(S3写)──┬──通过 META 更新──┬──COMMIT──
              (获取行锁)           │               │                          │                 │
Request B: ──通过 META 读取并加锁──┴── [阻塞] ─────┴── [阻塞] ────────────────┴── [阻塞] ───────┴── A COMMIT后获取锁，重新读取最新 metadata_location
```

- 通过 META 读取表元信息并加行锁，串行化同一表的并发操作
- SDK S3 写在持有锁期间完成——其他请求被阻塞，不会并发写同一表的 S3
- 通过 META 更新表记录时的乐观锁提供额外防御层

#### 5.15.4 SDK 失败的回滚保证

| 函数 | 失败场景 | 结果 |
|------|---------|------|
| `create_namespace` | 通过 META 写入成功 → 通过 SDK 创建失败 | ✅ 事务回滚，META 写入撤销，SDK 写入失败无残留 |
| `create_table` | 通过 SDK 创建成功 → DDL 创建成功 → 通过 META 写入冲突 | ⚠️ 事务回滚，DDL 表自动删除（DDL 事务性），S3 残留孤儿 metadata JSON（低概率，可接受） |
| `create_table` | 通过 SDK 创建成功 → DDL 创建失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON |
| `commit_table` | 通过 META 加锁成功 → 通过 SDK 提交成功 → 通过 META 更新（乐观锁）失败 | ⚠️ 事务回滚，S3 残留孤儿 metadata JSON（行锁下极少发生） |
| 所有函数 | 通过 META 操作成功 → 通过 SDK 操作失败 | ✅ 事务回滚，META 变更撤销，SDK 写入失败无残留 |

> **孤儿文件**：仅在 SDK S3 写入成功后 META/DDL 操作失败的极端场景产生（`create_table` 并发冲突、`commit_table` 乐观锁失败等）。孤儿文件处理不在本文档范围内。

---

## 6. 错误码映射

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

