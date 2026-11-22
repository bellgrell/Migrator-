# mini-migrator

[English](README.md) | **中文** | [Русский](README.ru.md)

[![Release](https://img.shields.io/github/v/release/bellgrell/Migrator-?display_name=tag&sort=semver&label=release)](https://github.com/bellgrell/Migrator-/releases/latest) [![Platform](https://img.shields.io/badge/platform-linux%20x86--64-blue)](https://github.com/bellgrell/Migrator-/releases/latest) [![License](https://img.shields.io/github/license/bellgrell/Migrator-)](LICENSE)

> 一个二进制文件，把数据从 A 库搬到 B 库。不依赖 Docker，不依赖 Python，不依赖中间件。

mini-migrator 是 C++17 编写的异构数据库迁移工具，支持 **MySQL、PostgreSQL、
openGauss、SQL Server、Oracle** 之间 13 种方向的批量迁移，附带行数与校验和验证、
主键区间并行、断点续传、CDC 增量同步，以及一个只读的 Web 监控面板。

## 为什么需要它

跨库搬数据的现实选择通常只有两个：写个逐行循环的脚本（慢，且容易把目标库拖垮），
或者买一套只认两种数据库的厂商工具（贵，且不覆盖国产库）。

mini-migrator 只做一件事：**以目标库能吸收的最快速度把 schema 和数据搬过去，
然后诚实地告诉你结果**——搬了多少行、有没有校验通过、写放大多少、哪张表失败了。

## 特性

- **13 种原生方向**，覆盖五种数据库；其余组合用 `--source-type` / `--target-type` 表达
- **主键区间并行**：大表按 PK 边界切分，N 个 worker 同时读，不再单线程爬
- **MySQL 目标端自适应装载**：窄行走 `LOAD DATA`，宽行（>512 B/行）自动切多行 `INSERT`
- **行数 + 校验和验证**：FNV 或 mmv 摘要，可检出单行篡改
- **断点续传**：按主键水位恢复，`--verify-async` 把校验移出关键路径
- **CDC**：MySQL binlog / PostgreSQL 逻辑复制，可选 DDL 同步
- **可观测**：内置面板输出吞吐（MB/s）、行/秒、读/写放大、峰值 RSS、目标库内存
- **单二进制部署**：Release 里是免 Docker 的便携包，解压即可运行

## 快速开始

### 下载

前往 [Releases](https://github.com/bellgrell/Migrator-/releases/latest) 下载
`migrator-1.0.0-linux-x64.tar.gz`，校验后解压：

```bash
shasum -a 256 -c SHA256SUMS
tar -xzf migrator-1.0.0-linux-x64.tar.gz
cd migrator-1.0.0-linux-x64
./run.sh doctor        # 环境自检
```

### 跑一次迁移

```bash
./run.sh --mode mysql2pg \
  --source "db=app;host=10.0.0.5;port=3306;user=reader;password=***" \
  --target "host=10.0.0.9;port=5432;user=postgres;db=app_dst;password=***" \
  --batch 5000 --commit 10 --workers 4 --truncate --verify-async
```

密码支持 `password=env:VAR` 与 `password=file:PATH`，不必写在命令行里。
如果 DSN 里有 `driver=`，`password=` 必须放在**最后一个字段**。

### 从源码构建

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4
```

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

任意其它组合：

```bash
./run.sh --mode mysql2pg --source-type oracle --target-type pg ...
```

## 性能

决定吞吐的只有两件事：**目标端怎么提交**、**数据怎么插入**。

MySQL 目标端最关键的设置：

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

工具侧：提高 `--commit` 减少 fsync，`--workers` 保持 4–8，`--load-mode auto` 不要关。

实测（62 GB 内存 / NVMe，2000 万行数据集）：

| 方向 | 吞吐 |
|---|---|
| mysql → pg | 176 MB/s |
| pg → pg | 150 MB/s |
| mssql → pg | 187 MB/s |
| mysql → mssql | 25 MB/s |
| \* → oracle | 3.5–4.3 MB/s |

Oracle 目标端逐行装载，是整个矩阵的瓶颈；若目标是 Oracle，请预留时间或启用 `sqlldr`。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/usage.md](docs/usage.md) | 完整参数手册与常用配方 |
| [docs/dependencies.md](docs/dependencies.md) | 驱动、装载工具、依赖安装 |
| [docs/operations.md](docs/operations.md) | 运维：面板、日志、审计 |
| [docs/adapters-mssql-oracle.md](docs/adapters-mssql-oracle.md) | SQL Server / Oracle 适配说明 |
| [INSTALL.md](packaging/portable/INSTALL.md) | 便携包部署与排错 |
| [docs/README.md](docs/README.md) | 文档索引 |

## 常见问题

| 现象 | 处理 |
|---|---|
| `Lost connection to MySQL server during query` | 调高 `net_read_timeout`/`net_write_timeout`，保持 `--load-mode auto` |
| `Can't open lib 'ODBC Driver 18 for SQL Server'` | 安装 `msodbcsql18` 并注册到 `odbcinst.ini` |
| `ORA-12541` | 检查端口与监听 |
| `ORA-01017` | 密码含特殊字符，改用 `password=file:PATH` |
| `ORA-12954` | PDB 达到 `MAXSIZE`，用 `STORAGE (MAXSIZE 64G)` 重建 |
| 大表校验超时 | 用 `--verify-async` |
| `error while loading shared libraries` | 目标机 glibc 低于构建机 |

## 参与贡献

- 提 Issue 请附：数据库版本、`doctor` 输出、失败方向的日志片段
- 改代码后请先 `bash -n` 校验脚本、再跑一个单方向冒烟，最后跑矩阵
- 打包脚本：`bash packaging/build-portable.sh 1.0.0`

## 许可证

[LICENSE](LICENSE)

## 联系

- 仓库：https://github.com/bellgrell/Migrator-
- 问题：https://github.com/bellgrell/Migrator-/issues
