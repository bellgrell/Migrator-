# Migrator-
mini-migrator 是一款面向国产数据库的异构数据迁移工具，使用 C++17 实现，支持 MySQL 到 PostgreSQL / openGauss 的全量与实时增量迁移。 全量侧采用 COPY 协议，实测 1000 万行 9.32 秒（约 107 万行/秒）；支持表级策略（copy/rebuild/skip）、二级索引迁移、BLOB 二进制与视图转换，并提供行数加 checksum 双重校验，可检出单行篡改。 增量侧解析 MySQL binlog ROW 事件，实现增删改实时同步、binlog 轮转跟随与 DDL 变更同步，位点持久化支持断点续传。 配套 Web 监控面板与 Qt 桌面客户端，实时呈现进度、吞吐与事件流。
