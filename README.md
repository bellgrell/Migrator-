# mini-migrator

面向国产数据库的**异构数据迁移工具**（学习 / 作品项目）。

- 源端：MySQL / PostgreSQL
- 目标端：openGauss / DM8

> 项目定位：**数据搬运 + 类型映射 + 校验**，不是数据库引擎。
> 主要参考：pgloader（类型映射）、DataX/Addax（插件化）、Debezium（CDC）。

## 路线

| 阶段 | 内容 |
|---|---|
| M0 地基 | 配置 / 日志 / 通用类型 / 重试 |
| M1 连接层 | 连接池 + 各数据库连接封装 |
| M2 源端 | ISource 抽象 + MySQL 实现 |
| M3 目标端 | ITarget 抽象 + openGauss 实现 |
| **M4 类型映射** | ⭐ 两级映射 + DDL 生成 |
| **M5 全量搬运** | ⭐ 游标读 + 批量写 + 事务 |
| M6 校验 | 行数 / checksum / 抽样比对 |
| M7 增量 CDC | binlog 解析 + 位点衔接 |
| M8 工程化 | CLI / 重试 / 死信队列 |
| M9 DM8 | 可选，留接口 |

## 目录

    include/migrator/     公共接口（status / slice / options / source / target）
    src/common/           基础设施（日志、重试）
    src/config/           配置解析 + 类型映射
    src/db/mysql/         MySQL（含连接池）
    src/db/postgres/      PostgreSQL
    src/db/opengauss/     openGauss（继承 postgres）
    src/db/dm8/           DM8（占位）
    src/schema/           Schema 抽取 + DDL 生成
    src/data/             全量搬运（copier / batch_writer）
    src/cdc/              增量捕获
    src/verify/           校验
    src/util/             编码 / 校验和 / 状态实现
    config/               YAML 配置（类型映射表放这里）
    tests/ bench/ docs/
    archive/              上一版（LSM 存储引擎）的代码，保留但不参与构建

## 两个核心设计

### 1. 两级类型映射

    第一级：全局类型映射       mysql_type -> target_type
    第二级：列级覆盖           某表某列的特殊处理

参考 sqlines 的 sqlines_dtmap.txt / sqlines_cmap.txt。
映射表是**配置文件**，改类型不用重新编译。

### 2. 读一批、写一批

    OpenCursor -> 循环 FetchBatch(N) -> WriteBatch(N) -> 每 M 批 Commit -> CloseCursor

不要读一行写一行。

## 构建

    make

## 环境

- MySQL / PostgreSQL：Docker 一条命令
- openGauss：用 PostgreSQL 代替（同源，90% 语法相通）
- DM8：商业版，暂留接口和映射表
