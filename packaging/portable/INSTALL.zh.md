# migrator 1.0.0 - Linux x86-64 便携包

**中文** | [English](INSTALL.md)

不需要 Docker。这是一个原生二进制，通过 TCP 直接连接数据库。

## 目录结构

    bin/migrator      迁移主程序
    lib/              随附客户端库（MySQL、PostgreSQL、unixODBC）
    config/           类型映射、表策略、模块与 providers 配置
    run.sh            设置 LD_LIBRARY_PATH 的启动器

## 目标机要求

- Linux x86-64，glibc 2.39+（Ubuntu 24.04+ / Debian 13+）
- 能直连源库与目标库端口
- 仅当迁移涉及 SQL Server 或 Oracle 时才需要 ODBC 驱动：
  - SQL Server：msodbcsql18
  - Oracle：Oracle Instant Client + ODBC 驱动

其余依赖都已随包附带。如果目标机系统版本更新，可以删掉 lib/ 里对应的文件，
加载器会回退到系统库。

## 快速开始

    ./run.sh --help

    ./run.sh --mode mysql2pg \
      --source "db=app;host=10.0.0.5;port=3306;user=reader;password=***" \
      --target "host=10.0.0.9;port=5432;user=postgres;db=app_dst;password=***" \
      --workers 4 --batch 5000 --commit 10

密码可写成 `password=env:VAR` 或 `password=file:PATH`。

## 验证安装

    ./run.sh --mode sim --rows 200000 --tables 1 --exit

## 常见问题

| 现象 | 原因 |
|---|---|
| error while loading shared libraries | 目标机 glibc 低于构建机 |
| Can't open lib 'ODBC Driver 18 for SQL Server' | 驱动未安装或未注册到 odbcinst.ini |
| ORA-12541: no listener | 端口错误 / 监听未启动 |
| Lost connection to MySQL server during query | 服务端 net_read_timeout / net_write_timeout 太短 |
