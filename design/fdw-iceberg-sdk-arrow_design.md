# FDW + Arrow + Iceberg Java SDK：Gauss Vector 读取 Iceberg 表设计

## 1. 需求场景

### 1.1 背景

本方案的核心思路是：通过 Iceberg Java SDK 读取 Iceberg 表的元数据和数据文件，通过 Arrow C Data Interface 在 Java 和 C 之间零拷贝传递列式数据，通过 Gauss Vector FDW 框架将数据以 HeapTuple 形式交给 GV 执行器。(Gauss Vector 是基于 PG 的关系型数据库)

- **输入**：一张 Iceberg 表的 metadata 文件路径 + 存储系统凭证
- **处理**：Iceberg Java SDK 解析 metadata → 遍历 manifest → 读取 Parquet 数据文件 → 以 Arrow 列式格式输出
- **输出**：Arrow 列式数据通过 Arrow C Data Interface 零拷贝传给 C 侧 → 转换为 HeapTuple 交给 GV 执行器

## 2. 总体设计

### 2.1 三层架构

整个系统从上到下分为三层：

```
┌─────────────────────────────────────────────────────────────┐
│  FDW 层 (C 扩展)                                              │
│  ┌───────────────────────────────────────────────────────┐  │
│  │  职责: GV FDW 回调 → 批次调度 → Arrow→HeapTuple → Slot │  │
│  │                                                         │  │
│  │  BeginForeignScan   →  打开表、初始化扫描状态             │  │
│  │  IterateForeignScan →  逐批拉取 → 类型转换 → 逐行返回    │  │
│  │  EndForeignScan     →  销毁扫描、释放资源                 │  │
│  │  ReScan             →  重置状态 + 重新 Begin            │  │
│  └──────────────────────┬────────────────────────────────┘  │
│                         │ 调用                                │
├─────────────────────────┼───────────────────────────────────┤
│  C 桥接层 (C 扩展)                                           │
│  ┌──────────────────────┴────────────────────────────────┐  │
│  │  职责: JNI 封装 + Arrow C Data 导入                    │  │
│  │                                                         │  │
│  │  扫描编排     → 组装参数 → JNI 调用 → 返回指针          │  │
│  │  JNI 胶水     → FindClass / GetMethodID / CallMethod   │  │
│  │  JVM 生命周期 → CreateJavaVM / DestroyJavaVM           │  │
│  └──────────────────────┬────────────────────────────────┘  │
│                         │ JNI                                 │
├─────────────────────────┼───────────────────────────────────┤
│  Java SDK 层 (in-process JVM)                                │
│  ┌──────────────────────┴────────────────────────────────┐  │
│  │  职责: Iceberg 元数据解析 → Parquet 读取 → Arrow 导出   │  │
│  │                                                         │  │
│  │  JNI 入口     → 接收参数，打开表，逐批返回 Arrow          │  │
│  │  iceberg-core → TableMetadataParser / BaseTable        │  │
│  │  iceberg-arrow→ ArrowReader / ColumnarBatch            │  │
│  │  iceberg-data → GenericDeleteFilter（MOR 回退路径）     │  │
│  │  arrow-c-data → Arrow C Data Interface 导出		     │  │
│  │  hadoop-aws   → S3A FileSystem                        │  │
│  └───────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
```

> Java SDK 由 Formation SDK 提供

### 2.2 调用时序

```
GV Executor          FDW             C Bridge         JNI            Java SDK
══════════          ════            ════════         ════           ════════
   │                  │                  │               │               │
   │ BeginForeignScan │                  │               │               │
   ├─────────────────►│                  │               │               │
   │                  │ open(metadata,   │               │               │
   │                  │      columns)    │               │               │
   │                  ├─────────────────►│               │               │
   │                  │                  │ open_table(   │               │
   │                  │                  │   config_json)│               │
   │                  │                  ├──────────────►│               │
   │                  │                  │               │ TableMetadata │
   │                  │                  │               │ Parser.read() │
   │                  │                  │               ├──────────────►│
   │                  │                  │               │   Table obj   │
   │                  │                  │               │◄──────────────┤
   │                  │                  │    OK         │               │
   │                  │                  │◄──────────────┤               │
   │                  │   scan handle    │               │               │
   │                  │◄─────────────────┤               │               │
   │                  │                  │               │               │
   │ IterateForeignScan (循环 N 次)       │               │               │
   ├─────────────────►│                  │               │               │
   │                  │ 批次耗尽?         │               │               │
   │                  │ readNextBatch()  │               │               │
   │                  ├─────────────────►│               │               │
   │                  │                  │ readNextBatch │               │
   │                  │                  ├──────────────►│               │
   │                  │                  │               │ newScan()     │
   │                  │                  │               │ → ArrowReader │
   │                  │                  │               │ → Parquet     │
   │                  │                  │               ├──────────────►│
   │                  │                  │               │ ColumnarBatch │
   │                  │                  │               │ exportVector()│
   │                  │                  │               │◄──────────────┤
   │                  │                  │  ArrowArray*  │               │
   │                  │                  │  ArrowSchema* │               │
   │                  │                  │◄──────────────┤               │
   │                  │                  │               │               │
   │                  │  ArrowToDatum    │               │               │
   │                  │ → HeapTuple[]    │               │               │
   │                  │                  │               │               │
   │  ExecStoreTuple  │  ← 逐行返回              │               │
   │◄─────────────────┤                  │               │               │
   │       ...        │                  │               │               │
   │                  │                  │               │               │
   │ EndForeignScan   │                  │               │               │
   ├─────────────────►│                  │               │               │
   │                  │ close()          │               │               │
   │                  ├─────────────────►│               │               │
   │                  │                  │ close()       │               │
   │                  │                  ├──────────────►│               │
   │                  │                  │               │ release/close │
   │                  │                  │               ├──────────────►│
   │                  │                  │               │               │
```

图例说明：
- FDW 层维护批次状态：当前 batch 耗尽时才调用 `readNextBatch`
- 每次 `readNextBatch` 内部自动完成 `releaseBatch()` 释放上一批资源

### 2.3 数据格式转换路径

```
S3/MinIO 存储        Java SDK (JVM)        		 C Bridge             FDW            GV Executor
═══════════════      ══════════════        		  ═════════            ════          ════════════
Parquet 文件          ParquetReader        		 (同进程)              (同进程)
  │                     │                     		│                    │
  │ 磁盘字节             │ 列式内存             	   │ 列式(零拷贝)        │ 行式内存
  │                     │                     		│                    │
  ▼                     ▼                     		▼                    ▼              ▼
[列存,压缩] ─读入─► ColumnarBatch ─exportVector()─► ArrowArray ────  ArrowToDatum	──► HeapTuple
            解压缩   (FieldVector)   Arrow C     (buffer指针)   		逐行构造        (GV Datum)
                     (堆外内存)      Data I/F    + ArrowSchema
                                                  (format string)
```

关键转换点：

1. **Parquet → ColumnarBatch**：Java SDK 完成，列式到列式，无格式损失
2. **FieldVector → ArrowArray**：`Data.exportVector()` 完成。核心——数据 buffer 不动，只是生成了 C 结构体描述符（offset / length / null bitmap 指针），**零拷贝**
3. **ArrowArray → HeapTuple**：FDW 层完成，逐行类型转换填入 `TupleTableSlot`。详见 3.3.4

## 3. 特性设计

### 3.1 JNI 封装（Java 侧）

> TODO：待确认 Formation SDK 提供的实际可用接口

Java 侧提供一个 JNI 入口类，C 侧通过 `FindClass` 发现并调用。该类是对 Iceberg SDK 的薄封装，负责把 C 侧传入的参数转交给 SDK，把 SDK 的输出转为 Arrow C Data 指针返回。

#### 3.1.1 对外方法

| 方法 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `openTable` | 表定位 + 存储凭证 + 列列表 + 过滤表达式 | `int`（0 成功） | 打开表，初始化扫描 |
| `readNextBatch` | — | `int`（行数，0=EOF）+ `ArrowArray*` + `ArrowSchema*` | 读下一批，支持有delete file的MOR模式 |
| `releaseBatch` | — | — | 释放当前批次 |
| `close` | — | — | 关闭扫描 |

#### 3.1.2 SDK 依赖

`openTable` 时检测当前快照是否存在 delete file，据此选择读取路径：

**列式快路径**（无 delete file）：

| 模块 | 调用的关键 API | 用途 |
|------|--------------|------|
| `iceberg-core` | `TableMetadataParser.read(io, path)` | 加载指定 metadata 文件 |
| | `BaseTable(ops, path)` | 构造 `Table` 对象 |
| | `table.newScan()` → `TableScan` | 创建扫描 |
| | `scan.select(columns)` | 列裁剪 |
| | `scan.filter(expr)` | 谓词下推 |
| `iceberg-arrow` | `VectorizedTableScanIterable(scan)` | 向量化读取器，逐批读 Parquet，输出 `ColumnarBatch` |
| `arrow-c-data` | `Data.exportVector(allocator, fv, null, &c_array, &c_schema)` | 将每列 `FieldVector` 导出为 Arrow C Data |

**行式回退路径**（有 delete file，需应用 MOR）：

| 模块 | 调用的关键 API | 用途 |
|------|--------------|------|
| `iceberg-core` | `table.newScan().planFiles()` | 获取 `FileScanTask` 列表（含 delete file 引用） |
| `iceberg-data` | `GenericDeleteFilter(io, task, schema, projection)` | 构造 delete filter（position/equality delete） |
| | `deleteFilter.filter(records)` | 逐行应用 delete |
| | `convertToArrowTable(schema, records, batchSize)` | 行式 Record → 列式 `VectorSchemaRoot` |
| `arrow-c-data` | `Data.exportVectorSchemaRoot(root, &c_array, &c_schema)` | 整批导出为 Arrow C Data |

`openTable` 判断逻辑：读 `table.currentSnapshot()` → 检查 manifest 中是否有 delete file 条目 → 无则走列式快路径，有则走行式回退路径。对上层 FDW 透明——两种路径输出相同的 `ArrowArray*` / `ArrowSchema*` 结构体。

#### 3.1.3 内存模型

Java 侧的 Arrow buffer 走 JVM 堆外分配（JVM内存默认最小4G，最大30G）（`DirectByteBuffer`），C 侧将数据拷贝到 GV 内存后立即调用 `releaseBatch()` 释放。理想方案是将 C 侧 `palloc` 实现的自定义 `BufferAllocator` 注入 `ArrowReader`，但 `VectorizedTableScanIterable` 当前未暴露 allocator 注入点，此优化暂不实现。

### 3.2 C 侧封装

JVM 生命周期由GV管理，C 侧封装负责：JNI 调用封装、以及向 FDW 层提供简洁的扫描接口。

#### 3.2.1 扫描接口

对外暴露给 FDW 层的 C 接口：

```c
IcebergScan *iceberg_scan_open(MemoryContext cxt,
                                const char *metadata_path,
                                const char *storage_config,
                                const char **columns, int n_columns,
                                const char *filter,
                                ArrowSchema **out_schema);

int iceberg_scan_next(IcebergScan *scan,
                      ArrowArray **out_array,
                      ArrowSchema **out_schema);

void iceberg_scan_close(IcebergScan *scan);
```

- `iceberg_scan_open`：在 `cxt` 内分配 `IcebergScan`，组装参数编码后调用 `jni_open_table`。额外出参 `ArrowSchema*`（列名、类型、数量），供 FDW 初始化 converters
- `iceberg_scan_next`：调 `jni_read_batch`，返回 Arrow 指针。调用方读完一批后调 `jni_release_batch` 通知 Java 释放堆外内存
- `iceberg_scan_close`：调 `jni_close` + `MemoryContextDelete(arrow_cxt)` + `pfree`

#### 3.2.2 错误处理

JNI 调用的错误主要来自Java 侧 SDK 内部异常。典型场景举例：

| 错误级别 | 适用场景 | 处理方式 | JNI 侧关键动作 |
|---------|----------|----------|----------------|
| **FATAL** | JVM 创建失败、JNI 函数因 JVM 内部致命错误而崩溃、无法加载必需的本机库 | 终止后端进程（或标记该 Backend 永久不可用），记录严重日志，触发报警 | Gauss Vector 管理 |
| **ERROR** | Java 方法抛出业务异常（如 S3 不可达、类型转换错误）、`AttachCurrentThread` 失败、`FindClass` 找不到类、`GetMethodID` 失败 | 当前事务/查询失败并回滚，向上层返回明确的错误码和错误信息，但后端进程继续运行 | 1. `ExceptionCheck()` → true<br>2. `ExceptionDescribe()` 或提取堆栈<br>3. `ExceptionClear()`<br>4. 将信息转换为 `ereport(ERROR)` 或等效的 C++ 异常 |
| **WARNING** | 暂不设置，Java 侧发生的异常均记为 ERROR |    |   |
| **LOG/INFO** | Java 侧的正常状态信息（如 JVM 启动参数、SDK 版本）、JNI 调用的性能统计数据 | 按日志级别输出到系统日志，业务无感知 | 不涉及异常检查。若需要从 Java 获取信息，通过普通方法调用返回字符串，再打印。 |

每次 JNI 调用后必须检查 `ExceptionCheck()`——JNI 不会自动把 Java 异常转成返回码，不检查则后续 JNI 调用行为未定义。捕捉后 `ExceptionClear()` 清掉，避免残留影响后续操作。


### 3.3 FDW 设计

FDW 层是 GV 执行器与 C 桥接层之间的适配层，负责实现 GV FDW 回调、管理批次状态、以及执行 Arrow → HeapTuple 类型转换。

#### 3.3.1 FDW 回调概览

GV 执行器通过六个核心回调驱动 FDW 扫描，具体参考 FDW 设计文档。FDW 协议是行式迭代模型：每次 `IterateForeignScan` 返回一行或空 slot（EOF）。

| 回调 | 触发时机 | 职责 |
|------|---------|------|
| `GetForeignRelSize` | 查询规划阶段 | 轻量获取行数估算，设置 `baserel->rows` |
| `GetForeignPlan` | 查询规划阶段 | 提取列列表和 WHERE 条件，存入 `fdw_private` |
| `BeginForeignScan` | 执行开始 | 打开扫描、初始化状态和转换器（见 3.3.2） |
| `IterateForeignScan` | 逐行拉取（热路径） | 批次状态机 + Arrow→HeapTuple 转换（见 3.3.3） |
| `EndForeignScan` | 执行结束 | 关闭扫描、释放资源 |
| `ReScanForeignScan` | 重新扫描 | 关旧扫描 → 重新 Begin |

#### 3.3.2 BeginForeignScan

FDW 框架层的 `IcebergScanState`（由 FDW 详细设计定义）有一个 `void *reader` 字段指向 reader 内部状态。本方案定义 reader 内部结构体，挂在 `reader` 后面：

```c
// Reader 内部结构体（挂在 IcebergScanState.reader 后面）
struct IcebergReaderState {
    IcebergScan *scan;           // iceberg_scan_open 返回的 opaque handle
};
```

框架层字段（`tuples`/`num_tuples`/`next_tuple`/`batch_cxt`/`tupdesc`）属于 `IcebergScanState`，reader 不感知。

初始化流程：

```
void BeginForeignScan(ForeignScanState *node):

    // 1. 分配 reader 内部态，挂在 fsstate->reader
    reader = palloc0(sizeof(IcebergReaderState))
    fsstate->reader = reader

    // 2. 调 C 桥接接口 iceberg_scan_open()，同时获取 schema
    //   → 内部调 jni_open_table() → Java openTable()
    //   → JVM 首次调用时自动启动
    //   metadata_path 来自 fdw_private，由 FDW 框架层在 GetForeignPlan 时取得
    reader->scan = iceberg_scan_open(
        CurrentMemoryContext,
        metadata_path, storage_config,
        columns, n_columns, filter_expr,
        &c_schema)

    node->fdw_state = fsstate
```

#### 3.3.3 IterateForeignScan

`IterateForeignScan` 内部维护一个批次状态机，因为 GV 每次调用只返回一行，而 C 桥接层一次返回整批（默认 64K 行）。Arrow → HeapTuple 的类型转换逻辑封装在 `fetch_more_data()` 中，与 FDW 回调解耦：

```
tuple IterateForeignScan(ForeignScanState *node):
    // 1. 判断是否需要拉新批次
    if fsstate->next_tuple >= fsstate->num_tuples:
        MemoryContextReset(fsstate->batch_cxt)

        // → 调 fetch_more_data() （实现在 C 桥接层）
        //   → 内部调 iceberg_scan_next(reader->scan) → Arrow 数据
        //   → 按 c_schema->format 查静态表得 converter → heap_form_tuple()
        //   → 释放 Arrow buffer（相当于 jni_release_batch）
        //   → 结果填入 fsstate->tuples / num_tuples / next_tuple
        if fetch_more_data(reader, fsstate) == 0:
            return NULL          // EOF

    // 2. 从当前批次取一行返回
    tuple = fsstate->tuples[fsstate->next_tuple]
    fsstate->next_tuple++
    ExecStoreHeapTuple(tuple, slot)
    return slot
```

`fetch_more_data()` 内部包含两个阶段：
1. **拉取**：调用 `iceberg_scan_next(reader->scan)` 获取 Arrow 列式数据
2. **转换**：通过 `c_schema->children[c]->format` 查静态表获取 converter（O(1)），逐列逐行将 Arrow 数据转为 Datum，再通过 `heap_form_tuple()` 组装 HeapTuple，填入 `fsstate->tuples`

批次数据和游标由框架层 `IcebergScanState` 的三个字段管理——`tuples`、`num_tuples`、`next_tuple`——`fetch_more_data` 直接读写这些字段。

#### 3.3.4 Arrow → HeapTuple 类型转换

热路径按 Arrow schema 的 format 字符串直接查静态表获取 converter，无需在 `BeginForeignScan` 预构建。静态表由 `g_converter_map[256]` 数组实现，以 `format[0]` 为索引，O(1) 查找：

```c
// 静态表（进程生命周期内仅初始化一次）
static IcebergArrowToDatum g_converter_map[256] = {NULL};

// lookup 函数
IcebergArrowToDatum iceberg_converter_for_format(const char *format)
{
    if (format[0] == 't') {
        if (strcmp(format, "tdD") == 0) return convert_date32;
        if (strcmp(format, "tts") == 0) return convert_timestamp;
        return NULL;
    }
    return g_converter_map[(unsigned char) format[0]];
}

// fetch_more_data 热路径（查表 + 调用）
for (int r = 0; r < nrows; r++) {
    for (int c = 0; c < ncols; c++) {
        IcebergArrowToDatum conv;
        conv = iceberg_converter_for_format(c_schema->children[c]->format);
        values[c] = conv(c_array->children[c], r, &isnull[c]);
    }
    tuples[r] = heap_form_tuple(tupdesc, values, isnull);
}
```

> TODO: 等宽类型（int64/float8/timestamp）Arrow buffer 与 Datum 都是 8 字节，可直接 memcpy 整列 + 单次 null bitmap 扫描，替代逐行 converter 调用，提升热路径性能。

Iceberg 类型到 GaussVector 类型的映射表：

| Iceberg Type | GaussVector Type | 转换方式 |
|-------------|-----------------|---------|
| `int` | INTEGER | `Int32GetDatum`，直接读 buffer 值 |
| `long` | BIGINT | `Int64GetDatum`，直接读 buffer 值 |
| `boolean` | BOOLEAN | `BoolGetDatum`，bitmap 取 bit |
| `float` | FLOAT4 | `Float4GetDatum`，直接读 buffer 值 |
| `double` | FLOAT8 | `Float8GetDatum`，直接读 buffer 值 |
| `string` | TEXT / VARCHAR | 从 offset buffer 取字符串指针+长度，`palloc` 拷贝后赋值 |
| `binary` | BYTEA | 同 string，`PointerGetDatum` + `palloc` 拷贝 |
| `uuid` | UUID | 16 字节定长，直接 memcpy |
| `fixed(L)` | — | 不支持 |
| `date` | DATE | 距 epoch 天数，直接赋值 `DateADTGetDatum` |
| `time` | TIME | 微秒数，`TimeADTGetDatum` |
| `timestamp` | TIMESTAMP | epoch 微秒，`TimestampGetDatum` |
| `decimal(p,s)` | DECIMAL | 从 Arrow decimal128 buffer（16 字节）转换为 GaussVector Numeric 内部表示 |

定点类型（int/long/float/double/date/time/timestamp/uuid）直接读 buffer 值赋 Datum，不做拷贝。变长类型（string/binary）必须 `palloc` 拷贝到 `batch_cxt`，因为 Arrow buffer 在 `jni_release_batch` 后被 Java 回收。`decimal` 需额外转换：Iceberg 用 16 字节紧凑表示，GaussVector Numeric 用变长结构，需逐值展开。



## 4. 与 GV 已有方案的关键异同

GV（GaussVector）已有 Iceberg 读取实现，两条路径由参数 `enable_istore_cache` 控制：

- **Formation**（Java SDK，`enable_istore_cache=false`）：通过 Iceberg Java SDK 读取，支持 MOR（GenericDeleteFilter），数据经过行式 Record 中间层后转为 Arrow 列式。本章对比主要针对此路径。
- **直接 Parquet**（C++，`enable_istore_cache=true`）：C++ 侧直接用 `parquet::arrow::FileReader` 读 Parquet 文件，跳过 Java SDK。不支持 MOR。AOT 模式自动启用此路径。本章仅在相关特性处提及。

### 4.1 表定位

**GV Formation**：走完整 catalog 路径。`catalog.loadTable(tableId)` 通过 catalog service（REST/Hive/Glue）解析表名 → 获取 metadata 文件路径 → 加载表元数据。

**GV 直接 Parquet**：不经过 Java SDK。文件路径从系统表 `lake_catalog.datafile` 获取，C++ 侧直接读路径对应的 Parquet 文件。跳过了 Iceberg metadata 的解析和 manifest 遍历，但仍需系统表做文件路径的映射。

**本方案**：metadata 直读。调用方直接提供 metadata 文件完整路径，`TableMetadataParser.read(io, path)` 加载指定快照。不需要 catalog service。

**异同**：本方案与 GV Formation 都通过 Java SDK 加载 metadata 后走相同读流程，差异只在"表名→文件路径"由谁完成。直接 Parquet 路径则完全绕过了 Iceberg 元数据层。

### 4.2 文件读取

**GV Formation**：`planFiles()` → `FileScanTask` → `FormationReader.open(task)`：
- `GenericDeleteFilter(io, task, ...)` 构造 delete filter
- `openFile(task)` 读 Parquet → `Record` 列表（行式）
- `deletes.filter(records)` 应用 position/equality delete
- 完整支持 MOR

**GV 直接 Parquet**：`parquet::arrow::FileReader::ReadTable()` → `arrow::Table`。纯 C++ Parquet 读取，不经过 Java，不支持 MOR。

**本方案**：`VectorizedTableScanIterable(scan)` → `ArrowReader` 批量读 Parquet → `ColumnarBatch`（列式）。**不支持 delete file**。

**异同**：三个路径对 MOR 的支持递进：GV Formation 完整支持（代价是行式中间层），本方案和直接 Parquet 不支持。本方案的向量化路径是 Iceberg 社区的性能优化方案，同为列式但比直接 Parquet 多了 Iceberg schema evolution / partition pruning 等语义支持。


### 4.3 Parquet → Arrow → 导出

**GV Formation**：`Parquet → Record(行式) → convertToArrowTable() → VectorSchemaRoot(列式) → Data.exportVectorSchemaRoot()`。存在行→列转换开销，MOR 支持的代价。

**GV 直接 Parquet**：`Parquet → arrow::Table(列式)`。C++ 原生列式，进程内直接使用，无需跨语言导出。

**本方案**：`Parquet → ColumnarBatch(列式, iceberg-arrow) → FieldVector → Data.exportVector() → ArrowArray* + ArrowSchema*`。全程列式，无行列转换。导出途径与 GV Formation 功能等价，最终都是 Arrow C Data struct。

### 4.4 内存模型

**GV Formation**：Arrow buffer 由 Java `DirectByteBuffer` 分配，JVM Cleaner 回收。C++ 侧通过 `arrow::ImportRecordBatch` 拷贝数据到 C++ Arrow 内存。

**GV 直接 Parquet**：`parquet::arrow::FileReader` 使用 GaussArrowMemoryPool 分配，直接在 C++ 内存空间。

**本方案**：当前与 GV Formation 一致——Arrow buffer 由 Java 堆外分配，C 侧拷贝后通知 Java 释放。长期方向为 C 侧 allocator 注入，但依赖 SDK 暴露注入点。

**异同**：本方案与 GV Formation 内存模型相同（Java 分配 + C 拷贝）。相比直接 Parquet 路径的纯 C++ 内存多了一次跨堆拷贝。

### 4.5 JVM 线程模型

**GV Formation** 和 **本方案** 一致：单例 JVM、`pthread_once` 保证初始化、工作线程 `AttachCurrentThread` 按需 attach、TLS 缓存 `JNIEnv*`、线程退出时通过 TLS destructor 自动 detach。

**GV 直接 Parquet**：不涉及 JVM。
