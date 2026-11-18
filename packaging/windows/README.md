# Windows 原生构建

代码里的 POSIX 依赖已经做了平台抽象（`src/platform.h`）：

| 原实现 | Windows 替代 |
|---|---|
| `flock()` 单实例锁 | 命名互斥体（`CreateMutex`） |
| `getpid()` | `_getpid()` |
| `/tmp/mig-load-*` | `std::filesystem::temp_directory_path()` |
| `unistd.h` | 已按平台条件包含 |

## 一、安装工具链（一次性）

在 Windows 上用管理员 PowerShell：

```powershell
winget install MSYS2.MSYS2
```

或者从 https://www.msys2.org 下载安装包。

## 二、构建

打开开始菜单里的 **MSYS2 MINGW64**（注意不是 MSYS2 MSYS），执行：

```bash
cd /c/path/to/migrator-mmv
bash packaging/windows/build-msys2.sh
```

脚本会：

1. `pacman` 安装 gcc / cmake / make / postgresql / unixodbc / openssl / mariadb-connector-c
2. 为 `include/mysql/mysql.h` 补一个指向 mariadb 头文件的符号链接
3. `cmake -G "MinGW Makefiles"` 配置并编译
4. 产物汇总到 `out-win/`：`bin/migrator.exe`、`lib/*.dll`、`run.cmd`

运行：

```powershell
out-win\run.cmd --version
```

## 三、当前限制

| 功能 | Windows 状态 |
|---|---|
| 全量迁移 / 校验 / CDC | 支持 |
| 单实例锁（`--instance-lock`） | 已支持（命名互斥体） |
| 异步校验（`--verify-async`） | **不支持**，会自动退回同步校验 |
| 校验互斥锁 | **不支持**（多进程同时校验需自行避免） |

## 四、没有工具链时的替代方案

- WSL2：装 Ubuntu 22.04，用 Linux 包
- Docker Desktop：用 `mini-migrator:0.9.0` 镜像
