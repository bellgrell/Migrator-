# mm-doctor — 迁移环境自检

mini-migrator 的配套工具：在正式迁移前检查源端与目标端环境，把"跑起来才发现"的问题提前暴露。

## 构建

```bash
make doctor        # 生成 build/mm-doctor
```

依赖与主程序一致：libpq（PostgreSQL/openGauss）、libmysqlclient（MySQL）。

## 用法

```bash
./build/mm-doctor \
  --source "host=127.0.0.1;port=3306;user=root;db=src"   --source-type mysql \
  --target "host=10.0.0.5;port=5432;user=migrator;db=dst" --target-type og
```

类型取值：`mysql` / `pg` / `og`。也可用 `--mode mysql2og` 自动推导。

## 检查项

| 类别 | 检查内容 | 失败/警告时的建议 |
|---|---|---|
| 连通性 | TCP 可达性与往返延迟 | 检查网络/防火墙/端口映射 |
| 认证 | 用户名、密码、库名 | 检查 pg_hba、密码、账号是否被锁 |
| DSN | 是否显式带 `db=`，实际连到哪个库 | 补上 db= 避免写入错库 |
| 版本 | 服务端版本 | 与构建时的客户端库匹配 |
| wal_level | 是否为 logical | `ALTER SYSTEM SET wal_level='logical'` 后重启 |
| 复制槽 | max_replication_slots、残留槽数量 | 调大到 ≥10；清理无用槽 |
| 插件 | 真实创建一次 wal2json 槽再删除 | 安装 wal2json 或改用 test_decoding |
| 权限 | superuser / SYSADMIN | 提升权限 |
| binlog | MySQL log_bin、binlog_format | 开启 binlog 并设为 ROW |
| 端口 | 面板 8123 是否被占用 | 换 --port 或停止占用进程 |

## 退出码

- `0`：全部通过（可能有警告）
- `2`：存在 FAIL，或参数不完整

可直接用于 CI / 部署脚本的前置门禁。

## 示例输出

```
mm-doctor  源=mysql  目标=og
----------------------------------------
[INFO] 目标 延迟                192.168.0.11:5432 13 ms
[ OK ] 认证                      root @mm_src
[ OK ] binlog                    ON
[ OK ] binlog_format             ROW
[ OK ] wal_level                 logical
[WARN] max_replication_slots     8
        修复: 调大到 >= 10
[ OK ] 权限                      SYSADMIN
[ OK ] wal2json                  可加载
[ OK ] 复制槽                    无残留槽
[ OK ] 面板端口                  8123 可用
----------------------------------------
通过 11 / 警告 1 / 失败 0
```

## 设计说明

- 插件检查会**真实建一次槽再删**，能同时验证 wal_level、插件可用性与权限，比查系统表更可靠。
- 不打印密码；支持 password=env:NAME / file:PATH，避免明文。
- 与主程序共用 Makefile 的编译参数，不引入新的依赖。
