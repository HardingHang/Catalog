# Iceberg Metadata SQL Function Design

## 1. 文档说明

### 1. 目标

OpenGauss将Iceberg REST API包装成自定义SQL函数对外提供操作Iceberg元数据的能力，此设计完成SQL函数定义，设计目标如下：
1. 遵循Iceberg REST API规范。
2. 函数命名风格一致。
3. 向后兼容可扩展。
4. 异常处理契约化。
5. 数据类型安全。

## 2. 设计约束

### 2.1 不支持多级namespace

跨领域的资源对象映射关系如下：
| Iceberg | OpenGauss |
|--|--|
| Catalog | Database |
| Namespace | Schema |
| Table | Table |

由于Schema不支持级联创建，所以系统仅支持创建一级Namespace。

### 2.2 不支持定长数据类型

跨领域的数据类型映射关系如下：
| Iceberg | OpenGauss |
|--|--|
| int | INTEGER |
| long | BIGINT |
| boolean | BOOLEAN |
| float | FLOAT4 |
| double | FLOAT8 |
| string | TEXT/VARCHAR |
| binary | BYTEA |
| uuid | UUID |
| fixed(L) | UnSupported |
| date | DATE |
| time | TIME |
| timestamp | TIMESTAMP |
| decimal(p, s) | DEC[(p[, s])] |

其中不支持fixed(L)定长数据类型，涉及不支持的数据类型时SQL函数会校验并返回错误信息。

## 3. 设计原则

### 3.1 参数显示化

所有影响行为的外部信息必须通过参数传递，不依赖会话变量或隐藏配置。

### 3.2 返回混合格式数据

一方面从单一职责考虑，系统函数属于数据层不属于表示层，不应该对数据格式负责，虽然实现的是Iceberg REST API语义，如果按JSON、XML等特定协议格式返回数据，系统函数将失去通用性与可复用性；另一方面metadata.json字段数量非常多，嵌套层级庞大，很难以结构化关系数据完整地描述语义，因此做折中设计：
  - 对于如list_tables、list_namespaces等列表接口，系统函数以Table格式返回。
  - 对于如load_table、load_namespace等单对象接口，系统函数以JSONB格式返回。

### 3.3 错误抛出SQLSTATE

Iceberg REST API错误响应格式标准：
```json
{
  "error": {
    "code": 404,
    "message": "请求失败的人类可读描述",
    "type": "NoSuchNamespaceException",
    "stack": ["可选的堆栈信息帧列表"]
  }
}
```

message、type、stack信息通过Message承载，异常信息必须保持与Iceberg REST API一致且完整;

code是标准http错误码，系统函数抛出SQLSTATE做映射:
| SQLSTATE | 含义 | 映射 HTTP 状态 |
|--------|--------|-----------|
| P0001 | 无效参数 | 400 |
| P0002 | 未认证 | 401 |
| P0003 | 无权限 | 403 |
| P0004 | 资源不存在 | 404 |
| P0005 | 资源已存在 | 409 |
| P0008 | 服务端不支持 | 503 |
| P0009 | 内部服务器错误 | 500 |

## 4. API范围

实现以下13个系统函数，覆盖 Iceberg REST Catalog 的 Namespace 和 Table 管理能力：

| 函数名 | 对应 REST API | HTTP 方法 | 功能 |
|--------|--------------|-----------|------|
| `create_namespace` | `/v1/{prefix}/namespaces` | POST | 创建命名空间 |
| `list_namespaces` | `/v1/{prefix}/namespaces` | GET | 分页列出命名空间 |
| `load_namespace` | `/v1/{prefix}/namespaces/{namespace}` | GET | 加载命名空间元数据 |
| `drop_namespace` | `/v1/{prefix}/namespaces/{namespace}` | DELETE | 删除命名空间（须为空） |
| `is_namespace_existed` | `/v1/{prefix}/namespaces/{namespace}` | HEAD | 检查命名空间是否存在 |
| `create_table` | `/v1/{prefix}/namespaces/{namespace}/tables` | POST | 创建表 |
| `load_table` | `/v1/{prefix}/namespaces/{namespace}/tables/{table}` | GET | 加载表元数据 |
| `list_tables` | `/v1/{prefix}/namespaces/{namespace}/tables` | GET | 分页列出命名空间下的表 |
| `drop_table` | `/v1/{prefix}/namespaces/{namespace}/tables/{table}` | DELETE | 删除表 |
| `commit_table` | `/v1/{prefix}/namespaces/{namespace}/tables/{table}` | POST | 提交表变更（乐观锁） |
| `add_column` | 便捷函数（封装 `commit_table`） | — | 为表添加列 |
| `rename_table` | `/v1/{prefix}/tables/rename` | POST | 重命名表（支持跨命名空间） |
| `is_table_existed` | `/v1/{prefix}/namespaces/{namespace}/tables/{table}` | HEAD | 检查表是否存在 |

## 5. 异常信息传递

SQL函数统一使用以下 `P0001`~`P0009` 自定义 SQLSTATE，对齐 HTTP 状态码：

| SQLSTATE | HTTP 状态码 | 语义 |
|----------|---------|------|
| `P0001` | 400 | 请求参数无效、格式错误或校验失败 |
| `P0002` | 401 | 未认证 |
| `P0003` | 403 | 无权限 |
| `P0004` | 404 | 指定的资源不存在 |
| `P0005` | 409 | 资源已存在 |
| `P0008` | 503 | 服务端不支持 |
| `P0009` | 500 | 服务端内部错误 |

所有 RAISE 异常的 MESSAGE 格式遵循 Iceberg REST API 错误响应格式：

```json
{
  "type": "<IcebergExceptionType>",
  "message": "<人类可读描述>",
  "stack": ["<可选堆栈帧>"]
}
```

## 6. SQL函数定义

### 6.0 create_table

````sql
CREATE OR REPLACE FUNCTION create_table(
    p_namespace    TEXT,
    p_table_name   TEXT,
    p_schema       JSONB,
    p_location     TEXT    DEFAULT NULL,
    p_partition_spec JSONB DEFAULT NULL,
    p_write_order  JSONB   DEFAULT NULL,
    p_stage_create BOOLEAN DEFAULT FALSE,
    p_properties   JSONB   DEFAULT NULL
) RETURNS JSONB
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION create_table(TEXT, TEXT, JSONB, TEXT, JSONB, JSONB, BOOLEAN, JSONB) IS
$$
## create_table — 在指定 Namespace 下创建 Iceberg 表

对应 REST API：`POST /v1/{prefix}/namespaces/{namespace}/tables`

### 功能描述

在目标 Namespace 中创建一张 Iceberg 表。支持直接创建（`p_stage_create = FALSE`）和暂存创建（`p_stage_create = TRUE`，返回初始化元数据但不真正创建，需后续调用 `commit_table` 完成事务）。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | 目标 Namespace 标识符（单段，不可为空字符串） |
| `p_table_name` | `TEXT` | 是 | — | 表名称（不可为空字符串） |
| `p_schema` | `JSONB` | 是 | — | Iceberg Schema 定义（JSON 对象，含 `type`=`"struct"` 和 `fields` 数组） |
| `p_location` | `TEXT` | 否 | `NULL` | 表数据存储位置，为 `NULL` 时由服务端自动分配 |
| `p_partition_spec` | `JSONB` | 否 | `NULL` | 分区规范，为 `NULL` 表示不分区 |
| `p_write_order` | `JSONB` | 否 | `NULL` | 写入排序规则，为 `NULL` 表示无排序 |
| `p_stage_create` | `BOOLEAN` | 否 | `FALSE` | `TRUE` 表示暂存创建（启动创建事务），`FALSE` 表示直接创建 |
| `p_properties` | `JSONB` | 否 | `NULL` | 表级属性键值对，为 `NULL` 等价于空对象 `{}` |

### 返回值

类型：`JSONB`，对齐 Iceberg REST API `LoadTableResult`：

​```json
{
  "metadata-location": "s3://bucket/warehouse/accounting/tax/metadata/v1.metadata.json",
  "metadata": {
    "format-version": 2,
    "table-uuid": "d20125be-4c48-4444-97a6-7a2d6e28d7fb",
    "location": "s3://bucket/warehouse/accounting/tax",
    "last-updated-ms": 1602638573590,
    "current-schema-id": 0,
    "schemas": [
      {
        "type": "struct",
        "schema-id": 0,
        "fields": [
          { "id": 1, "name": "id", "type": "long", "required": true },
          { "id": 2, "name": "data", "type": "string", "required": false }
        ]
      }
    ],
    "partition-specs": [],
    "default-spec-id": 0,
    "sort-orders": [],
    "default-sort-order-id": 0,
    "snapshots": [],
    "refs": {},
    "current-snapshot-id": -1,
    "last-sequence-number": 0,
    "snapshot-log": [],
    "metadata-log": []
  },
  "config": {}
}
```
### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 或 `p_table_name` 为 `NULL`/空字符串、`p_schema` 格式错误 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 `p_namespace` 不存在 |
| `P0005` | 409 | `AlreadyExistsException` | 同 Namespace 下已存在同名 Table |
| `P0009` | 500 | `CommitStateUnknownException` | 服务端内部错误，创建状态未知 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "NoSuchNamespaceException", "message": "The given namespace does not exist", "stack": []}
```

RAISE 示例：

```sql
-- Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';

-- 表已存在
RAISE EXCEPTION '{"type":"AlreadyExistsException","message":"The requested table identifier already exists","stack":[]}'
    USING ERRCODE = 'P0005';

-- 参数校验失败
RAISE EXCEPTION '{"type":"BadRequestException","message":"p_namespace must not be NULL or empty","stack":[]}'
    USING ERRCODE = 'P0001';
```

### 注意事项

1. **一级 Namespace 限制**：由于 OpenGauss Schema 不支持级联创建，`p_namespace` 仅允许单段标识符（如 `"accounting"`），不支持多段（如 `"accounting.tax"`）。
2. **不支持 `fixed(L)` 类型**：如果 `p_schema` 中包含 `fixed(L)` 类型字段，参数校验阶段将抛出 `P0001`。
3. **暂存创建（Stage Create）**：当 `p_stage_create = TRUE` 时，返回的元数据中 `metadata-location` 为 `null`，需调用 `commit_table` 完成事务提交。
4. **分区与排序**：`p_partition_spec` 和 `p_write_order` 若传入，必须符合 Iceberg 规范格式；传入后即与表绑定，后续可通过 `commit_table` 变更。
$$;
````
---

### 6.1 create_namespace

````sql
CREATE OR REPLACE FUNCTION create_namespace(
    p_namespace  TEXT,
    p_properties JSONB DEFAULT NULL
) RETURNS JSONB
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION create_namespace(TEXT, JSONB) IS
$$
## create_namespace — 创建 Namespace
```
对应 REST API：`POST /v1/{prefix}/namespaces`

#### 功能描述

创建一个 Namespace，可选附带一组属性键值对。服务端可能会自动附加属性（如 `last_modified_time` 等）。实现方可以不支持 Namespace 属性。

#### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（单段，不可为空字符串） |
| `p_properties` | `JSONB` | 否 | `NULL` | Namespace 属性键值对，为 `NULL` 等价于空对象 `{}` |

#### 返回值

类型：`JSONB`，对齐 Iceberg REST API `CreateNamespaceResponse`：

```json
{
  "namespace": ["accounting"],
  "properties": { "owner": "Ralph", "created_at": "1452120468" }
}
```

#### 异常处理

| SQLSTATE | HTTP | 说明 |
|----------|------|------|
| `P0001` | 400 | `p_namespace` 为 `NULL` 或空字符串，或 `p_properties` 格式非法 |
| `P0005` | 409 | 指定的 Namespace 已存在 |
| `P0008` | 503 | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "AlreadyExistsException", "message": "The given namespace already exists", "stack": []}
```

RAISE 示例：

```sql
-- Namespace 已存在
RAISE EXCEPTION '{"type":"AlreadyExistsException","message":"The given namespace already exists","stack":[]}'
    USING ERRCODE = 'P0005';

-- 参数校验失败
RAISE EXCEPTION '{"type":"BadRequestException","message":"p_namespace must not be NULL or empty","stack":[]}'
    USING ERRCODE = 'P0001';
```

#### 注意事项

1. **一级 Namespace 限制**：仅支持单段标识符（如 `"accounting"`），不支持多段 Namespace。
2. **属性可选**：如果服务端实现不支持 Namespace 属性，`p_properties` 传入的值可能被忽略，但不应报错。
````
---

### 6.2 list_namespaces

````sql
CREATE OR REPLACE FUNCTION list_namespaces(
    p_parent     TEXT    DEFAULT NULL,
    p_page_size  INTEGER DEFAULT 1000,
    p_page_token INTEGER DEFAULT 0
) RETURNS JSONB
LANGUAGE plpgsql STABLE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION list_namespaces(TEXT, INTEGER, INTEGER) IS
$$

对应 REST API：`GET /v1/{prefix}/namespaces?parent=&pageToken=&pageSize=`

#### 功能描述

列出指定父级 Namespace 下的所有一级子 Namespace，支持分页。若 `p_parent` 为 `NULL`，则列出所有顶层 Namespace。

#### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_parent` | `TEXT` | 否 | `NULL` | 父级 Namespace 标识符；为 `NULL` 或空字符串时返回顶层 Namespace 列表。 |
| `p_page_size` | `INTEGER` | 否 | `1000` | 每页返回的最大结果数（最小值 1） |
| `p_page_token` | `INTEGER` | 否 | `0` | 当前分页的偏移量 |

#### 返回值

类型：`TABLE(namespace TEXT, next_page_token INTEGER)`，对齐 Iceberg REST API `ListNamespacesResponse`：

| 列名 | 类型 | 语义 |
|------|------|------|
| `namespace` | `TEXT` | 子 Namespace 标识符 |
| `next_page_token` | `INTEGER` | 下一页分页令牌；每行值相同，读第一行即可决定是否翻页；为 `NULL` 时表示已到最后一页 |

返回值示例：

```
  namespace   | next_page_token
--------------+-----------------
 accounting   | 10
 tax          | 10
 hr           | 10
(3 rows)
```

最后一页示例：

```
  namespace   | next_page_token
--------------+-----------------
 audit        | NULL
(1 row)
```

#### 异常处理

| SQLSTATE | HTTP | 说明 |
|----------|------|------|
| `P0001` | 400 | `p_page_size` 小于 1，或 `p_page_token` 格式无效 |
| `P0004` | 404 | `p_parent` 指定的父级 Namespace 不存在 |
| `P0008` | 503 | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "NoSuchNamespaceException", "message": "The given namespace does not exist", "stack": []}
```

RAISE 示例：

```sql
-- 父级 Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';

-- page_size 无效
RAISE EXCEPTION '{"type":"BadRequestException","message":"p_page_size must be >= 1","stack":[]}'
    USING ERRCODE = 'P0001';
```

#### 注意事项

1. **查询类函数标记 STABLE**：此函数为只读查询，允许优化器做语句级缓存。
2. **分页语义**：`p_page_token` 为 `0`或NULL 时返回第一页；后续使用返回的 `next_page_token` 继续翻页；最后一页的 `next_page_token` 为 `NULL`。
3. **一级 Namespace 限制**：`p_parent` 仅支持单段值或 `NULL`；若传入多段值（如 `"accounting.tax"`），将抛出 `P0001`。
4. **无分页支持的实现**：若服务端不支持分页，可忽略 `p_page_size` 和 `p_page_token` 参数，一次性返回全部结果，此时 `next_page_token` 始终为 `NULL`。
$$;
````
---

### 6.3 load_namespace

````sql
CREATE OR REPLACE FUNCTION load_namespace(
    p_namespace TEXT
) RETURNS JSONB
LANGUAGE plpgsql STABLE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION load_namespace(TEXT) IS
$$
```
对应 REST API：`GET /v1/{prefix}/namespaces/{namespace}`

#### 功能描述

返回指定 Namespace 的所有已存储元数据属性。

#### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |

#### 返回值

类型：`JSONB`，对齐 Iceberg REST API `GetNamespaceResponse`：

```json
{
  "namespace": ["accounting"],
  "properties": {
    "owner": "Ralph",
    "created_at": "1452120468"
  }
}
```

> **注意**：若服务端不支持 Namespace 属性，`properties` 字段为 `null`；若支持但未设置任何属性，`properties` 为空对象 `{}`。

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 为 `NULL` 或空字符串 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "NoSuchNamespaceException", "message": "The given namespace does not exist", "stack": []}
```

RAISE 示例：

```sql
-- Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';
```

### 注意事项

1. **查询类函数标记 STABLE**：此函数为只读查询，允许优化器做语句级缓存。
2. **一级 Namespace 限制**：仅支持单段标识符。
$$;
````
---

### 6.4 drop_namespace

````sql
CREATE OR REPLACE FUNCTION drop_namespace(
    p_namespace TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION drop_namespace(TEXT) IS
$$
```

对应 REST API：`DELETE /v1/{prefix}/namespaces/{namespace}`

#### 功能描述

从 Catalog 中删除指定 Namespace。**Namespace 必须为空**（即其下不存在任何 Table），否则操作失败。

#### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |

#### 返回值

类型：`BOOLEAN`。成功删除返回 `TRUE`。

#### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 为 `NULL` 或空字符串 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0005` | 409 | `NamespaceNotEmptyException` | Namespace 非空，其下仍有 Table |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "NamespaceNotEmptyException", "message": "The given namespace is not empty", "stack": []}
```

RAISE 示例：

```sql
-- Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';

-- Namespace 非空
RAISE EXCEPTION '{"type":"NamespaceNotEmptyException","message":"The given namespace is not empty","stack":[]}'
    USING ERRCODE = 'P0005';
```

### 注意事项

1. **必须为空**：删除前确保 Namespace 下无任何 Table。若需强制删除，请先逐表删除。
3. **不可逆**：删除操作不可撤销；一旦成功，Namespace 及其属性将被永久移除。
$$;
````
---

### 6.5 is_namespace_existed

````sql
CREATE OR REPLACE FUNCTION is_namespace_existed(
    p_namespace TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql STABLE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION is_namespace_existed(TEXT) IS
$$
```
对应 REST API：`HEAD /v1/{prefix}/namespaces/{namespace}`

#### 功能描述

检查指定 Namespace 是否存在于 Catalog 中。不返回响应体内容。

#### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |

#### 返回值

类型：`BOOLEAN`。
- `TRUE`：Namespace 存在。
- `FALSE`：Namespace 不存在（不抛异常，直接返回 `FALSE`）。

#### 异常处理

| SQLSTATE | HTTP | 说明 |
|----------|------|------|
| `P0001` | 400 | `p_namespace` 为 `NULL` 或空字符串 |
| `P0008` | 503 | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "BadRequestException", "message": "p_namespace must not be NULL or empty", "stack": []}
```

RAISE 示例：

```sql
-- 参数校验失败
RAISE EXCEPTION '{"type":"BadRequestException","message":"p_namespace must not be NULL or empty","stack":[]}'
    USING ERRCODE = 'P0001';
```

### 注意事项

1. **查询类函数标记 STABLE**：此函数为只读查询。
2. **不抛 NoSuchNamespaceException**：此函数设计为"检查"语义，不存在时返回 `FALSE` 而非抛异常；仅在参数错误或权限问题时才抛异常。
3. **轻量操作**：对应 HTTP `HEAD` 方法，无需传输响应体；实现应使用低成本的存在性检查（如仅查元数据缓存）。
````
---

### 6.6 load_table

````sql
CREATE OR REPLACE FUNCTION load_table(
    p_namespace TEXT,
    p_table     TEXT
) RETURNS JSONB
LANGUAGE plpgsql STABLE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION load_table(TEXT, TEXT) IS
$$
## load_table — 加载 Table 元数据

对应 REST API：`GET /v1/{prefix}/namespaces/{namespace}/tables/{table}`

### 功能描述

从 Catalog 加载指定 Table 的完整元数据，包括表结构、分区规范、排序规则、快照历史、配置信息及存储凭证等。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |
| `p_table` | `TEXT` | 是 | — | Table 名称（不可为空字符串） |

### 返回值

类型：`JSONB`，对齐 Iceberg REST API `LoadTableResult`：

​```json
{
  "metadata-location": "s3://bucket/warehouse/accounting/tax/metadata/v10.metadata.json",
  "metadata": {
    "format-version": 2,
    "table-uuid": "d20125be-4c48-4444-97a6-7a2d6e28d7fb",
    "location": "s3://bucket/warehouse/accounting/tax",
    "last-updated-ms": 1602638573590,
    "current-schema-id": 1,
    "schemas": [{ "type": "struct", "schema-id": 1, "fields": ["..."] }],
    "partition-specs": [{ "spec-id": 0, "fields": ["..."] }],
    "default-spec-id": 0,
    "sort-orders": [],
    "default-sort-order-id": 0,
    "snapshots": [{ "snapshot-id": 3051729675574597000, "timestamp-ms": 1602638573590, "manifest-list": "...", "summary": { "operation": "append" } }],
    "refs": { "main": { "type": "branch", "snapshot-id": 3051729675574597000 } },
    "current-snapshot-id": 3051729675574597000,
    "last-sequence-number": 1,
    "snapshot-log": [],
    "metadata-log": []
  },
  "config": {
    "token": "bearer-token-value"
  }
}
```

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 或 `p_table` 为 `NULL` 或空字符串 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0004` | 404 | `NoSuchTableException` | 指定的 Table 不存在 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "NoSuchTableException", "message": "The given table does not exist", "stack": []}
```

RAISE 示例：

```sql
-- Table 不存在
RAISE EXCEPTION '{"type":"NoSuchTableException","message":"The given table does not exist","stack":[]}'
    USING ERRCODE = 'P0004';

-- Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';
```

### 注意事项

1. **查询类函数标记 STABLE**：此函数为只读查询。
2. **元数据与配置分离**：`metadata` 字段包含 Iceberg 表元数据；`config` 字段包含表级配置覆盖（如 FileIO 实现、凭证令牌等），客户端应使用 config 中的信息覆盖 Catalog 默认配置。
3. **ETag 支持**：服务端可通过 `ETag` 头返回元数据版本标识，客户端可用于后续条件查询（`If-None-Match`）。此函数暂不暴露 ETag 作为参数，后续可按需扩展。
$$;
````
---

### 6.7 list_tables

````sql
CREATE OR REPLACE FUNCTION list_tables(
    p_namespace  TEXT,
    p_page_size  INTEGER DEFAULT 1000,
    p_page_token TEXT    DEFAULT NULL
) RETURNS TABLE(namespace TEXT, table_name TEXT, next_page_token TEXT)
LANGUAGE plpgsql STABLE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION list_tables(TEXT, INTEGER, TEXT) IS
$$
## list_tables — 分页列出 Namespace 下的 Table

对应 REST API：`GET /v1/{prefix}/namespaces/{namespace}/tables?pageToken=&pageSize=`

### 功能描述

返回指定 Namespace 下的所有 Table 标识符列表，支持分页。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |
| `p_page_size` | `INTEGER` | 否 | `1000` | 每页返回的最大结果数（最小值 1） |
| `p_page_token` | `TEXT` | 否 | `NULL` | 分页游标令牌；首页调用传 `NULL`，后续传上一页返回的 `next_page_token` |

### 返回值

类型：`TABLE(namespace TEXT, table_name TEXT, next_page_token TEXT)`，对齐 Iceberg REST API `ListTablesResponse`：

| 列名 | 类型 | 语义 |
|------|------|------|
| `namespace` | `TEXT` | Table 所属 Namespace（JSON 数组形式的字符串表示） |
| `table_name` | `TEXT` | Table 名称 |
| `next_page_token` | `TEXT` | 下一页分页令牌；为 `NULL` 时表示已到最后一页 |

返回值示例：

```
    namespace     | table_name | next_page_token
------------------+------------+-----------------
 ["accounting"]   | paid       | eyJvZmZzZXQ...
 ["accounting"]   | owed       | eyJvZmZzZXQ...
 ["accounting"]   | ledger     | eyJvZmZzZXQ...
(3 rows)
```

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 为 `NULL` 或空字符串，或 `p_page_size` 小于 1 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

​```json
{"type": "NoSuchNamespaceException", "message": "The given namespace does not exist", "stack": []}
```

RAISE 示例：

```sql
-- Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';
```

### 注意事项

1. **查询类函数标记 STABLE**：此函数为只读查询。
2. **分页语义**：同 `list_namespaces`，`p_page_token` 为 `NULL` 时返回第一页；最后一页的 `next_page_token` 为 `NULL`。
3. **无分页支持的实现**：若不支持分页，可忽略分页参数并一次返回全部结果，此时 `next_page_token` 始终为 `NULL`。
$$;
````
---

### 6.8 drop_table

````sql
CREATE OR REPLACE FUNCTION drop_table(
    p_namespace TEXT,
    p_table     TEXT,
    p_purge     BOOLEAN DEFAULT FALSE
) RETURNS BOOLEAN
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION drop_table(TEXT, TEXT, BOOLEAN) IS
$$
## drop_table — 删除 Table

对应 REST API：`DELETE /v1/{prefix}/namespaces/{namespace}/tables/{table}?purgeRequested=`

### 功能描述

从 Catalog 中删除指定 Table。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |
| `p_table` | `TEXT` | 是 | — | Table 名称（不可为空字符串） |
| `p_purge` | `BOOLEAN` | 否 | `FALSE` | `TRUE` 表示同时清理底层数据和元数据文件；`FALSE` 表示仅移除 Catalog 注册 |

### 返回值

类型：`BOOLEAN`。成功删除返回 `TRUE`。

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 或 `p_table` 为 `NULL` 或空字符串 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0004` | 404 | `NoSuchTableException` | 指定的 Table 不存在 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

​```json
{"type": "NoSuchTableException", "message": "The given table does not exist", "stack": []}
```

RAISE 示例：

```sql
-- Table 不存在
RAISE EXCEPTION '{"type":"NoSuchTableException","message":"The given table does not exist","stack":[]}'
    USING ERRCODE = 'P0004';
```

### 注意事项

1. **Purge 语义**：`p_purge = TRUE` 时，底层数据文件（data files）和元数据文件（metadata files）也将被物理删除，此操作不可逆。
3. **与 `unregisterTable` 的区别**：此函数对应 `DELETE` 端点；若需要仅取消注册而不删除数据文件，请使用 `POST /.../unregister` 端点（本阶段暂未包装为系统函数）。
$$;
````
---

### 6.9 commit_table

````sql
CREATE OR REPLACE FUNCTION commit_table(
    p_namespace TEXT,
    p_table     TEXT,
    p_updates   JSONB
) RETURNS JSONB
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION commit_table(TEXT, TEXT, JSONB) IS
$$
## commit_table — 提交 Table 变更

对应 REST API：`POST /v1/{prefix}/namespaces/{namespace}/tables/{table}` (`updateTable`)

### 功能描述

向指定 Table 提交元数据变更。当前首期仅支持 `add-snapshot` 操作，用于将新生成的快照注册到 Table 元数据中。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Table 所属 Namespace 标识符（不可为空字符串） |
| `p_table` | `TEXT` | 是 | — | Table 名称（不可为空字符串） |
| `p_updates` | `JSONB` | 是 | — | Updates 数组，当前仅支持 `add-snapshot` 操作（不可为空数组） |

#### p_updates 结构

`p_updates` 是一个 JSON 数组，首期仅支持 `add-snapshot` action：

```json
[
  {
    "action": "add-snapshot",
    "snapshot": {
      "snapshot-id": 3051729675574597000,
      "timestamp-ms": 1602638573590,
      "manifest-list": "s3://bucket/warehouse/accounting/tax/metadata/snap-xxx.avro",
      "summary": { "operation": "append" },
      "schema-id": 0
    }
  }
]
```

`snapshot` 对象字段说明：
- `snapshot-id`（必填）：快照唯一标识，64 位整数
- `timestamp-ms`（必填）：快照创建时间戳（毫秒）
- `manifest-list`（必填）：快照 manifest list 文件位置
- `summary`（必填）：快照摘要，`operation` 为必填字段（枚举值：`append`、`replace`、`overwrite`、`delete`）
- `parent-snapshot-id`（可选）：父快照 ID
- `sequence-number`（可选）：序列号

### 返回值

类型：`JSONB`，对齐 Iceberg REST API `CommitTableResponse`：

```json
{
  "metadata-location": "s3://bucket/warehouse/accounting/tax/metadata/v11.metadata.json",
  "metadata": {
    "format-version": 2,
    "table-uuid": "d20125be-4c48-4444-97a6-7a2d6e28d7fb",
    "location": "s3://bucket/warehouse/accounting/tax",
    "last-updated-ms": 1602638574000,
    "current-schema-id": 1,
    "schemas": ["..."],
    "partition-specs": ["..."],
    "default-spec-id": 0,
    "sort-orders": ["..."],
    "default-sort-order-id": 0,
    "snapshots": ["..."],
    "refs": {},
    "current-snapshot-id": 3051729675574598000,
    "last-sequence-number": 2,
    "snapshot-log": ["..."],
    "metadata-log": ["..."]
  }
}
```

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | 参数为 `NULL`/空，或 `p_updates` 包含非 `add-snapshot` 的 action 或格式错误 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0004` | 404 | `NoSuchTableException` | 指定的 Table 不存在 |
| `P0005` | 409 | `CommitFailedException` | 提交冲突，客户端可重试 |
| `P0009` | 500 | `CommitStateUnknownException` | 服务端内部错误，提交状态未知 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

```json
{"type": "CommitFailedException", "message": "Commit conflict: the table has been modified concurrently, retry with updated metadata", "stack": []}
```

RAISE 示例：

```sql
-- Table 不存在
RAISE EXCEPTION '{"type":"NoSuchTableException","message":"The given table does not exist","stack":[]}'
    USING ERRCODE = 'P0004';

-- 提交冲突
RAISE EXCEPTION '{"type":"CommitFailedException","message":"Commit conflict: the table has been modified concurrently","stack":[]}'
    USING ERRCODE = 'P0005';

-- 提交状态未知
RAISE EXCEPTION '{"type":"CommitStateUnknownException","message":"Internal server error, commit state is unknown","stack":[]}'
    USING ERRCODE = 'P0009';

-- 不支持的 update action
RAISE EXCEPTION '{"type":"BadRequestException","message":"Unsupported update action: add-schema, only add-snapshot is supported","stack":[]}'
    USING ERRCODE = 'P0001';
```

### 注意事项

1. **首期仅支持 add-snapshot**：`p_updates` 数组中的每个元素必须是 `{"action": "add-snapshot", "snapshot": {...}}`，传入其他 action（如 `add-schema`、`set-properties` 等）将抛出 `P0001`。
2. **原子性**：同一请求中的多个 `add-snapshot` updates 在同一事务中原子执行；要么全部成功，要么全部失败。
3. **未知 action 严格校验**：服务端必须拒绝任何未知的 update action，返回 `P0001`。
4. **后续扩展**：Requirements 及其他 update action 计划在后续版本中支持。
$$;
````
---

### 6.10 add_column

````sql
CREATE OR REPLACE FUNCTION add_column(
    p_namespace    TEXT,
    p_table        TEXT,
    p_column_name  TEXT,
    p_column_type  TEXT,
    p_column_doc   TEXT    DEFAULT NULL,
    p_is_required  BOOLEAN DEFAULT FALSE
) RETURNS JSONB
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION add_column(TEXT, TEXT, TEXT, TEXT, TEXT, BOOLEAN) IS
$$
## add_column — 为 Table 添加列

便捷函数，封装 `load_table` + `commit_table`，无需手动构造 `AddSchemaUpdate` 和 `SetCurrentSchemaUpdate`。

### 功能描述

为指定 Table 的当前 Schema 添加一个新列。内部实现直接操作 Iceberg 表元数据完成 Schema 变更。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Table 所属 Namespace 标识符（不可为空字符串） |
| `p_table` | `TEXT` | 是 | — | Table 名称（不可为空字符串） |
| `p_column_name` | `TEXT` | 是 | — | 新列名称（不可为空字符串） |
| `p_column_type` | `TEXT` | 是 | — | 列数据类型（如 `"long"`, `"string"`, `"decimal(10,2)"` 等 Iceberg 类型字符串） |
| `p_column_doc` | `TEXT` | 否 | `NULL` | 列的文档注释（可为 `NULL`） |
| `p_is_required` | `BOOLEAN` | 否 | `FALSE` | `TRUE` 表示列不可为空；`FALSE` 表示列可为空 |

### 返回值

类型：`JSONB`，同 `commit_table` 的返回结构（`CommitTableResponse`），包含更新后的表元数据。

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | 任一必填参数为 `NULL` 或空字符串；`p_column_type` 为不支持的 Iceberg 类型（如 `fixed(L)`）；列名与已有列重复 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 指定的 Namespace 不存在 |
| `P0004` | 404 | `NoSuchTableException` | 指定的 Table 不存在 |
| `P0005` | 409 | `CommitFailedException` | 并发冲突导致提交失败，客户端可重试 |
| `P0009` | 500 | `CommitStateUnknownException` | 服务端内部错误，提交状态未知 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

​```json
{"type": "BadRequestException", "message": "Unsupported type: fixed(16) is not supported", "stack": []}
```

RAISE 示例：

```sql
-- 不支持的列类型
RAISE EXCEPTION '{"type":"BadRequestException","message":"Unsupported type: fixed(16) is not supported","stack":[]}'
    USING ERRCODE = 'P0001';

-- 列名已存在
RAISE EXCEPTION '{"type":"BadRequestException","message":"Column new_col already exists in table accounting.tax","stack":[]}'
    USING ERRCODE = 'P0001';

-- Table 不存在
RAISE EXCEPTION '{"type":"NoSuchTableException","message":"The given table does not exist","stack":[]}'
    USING ERRCODE = 'P0004';
```

### 注意事项

1. **便捷封装**：此函数封装了 Schema 变更逻辑，简化添加列的常用操作；如需更复杂的列变更（如删除列、修改列注释），请直接操作 Iceberg 元数据。
2. **不支持 `fixed(L)` 类型**：根据数据类型映射表（见 §2.2），`fixed(L)` 定长二进制类型不被 OpenGauss 支持；传入此类类型将抛出 `P0001`。
3. **自动生成列 ID**：新列的 `id` 由服务端根据 `last-column-id` 自动分配（`last-column-id + 1`）；列 ID 不可由客户端显式指定。
4. **并发冲突**：若在 Schema 读取和提交之间表被其他会话变更，操作可能因冲突而失败（`P0005`），需重试。
$$;
````
---

### 6.11 rename_table

````sql
CREATE OR REPLACE FUNCTION rename_table(
    p_source_namespace TEXT,
    p_source_table     TEXT,
    p_dest_namespace   TEXT,
    p_dest_table       TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql VOLATILE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION rename_table(TEXT, TEXT, TEXT, TEXT) IS
$$
## rename_table — 重命名 Table

对应 REST API：`POST /v1/{prefix}/tables/rename`

### 功能描述

将 Table 从当前标识符（source）重命名为新标识符（destination）。支持跨 Namespace 移动，但服务端实现不强制要求支持跨 Namespace 重命名。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_source_namespace` | `TEXT` | 是 | — | 源 Table 所在 Namespace 标识符（不可为空字符串） |
| `p_source_table` | `TEXT` | 是 | — | 源 Table 名称（不可为空字符串） |
| `p_dest_namespace` | `TEXT` | 是 | — | 目标 Namespace 标识符（不可为空字符串，可与源相同或不同） |
| `p_dest_table` | `TEXT` | 是 | — | 目标 Table 名称（不可为空字符串） |

### 返回值

类型：`BOOLEAN`。成功重命名返回 `TRUE`。

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | 任一参数为 `NULL` 或空字符串 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0004` | 404 | `NoSuchNamespaceException` | 源 Namespace 或目标 Namespace 不存在 |
| `P0004` | 404 | `NoSuchTableException` | 源 Table 不存在 |
| `P0005` | 409 | `AlreadyExistsException` | 目标标识符已被已有的 Table 或 View 占用 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

​```json
{"type": "NoSuchTableException", "message": "The given table does not exist", "stack": []}
```

RAISE 示例：

```sql
-- 源 Table 不存在
RAISE EXCEPTION '{"type":"NoSuchTableException","message":"The given table does not exist","stack":[]}'
    USING ERRCODE = 'P0004';

-- 目标已存在
RAISE EXCEPTION '{"type":"AlreadyExistsException","message":"The requested table identifier already exists","stack":[]}'
    USING ERRCODE = 'P0005';

-- 目标 Namespace 不存在
RAISE EXCEPTION '{"type":"NoSuchNamespaceException","message":"The given namespace does not exist","stack":[]}'
    USING ERRCODE = 'P0004';
```

### 注意事项

1. **跨 Namespace 移动**：重命名允许将 Table 移动到不同的 Namespace，但服务端实现可以选择不支持此能力；若服务端不支持跨 Namespace 重命名，应抛出 `P0008`（`UnsupportedOperationException`）。
2. **目标不可已存在**：如果 `p_dest_namespace.p_dest_table` 已被现有 Table 或 View 占用，操作将失败并抛出 `P0005`。
$$;
````
---

### 6.12 is_table_existed

````sql
CREATE OR REPLACE FUNCTION is_table_existed(
    p_namespace TEXT,
    p_table     TEXT
) RETURNS BOOLEAN
LANGUAGE plpgsql STABLE STRICT SET search_path = ''
AS $$
BEGIN
    RAISE EXCEPTION 'not implemented' USING ERRCODE = 'P0009';
END;
$$;

COMMENT ON FUNCTION is_table_existed(TEXT, TEXT) IS
$$
## is_table_existed — 检查 Table 是否存在

对应 REST API：`HEAD /v1/{prefix}/namespaces/{namespace}/tables/{table}`

### 功能描述

检查指定的 Table 是否存在于给定 Namespace 中。不返回响应体内容。

### 参数说明

| 参数 | 类型 | 必填 | 默认值 | 语义 |
|------|------|------|--------|------|
| `p_namespace` | `TEXT` | 是 | — | Namespace 标识符（不可为空字符串） |
| `p_table` | `TEXT` | 是 | — | Table 名称（不可为空字符串） |

### 返回值

类型：`BOOLEAN`。
- `TRUE`：Table 存在。
- `FALSE`：Table 不存在（不抛异常，直接返回 `FALSE`）。

### 异常处理

| SQLSTATE | HTTP | 异常类型 | 说明 |
|----------|------|---------|------|
| `P0001` | 400 | `BadRequestException` | `p_namespace` 或 `p_table` 为 `NULL` 或空字符串 |
| `P0002` | 401 | `NotAuthorizedException` | 认证失败 |
| `P0003` | 403 | `ForbiddenException` | 无权限操作 |
| `P0008` | 503 | `UnsupportedOperationException` | 功能尚未实现 |

MESSAGE 格式：

​```json
{"type": "BadRequestException", "message": "p_table must not be NULL or empty", "stack": []}
```

RAISE 示例：

```sql
-- 参数校验失败
RAISE EXCEPTION '{"type":"BadRequestException","message":"p_table must not be NULL or empty","stack":[]}'
    USING ERRCODE = 'P0001';
```

### 注意事项

1. **查询类函数标记 STABLE**：此函数为只读查询。
2. **不抛 NoSuchTableException**：与 `is_namespace_existed` 一致，不存在时返回 `FALSE` 而非抛异常；仅在参数错误或权限问题时才抛异常。
3. **轻量操作**：对应 HTTP `HEAD` 方法，无需传输响应体；实现应使用低成本的存在性检查（如仅查元数据缓存）。
$$;
````
---
