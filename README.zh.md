# mini-migrator

[English](README.md) | **中文** | [Русский](README.ru.md)

基于 C++17 的异构数据库迁移工具，支持 **MySQL、PostgreSQL、openGauss、SQL Server、Oracle**。
支持多方向全量迁移、行数与校验和比对、CDC 增量同步；SQL Server 与 Oracle 通过 ODBC 接入，
并可使用官方批量装载器（`bcp` / `sqlldr`）。

## 功能

- 13 个原生迁移方向，覆盖五个数据库；其余组合可用 `--source-type` / `--target-type` 覆盖
- 行数 + 校验和验证（FNV 或 mmv 摘要）
- 批量装载通道：SQL Server 用 `bcp`，Oracle 用 `sqlldr`；缺失时自动回退 SQL 插入
- Oracle `BLOB` 表两段式装载（十六进制暂存 + `TO_BLOB`）
- CDC：MySQL binlog、PostgreSQL 逻辑复制，可选 DDL 同步
- 主键水位断点续传、单表主键区间并行
- Web 面板（中/英/俄），支持审计日志与 TLS
- `doctor` 自检：客户端库、ODBC 驱动、批量装载器
- Debian 安装包与自带依赖的 Docker 镜像

## 支持方向

| 模式 | 源 | 目标 | 说明 |
|---|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL | 全量 + 校验 |
| `mysql2og` | MySQL | openGauss | 全量 + 校验 |
| `pg2mysql` | PostgreSQL | MySQL | 全量 + 校验 |
| `pg2og` | PostgreSQL | openGauss | 全量 + 校验 |
| `og2pg` | openGauss | PostgreSQL | 全量 + 校验 |
| `og2mysql` | openGauss | MySQL | 全量 + 校验 |
| `mysql2mysql` | MySQL | MySQL | 全量 + 校验 |
| `mysql2mssql` | MySQL | SQL Server | `bcp` 通道 |
| `mssql2mysql` | SQL Server | MySQL | 全量 |
| `mssql2mssql` | SQL Server | SQL Server | `bcp` 通道 |
| `mysql2oracle` | MySQL | Oracle | `sqlldr` 通道 |
| `oracle2mysql` | Oracle | MySQL | 全量 |
| `oracle2oracle` | Oracle | Oracle | `sqlldr` 通道 |

其他组合可覆盖类型，例如：

```bash
./build/migrator --mode mysql2pg --source-type oracle --target-type pg ...
```

## 快速开始

### 构建

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4            # 或：cmake -S . -B build -G Ninja && cmake --build build -j
```

macOS：

```bash
brew install mysql-client libpq unixodbc openssl@3
make -j4
```

### 环境自检

```bash
./build/migrator doctor
```

`doctor` 检查客户端库、已注册的 ODBC 驱动、`bcp` 与 `sqlldr`；只在硬失败时返回非零。
驱动名与工具路径来自 `config/providers.ini`（或环境变量 `MIG_MSSQL_DRIVER`、
`MIG_ORACLE_DRIVER`、`MIG_BCP`、`MIG_SQLLDR`）。详见 [docs/dependencies.md](docs/dependencies.md)。

### 执行迁移

```bash
./build/migrator --mode mysql2pg \
  --source "host=127.0.0.1;port=3306;user=root;password=...;db=demo" \
  --target "host=127.0.0.1;port=5432;user=postgres;password=...;db=demo_dst" \
  --batch 5000 --workers 4 --truncate --exit
```

SQL Server 目标需在 DSN 里指定驱动：

```bash
--target "host=127.0.0.1;port=1433;user=sa;password=...;db=demo_dst;driver=ODBC Driver 18 for SQL Server"
```

## 批量装载通道

检测到 `bcp` 或 `sqlldr` 时，目标端可整批交给厂商装载器；缺失则回退逐行 SQL 插入。

实测环境：505 万行 / 7 张表（Ubuntu，Docker 数据库）：

| 目标 | 通道 | 耗时 | 吞吐 |
|---|---|---|---|
| SQL Server | `bcp` | 35.8 s | 8.8 MB/s |
| SQL Server | INSERT 回退 | 233 s | 1.3 MB/s |
| Oracle | `sqlldr` | 133.7 s | 2.4 MB/s |
| Oracle | INSERT ALL 回退 | 1285 s | 0.2 MB/s |

Oracle `BLOB` 列采用两段式：先以十六进制文本写入暂存表，再 `TO_BLOB(HEXTORAW(...))` 转入目标表。

## 打包

```bash
# Debian/Ubuntu 安装包
bash packaging/build-deb.sh
sudo dpkg -i mini-migrator_0.9.0_amd64.deb

# 自带依赖的镜像（msodbcsql18 + Instant Client + bcp/sqlldr）
docker build -t mini-migrator:full -f packaging/Dockerfile .
docker run --rm mini-migrator:full doctor
```

安装包会安装 `/usr/bin/mini-migrator` 与 `/etc/mini-migrator/providers.ini`。

## 常用参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--mode` | `sim` | 迁移模式（见上表） |
| `--source` / `--target` | — | DSN：`host=;port=;user=;password=;db=` |
| `--batch` | 1000 | 每批行数 |
| `--commit` | 10 | 每 N 批提交一次 |
| `--workers` | 1 | 并行线程数，同时控制主键区间并行 |
| `--truncate` | 关 | 迁移前清空目标表 |
| `--no-verify` | 关 | 跳过行数与校验和验证 |
| `--resume` / `--resume-dir` | 关 | 按主键水位续传 |
| `--cdc` | 关 | 全量后继续应用增量 |
| `--with-views` | 关 | 迁移视图（支持的方言） |
| `--port` | 8123 | Web 面板端口 |

## 文档

- [docs/usage.md](docs/usage.md)：完整参数与用法
- [docs/dependencies.md](docs/dependencies.md)：驱动、批量装载器与故障排查
- `migrator doctor`：环境自检

## 限制

- Oracle `BLOB` 表走两段式，速度低于普通表
- `bcp` / `sqlldr` 的凭据通过命令行或临时文件传递，需限制主机访问
- CDC 需要 MySQL binlog 或 PostgreSQL 逻辑复制支持
- Web 面板只读；模块开关是编译期的

## 许可证

见 [LICENSE](LICENSE)。
