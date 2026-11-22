# mini-migrator

[English](README.md) | **中文** | [Русский](README.ru.md)

原生 C++17 编写的异构数据库迁移工具，支持 **MySQL、PostgreSQL、openGauss、
SQL Server、Oracle** 之间的全量迁移、校验、断点续传与 CDC。单个二进制文件，
不依赖 Docker、Python 或任何外部服务。

```bash
./run.sh --mode mysql2pg --source "db=app;host=10.0.0.5;user=reader;password=env:SRC_PW" \
                             --target "host=10.0.0.9;user=postgres;db=app_dst;password=env:DST_PW" \
         --batch 5000 --commit 10 --workers 4
```

## 特点

- **13 种原生模式**，覆盖五种数据库；其它组合用 `--source-type` / `--target-type` 表达
- **行数 + 校验和验证**（FNV 或方言支持的 mmv 摘要）
- **主键区间并行**：大表按主键边界切分，多 worker 同时拷贝
- **MySQL 目标端自适应装载**：窄行走 `LOAD DATA`，宽行（>512 B/行）自动改多行 `INSERT`
- **断点续传**（主键水位）、`--verify-async` 异步校验
- **CDC**：MySQL binlog / PostgreSQL 逻辑复制，可选 DDL 同步
- **内置 Web 面板**（中/英/俄），支持审计日志与 TLS
- **`doctor`** 自检客户端库、ODBC 驱动、批量装载工具
- **除客户端库外无运行时依赖，不需要 Docker**

## 支持的方向

| 模式 | 源 | 目标 | 说明 |
|---|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL | 全量 + 校验 |
| `mysql2og` | MySQL | openGauss | 全量 + 校验 |
| `pg2mysql` | PostgreSQL | MySQL | 全量 + 校验 |
| `pg2og` | PostgreSQL | openGauss | 全量 + 校验 |
| `og2pg` | openGauss | PostgreSQL | 全量 + 校验 |
| `og2mysql` | openGauss | MySQL | 全量 + 校验 |
| `mysql2mysql` | MySQL | MySQL | 全量 + 校验 |
| `mysql2mssql` | MySQL | SQL Server | `bcp` 通道，可回退 INSERT |
| `mssql2mysql` | SQL Server | MySQL | 全量 |
| `mssql2mssql` | SQL Server | SQL Server | `bcp` 通道 |
| `mysql2oracle` | MySQL | Oracle | `sqlldr` 通道 |
| `oracle2mysql` | Oracle | MySQL | 全量 |
| `oracle2oracle` | Oracle | Oracle | `sqlldr` 通道 |

其它组合可覆盖类型参数：

```bash
./build/migrator --mode mysql2pg --source-type oracle --target-type pg ...
```

## 安装

### 免 Docker 便携包（推荐）

```bash
tar -xzf migrator-1.0.0-linux-x64.tar.gz
cd migrator-1.0.0-linux-x64
./run.sh doctor          # 环境自检
./run.sh --help
```

包内包含二进制、随附客户端库（`libmysqlclient`、`libpq`、`unixODBC`）和类型映射配置。
自行构建：

```bash
bash packaging/build-portable.sh 1.0.0
```

目标机要求：Linux x86-64、glibc 2.39+、能直连数据库端口。仅当目标或源是
SQL Server / Oracle 时才需要 ODBC 驱动（`msodbcsql18`、Oracle Instant Client + ODBC）。

### 从源码构建

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4
```

macOS：`brew install mysql-client libpq unixodbc openssl@3 && make -j4`

### Debian 包 / 容器

```bash
bash packaging/build-deb.sh
sudo dpkg -i mini-migrator_0.9.0_amd64.deb
```

Docker 镜像仅为 CI 方便，二进制本身不需要它。

## 使用

### DSN 格式

```
key=value;key=value
```

| 键 | 含义 |
|---|---|
| `host` / `port` | 地址与端口 |
| `user` / `password` | 账号；支持 `password=env:VAR`、`password=file:PATH` |
| `db` | 数据库名 / 服务名 |
| `driver` | ODBC 驱动名（SQL Server、Oracle 必填） |

同时出现 `driver=` 时，`password=` 必须放在**最后一个字段**，否则解析会把后面的内容当成密码。

### 典型命令

```bash
./build/migrator --mode mysql2oracle \
  --source "db=erp;host=10.0.0.5;port=3306;user=reader;password=env:SRC_PW" \
  --target "host=10.0.0.9;port=1521;user=ERP;db=FREEPDB1;driver=Oracle 23 ODBC Driver;password=env:DST_PW" \
  --batch 5000 --commit 10 --workers 4 --truncate --verify-async
```

### 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mode` | `sim` | 迁移模式 |
| `--source` / `--target` | — | DSN |
| `--batch` | 1000 | 每批行数 |
| `--commit` | 10 | 每 N 批提交 |
| `--workers` | 1 | 并行 worker / 主键区间数 |
| `--pk-min-rows` | 1000000 | 触发主键区间并行的表行数 |
| `--truncate` | 关 | 迁移前清空目标表 |
| `--load-mode` | `auto` | MySQL 目标：`auto`/`insert`/`ignore`/`replace` |
| `--io-scheduler` | `fair` | `off`/`fair`/`scan` |
| `--no-verify` | 关 | 跳过校验 |
| `--verify-async` | 关 | 迁移后异步校验 |
| `--resume` | 关 | 主键水位续传 |
| `--cdc` | 关 | 全量后持续同步 |
| `--tables-config` | — | 表级策略：`copy`/`rebuild:file.sql`/`skip` |
| `--type-map` | `config/type_map.json` | 类型映射 |
| `--port` | 8123 | 面板端口 |

## 性能

决定吞吐的只有两件事：目标端怎么提交、数据怎么插入。

**MySQL 目标端最关键的设置：**

```ini
[mysqld]
innodb_buffer_pool_size=16G
innodb_doublewrite=OFF
innodb_flush_method=O_DIRECT
innodb_flush_neighbors=0
innodb_io_capacity=2000
innodb_io_capacity_max=4000
innodb_flush_log_at_trx_commit=2
skip-log-bin
max_allowed_packet=64M
net_read_timeout=3600
net_write_timeout=3600
```

工具侧：提高 `--commit`（减少 fsync）、`--workers` 保持 4–8、`--load-mode auto` 不要关——
宽表会自动切多行 INSERT，避开服务端会掐断的超长 `LOAD DATA` 语句。

实测（62 GB 内存 / NVMe，Docker 承载数据库，2000 万行数据集）：

| 方向 | 吞吐 |
|---|---|
| mysql → pg | 176 MB/s |
| pg → pg | 150 MB/s |
| mssql → pg | 187 MB/s |
| mysql → mssql | 25 MB/s |
| * → oracle | 3.5–4.3 MB/s |

Oracle 目标端是明显短板：逐行装载。若目标是 Oracle，需预留时间或启用 `sqlldr`。

### 面板给出的指标

- 吞吐（MB/s）与行/秒
- 读/写放大（容器块设备字节 ÷ 数据载荷）
- 峰值 RSS（VmHWM）与目标库内存
- 单表进度、错误计数、ETA

## 校验

```bash
./build/migrator --mode mysql2pg ... --verify-engine fnv
./build/migrator --mode mysql2pg ... --verify-async
```

校验会比较行数和逐列规范化摘要，因此 DECIMAL/NUMERIC、DATETIME/TIMESTAMP
这类类型差异不会造成误报。

## CDC

```bash
./build/migrator --mode mysql2pg ... --cdc --cdc-drop-slot
```

MySQL 需要 `binlog_format=ROW`；PostgreSQL 需要 `wal_level=logical`。

## 常见问题

| 现象 | 处理 |
|---|---|
| `Lost connection to MySQL server during query` | 调高 `net_read_timeout`/`net_write_timeout`，保持 `--load-mode auto` |
| `Can't open lib 'ODBC Driver 18 for SQL Server'` | 安装 `msodbcsql18` 并注册到 `odbcinst.ini` |
| `ORA-12541: no listener` | 检查端口与监听 |
| `ORA-01017: invalid credential` | 密码含特殊字符，改用 `password=file:PATH` |
| `ORA-12954` 超出数据库大小上限 | PDB 达到 `MAXSIZE`，用 `STORAGE (MAXSIZE 64G)` 重建 |
| 大表校验超时 | 用 `--verify-async` |
| `error while loading shared libraries` | 目标机 glibc 低于构建机 |

`./build/migrator doctor` 输出机器可读的环境检查结果。

## 限制

- Oracle `BLOB` 表走两阶段 hex 路径，比普通表慢
- `bcp`/`sqlldr` 通过命令行或临时文件接收凭据
- CDC 需要服务端预先开启复制
- 面板只读，模块开关是编译期的

## 文档

- [docs/usage.md](docs/usage.md) —— 完整参数手册
- [docs/dependencies.md](docs/dependencies.md) —— 驱动、装载工具与排错

## 许可

见 [LICENSE](LICENSE)。
