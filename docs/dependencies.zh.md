# 依赖安装矩阵（dependencies）

**中文** | [English](dependencies.md)

本工具不打包 Oracle Instant Client、Microsoft ODBC Driver 等厂商组件，
只做“发现 + 配置 + 自检”。安装完成后用 \`migrator doctor\` 验证。

## 1. 组件清单

| 组件 | 用途 | 是否必需 |
|---|---|---|
| libmysqlclient（MySQL 8.0 客户端库） | MySQL 源/目标 | 需要 MySQL 方向时必需 |
| libpq（PostgreSQL 客户端库） | PG / openGauss 源/目标 | 需要 PG/OG 方向时必需 |
| unixODBC（libodbc2 + odbcinst） | ODBC 驱动管理 | 需要 MSSQL/Oracle 时必需 |
| ODBC Driver 18 for SQL Server | MSSQL 源/目标 | 需要 MSSQL 时必需 |
| Oracle Instant Client（Basic + ODBC + Tools） | Oracle 源/目标 | 需要 Oracle 时必需 |
| bcp（mssql-tools18） | MSSQL 目标批量装载 | 可选，缺失时回退 INSERT |
| sqlldr（Instant Client Tools） | Oracle 目标批量装载 | 可选，缺失时回退 INSERT ALL |

## 2. Ubuntu / Debian

\`\`\`bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc unixodbc-dev libssl-dev

# SQL Server ODBC 驱动（微软官方仓库）
curl -fsSL https://packages.microsoft.com/keys/microsoft.asc | sudo gpg --dearmor -o /usr/share/keyrings/microsoft-prod.gpg
echo "deb [arch=amd64 signed-by=/usr/share/keyrings/microsoft-prod.gpg] https://packages.microsoft.com/ubuntu/24.04/prod noble main" | sudo tee /etc/apt/sources.list.d/mssql-release.list
sudo apt-get update && sudo ACCEPT_EULA=Y apt-get install -y msodbcsql18 mssql-tools18
\`\`\`

Oracle Instant Client 在 <https://www.oracle.com/database/technologies/instant-client.html>
接受许可后下载 basic / odbc / tools 三个 zip，解压到同一目录即可，无需安装。

## 3. RHEL / CentOS / openEuler

\`\`\`bash
sudo dnf install -y gcc-c++ cmake ninja-build pkgconfig \
  mysql-devel postgresql-devel unixODBC unixODBC-devel openssl-devel
# msodbcsql18 与 mssql-tools18 用微软 RHEL 仓库
\`\`\`

## 4. macOS（开发环境）

\`\`\`bash
brew install mysql-client libpq unixodbc
brew install --cask oracle-instantclient   # 或手动解压 zip
\`\`\`

## 5. 无 root 的用户级安装（推荐给受限环境）

\`\`\`bash
mkdir -p ~/msodbc && cd ~/msodbc
curl -fsSLO https://packages.microsoft.com/ubuntu/24.04/prod/pool/main/m/msodbcsql18/msodbcsql18_18.6.2.1-1_amd64.deb
dpkg-deb -x msodbcsql18_18.6.2.1-1_amd64.deb root
# 驱动路径：~/msodbc/root/opt/microsoft/msodbcsql18/lib64/libmsodbcsql-18.6.so.2.1
\`\`\`

在 \`~/.odbc/odbcinst.ini\` 注册驱动，并导出：

\`\`\`bash
export ODBCSYSINI=$HOME/.odbc
export ODBCINI=$HOME/.odbc/odbc.ini
\`\`\`

## 6. 工具配置（config/providers.ini）

\`\`\`ini
mssql_driver=ODBC Driver 18 for SQL Server
oracle_driver=Oracle 23 ODBC Driver
bcp_path=
sqlldr_path=
\`\`\`

优先级：环境变量（MIG_MSSQL_DRIVER / MIG_ORACLE_DRIVER / MIG_BCP / MIG_SQLLDR）
> \`config/providers.ini\` > 自动探测（PATH、\`~/msodbc\`、\`~/oracle-ic\`）。

## 7. 自检

\`\`\`bash
./build/migrator doctor
\`\`\`

输出示例：

\`\`\`
[OK  ] providers.ini          config/providers.ini
[OK  ] mssql driver (cfg)     ODBC Driver 18 for SQL Server
[OK  ] oracle driver (cfg)    Oracle 23 ODBC Driver
[OK  ] mysql client           8.0.46
[OK  ] libpq                  160015
[OK  ] odbc drivers           Oracle 23 ODBC Driver, ODBC Driver 18 for SQL Server
[OK  ] bcp                    .../bcp | BCP - Bulk Copy Program for Microsoft SQL Server.
[OK  ] sqlldr                 .../sqlldr | SQL*Loader: Release 23.26.2.0.0
13 checks: 13 ok, 0 warn, 0 fail
\`\`\`

## 8. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| \`Can't open lib 'FreeTDS'\` | 未指定 driver，回退到 FreeTDS | DSN 加 \`driver=ODBC Driver 18 for SQL Server\`，或写 providers.ini |
| \`ORA-01017 invalid credential\` | 密码/服务名错误 | 检查 DSN 的 \`user/password/db=FREEPDB1\` |
| \`bcp: Unable to locate driver dynamic library\` | 缺 ODBCSYSINI 或驱动未注册 | \`export ODBCSYSINI=~/.odbc\` |
| \`sqlldr: libclntsh.so.23.1: cannot open\` | 缺 LD_LIBRARY_PATH | \`export LD_LIBRARY_PATH=~/oracle-ic/instantclient_23_26\` |
| \`resume module not built\` | 可执行目标缺编译宏 | 已在 CMake 修复；旧包请重编 |
| \`migration finished: 0 rows\` 但 exit 0 | 旧版本无行数校验 | 升级到含 doctor/强校验的版本 |
