#include <chrono>
#include <cstdio>
#include <filesystem>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "cdc.h"
#include "http.h"
#include "platform.h"
#include "migrator.h"
#include "type_system.h"
#include "config_validator.h"
#include "module.h"

namespace {

void print_features() {
  std::printf("mini-migrator modules:\n");
  std::printf("  core        on\n");
  for (const auto* mod : mig::ModuleManager::instance().all())
    std::printf("  %-11s %s\n", mod->name().c_str(),
                mod->compiled() ? "on" : "off");
}

void usage() {
  std::printf(
      "mini-migrator - 数据库迁移工具\n"
      "\n"
      "用法:\n"
      "  ./build/migrator --mode sim --rows 600000 --tables 6 --port 8123\n"
      "  ./build/migrator --mode mysql2pg --source \"host=127.0.0.1;port=3306;user=root;db=demo\" \\\n"
      "                   --target \"host=127.0.0.1;port=5432;user=postgres;db=demo_dst\"\n"
      "\n"
      "迁移:\n"
      "  --mode <sim|mysql2pg|mysql2og|pg2mysql|pg2og|og2pg|og2mysql|mysql2mysql>\n"
      "                                 运行模式（默认 sim）\n"
      "  --source-type <mysql|pg|og>   覆盖源类型\n"
      "  --target-type <mysql|pg|og>   覆盖目标类型\n"
      "  --source <dsn>                源库 DSN\n"
      "  --target <dsn>                目标库 DSN\n"
      "  --batch <N>                   每批行数（默认 1000）\n"
      "  --commit <N>                  每 N 批提交（默认 10）\n"
      "  --workers <N>                 并行线程数（默认 1；大表单表按主键区间并行）\n"
      "  --pk-min-rows <N>             触发表内主键并行的最小行数（默认 1000000）\n"
      "  --no-pk-parallel              关闭单表主键区间并行\n"
      "  --target-dialect <pg|og>      目标方言（默认按 mode 推断）\n"
      "  --log-every <N>               每 N 批记一条日志（默认 10）\n"
      "  --out-dir <path>              日志目录（默认 out）\n"
      "  --throttle <ms>               每批休眠毫秒（默认 0）\n"
      "  --load-mode <auto|insert|ignore|replace> MySQL 目标 LOAD DATA 模式（默认 auto）\n"
      "  --io-scheduler <off|fair|scan> 批 I/O 调度策略（默认 fair）\n"
      "  --io-queue-depth <N>          并发读批次数（默认 0 = workers）\n"
      "  --io-prefetch <N>             每个区间预取批数（默认 2）\n"
      "  --io-window <N>               SCAN 策略的区间窗口（默认自动）\n"
      "  --type-map <path>             类型映射（默认 config/type_map.json）\n"
      "  --tables-config <path>        表级策略（copy/rebuild/skip）\n"
      "  --no-index                    不迁移二级索引\n"
      "  --no-analyze                  不执行 ANALYZE\n"
      "  --no-copy                     回退 INSERT 模式\n"
      "  --with-views                  迁移视图\n"
      "\n"
      "校验:\n"
      "  --no-checksum                 只比行数\n"
      "  --verify-engine fnv|mmv       校验引擎（默认 mmv；fnv=兼容回退）\n"
      "  --verify-async                迁移后异步全量校验（不阻塞退出）\n"
      "  --verify-async-threshold <N>  行数达到 N 时自动异步（默认 10000000）\n"
      "  --verify-wait                 异步校验锁被占用时等待\n"
      "  --explain-types               只打印每列的类型规则、损失类别和边界\n"
      "  --use-type-rules              用类型规则生成目标 DDL（回退 JSON 映射）\n"
      "  --check-type-rules            检查类型规则冲突/覆盖\n"
      "  --prove-type-rules            形式化不变量检查（P1..P8）\n"
      "  --dump-type-tests             输出规则边界测试表（TSV）\n"
      "  --type-rules <file>           加载外部类型规则（TSV 或 JSON）\n"
      "  --export-type-rules <file>    导出当前规则到 TSV/JSON\n"
      "  --emit-type-tests <dir>       生成规则边界测试清单\n"
      "  --checksum-batch-bytes <N>    校验批次字节预算（默认 2097152）\n"
      "  --no-verify                   跳过校验\n"
      "  --connect-timeout <sec>       连接/读写超时（默认 10）\n"
      "  --connect-retries <N>         连接失败重试次数（默认 3）\n"
      "  --truncate                    迁移前 DROP 目标表，便于重跑\n"
      "  --idempotent                  重放安全：已存在的行跳过，不重复写入\n"
      "  --skip-row-count              跳过进度用的全表 COUNT(*)（超大表）\n"
      "  --consistent-snapshot         MySQL 一致性快照（单连接，自动 workers=1）\n"
      "  --resume                      按主键水位断点续传\n"
      "  --resume-dir <path>           断点文件目录（默认 out/resume）\n"
      "  --repair                      校验后按主键补/删差异\n"
      "  --sslmode <mode>              PostgreSQL/openGauss sslmode（默认 prefer）\n"
      "  --mysql-ssl-mode <mode>       MySQL SSL 模式：DISABLED/PREFERRED/REQUIRED\n"
      "  --cdc-retries <N>             CDC 断线重连次数（默认 10）\n"
      "  --cdc-slot <name>             PG CDC 逻辑复制槽名（默认 migrator_slot）\n"
      "  --cdc-until <file:pos>        MySQL CDC 追到该位点后停止（割接）\n"
      "  --cdc-until-lsn <lsn>         PG CDC 追到该 LSN 后停止（割接）\n"
      "  --verify-only                 只校验不迁移\n"
      "\n"
      "CDC 增量:\n"
      "  --cdc                         迁移后持续增量\n"
      "  --cdc-only                    只跑增量\n"
      "  --cdc-ddl                     同步 DDL\n"
      "  --cdc-start <file:pos>        起始位点\n"
      "  --cdc-server-id <N>           复制客户端 server_id\n"
      "  --cdc-max-events <N>          应用 N 个事件后退出（0=不限）\n"
      "  --cdc-setup                   开跑前启用源端日志（MSSQL sp_cdc_enable_* / Oracle 补充日志）\n"
      "  --cdc-poll-ms <N>             ODBC CDC 无事件时的轮询间隔（默认 1000）\n"
      "\n"
      "面板安全与运维:\n"
      "  --http-bind <addr>            面板绑定地址（默认 127.0.0.1）\n"
      "  --http-token <token>          面板 Bearer Token（非回环绑定必填）\n"
      "  --http-token-file <path>      从文件首行读取 Token\n"
      "  --http-audit <path>           审计日志路径（默认 out/audit.log）\n"
      "  --no-http-audit               关闭审计日志（不推荐）\n"
      "  --http-allow-insecure         显式允许远程明文/无认证面板\n"
      "  --http-cors                   允许跨域（默认关闭）\n"
      "  --tls-cert <pem>              面板 TLS 证书链（需要 OpenSSL）\n"
      "  --tls-key <pem>               面板 TLS 私钥\n"
      "  --instance-lock <path>        单实例锁，防止 HA 切换时双跑\n"
      "  --version                     显示版本\n"
      "\n"
      "其他:\n"
      "  --rows <N>                    模拟总行数（默认 200000）\n"
      "  --tables <N>                  模拟表数（默认 4）\n"
      "  --port <N>                    面板端口（默认 8123）\n"
      "  --only-tables <a,b,c>         只迁移指定表（逗号分隔）\n"
      "  --exclude-tables <a,b,c>      排除指定表\n"
      "  --disable-modules <a,b,c>     运行时禁用模块（pg_cdc/repair/metrics/snapshot/resume）\n"
      "  --modules-config <path>       模块配置文件（默认 config/modules.json）\n"
      "  --exit                        迁移完退出\n"
      "  --features                    显示编译进来的模块\n"
      "  -h, --help                    显示帮助\n");
}

std::vector<std::string> split_list(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    const size_t c = s.find(',', p);
    std::string item = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) item.pop_back();
    const size_t dot = item.find('.');
    if (dot != std::string::npos) item = item.substr(dot + 1);
    if (!item.empty()) out.push_back(item);
    if (c == std::string::npos) break;
    p = c + 1;
  }
  return out;
}

std::string read_secret_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) return std::string();
  std::string line;
  std::getline(in, line);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                           line.back() == '\t'))
    line.pop_back();
  size_t b = line.find_first_not_of(" \t");
  if (b == std::string::npos) return std::string();
  return line.substr(b);
}

// flock-based single-instance guard. Returns true when the lock is held (or
// no path was requested); fills err on contention/failure.
bool acquire_instance_lock(const std::string& path, int& fd, std::string& err) {
  fd = -1;
  if (path.empty()) return true;
#if defined(_WIN32)
  static mig::InstanceLock win_lock;
  if (!win_lock.acquire(path, err)) return false;
  return true;
#else
  std::error_code ec;
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) std::filesystem::create_directories(parent, ec);
  fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
  if (fd < 0) {
    err = "cannot open instance lock " + path;
    return false;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    fd = -1;
    err = "another migrator instance already holds " + path;
    return false;
  }
  if (::ftruncate(fd, 0) == 0) {
    const std::string pid = std::to_string((long long)mig::mig_pid()) + "\n";
    (void)!::write(fd, pid.data(), pid.size());
  }
  return true;
#endif
}

std::string fmt_num(int64_t v) {
  std::string s = std::to_string(v);
  std::string out;
  int c = 0;
  for (int i = (int)s.size() - 1; i >= 0; --i) {
    out.push_back(s[(size_t)i]);
    if (++c % 3 == 0 && i > 0) out.push_back(',');
  }
  return std::string(out.rbegin(), out.rend());
}

}  // namespace

int main(int argc, char** argv) {
  mig::Options o;
  mig::load_verify_config("config/verify.json", o);
  bool exit_after = false;
  bool verify_only = false;
  bool cdc_only = false;
  bool type_map_explicit = false;
  bool verify_engine_explicit = false;
  std::string source_type_flag, target_type_flag;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
    };
    if (a == "--mode") o.mode = next();
    else if (a == "--source") o.source = next();
    else if (a == "--target") o.target = next();
    else if (a == "--rows") o.sim_rows = std::stoll(next());
    else if (a == "--tables") o.sim_tables = std::stoi(next());
    else if (a == "--batch") o.batch_size = std::stoi(next());
    else if (a == "--commit") o.commit_every = std::stoi(next());
    else if (a == "--workers") o.workers = std::stoi(next());
    else if (a == "--pk-min-rows") o.pk_min_rows = std::stoi(next());
    else if (a == "--no-pk-parallel") o.pk_parallel = false;
    else if (a == "--source-type") source_type_flag = next();
    else if (a == "--target-type") target_type_flag = next();
    else if (a == "--target-dialect") {
      const std::string d = next();
      if (d == "opengauss" || d == "og") o.opengauss = true;
      else if (d == "postgres" || d == "pg") o.opengauss = false;
    }
    else if (a == "--log-every") o.log_every = std::stoi(next());
    else if (a == "--out-dir") o.out_dir = next();
    else if (a == "--throttle") o.throttle_ms = std::stoi(next());
    else if (a == "--load-mode") o.load_mode = next();
    else if (a == "--io-scheduler") o.io_scheduler = next();
    else if (a == "--io-queue-depth") o.io_queue_depth = std::stoi(next());
    else if (a == "--io-prefetch") o.io_prefetch = std::stoi(next());
    else if (a == "--io-window") o.io_window = std::stoi(next());
    else if (a == "--port") o.http_port = std::stoi(next());
    else if (a == "--http-bind") o.http_bind = next();
    else if (a == "--http-token") o.http_token = next();
    else if (a == "--http-token-file") o.http_token_file = next();
    else if (a == "--http-audit") o.http_audit = next();
    else if (a == "--no-http-audit") o.http_audit.clear();
    else if (a == "--http-allow-insecure") o.http_insecure = true;
    else if (a == "--http-cors") o.http_cors = true;
    else if (a == "--tls-cert") o.tls_cert = next();
    else if (a == "--tls-key") o.tls_key = next();
    else if (a == "--instance-lock") o.instance_lock = next();
    else if (a == "--version") {
      std::printf("mini-migrator %s\n", MIG_VERSION);
      return 0;
    }
    else if (a == "--type-map") { o.type_map_path = next(); type_map_explicit = true; }
    else if (a == "--tables-config") o.table_policy_path = next();
    else if (a == "--no-index") o.build_indexes = false;
    else if (a == "--no-analyze") o.analyze_after = false;
    else if (a == "--no-copy") o.use_copy = false;
    else if (a == "--no-checksum") o.checksum = false;
    else if (a == "--verify-engine") { o.verify_engine = next(); verify_engine_explicit = true; }
    else if (a == "--verify-async") o.verify_async = true;
    else if (a == "--verify-async-threshold") o.verify_async_threshold = std::stoll(next());
    else if (a == "--verify-wait") o.verify_wait = true;
    else if (a == "--explain-types") o.explain_types = true;
    else if (a == "--use-type-rules") o.use_type_rules = true;
    else if (a == "--check-type-rules") o.check_type_rules = true;
    else if (a == "--prove-type-rules") o.prove_type_rules = true;
    else if (a == "--dump-type-tests") o.dump_type_tests = true;
    else if (a == "--type-rules") o.type_rules_path = next();
    else if (a == "--export-type-rules") o.export_type_rules = next();
    else if (a == "--emit-type-tests") o.emit_type_tests_dir = next();
    else if (a == "--verify-status-file") o.verify_status_file = next();
    else if (a == "--checksum-batch-bytes") o.checksum_batch_bytes = std::stoi(next());
    else if (a == "--checksum-max-rows") o.checksum_max_rows = (std::size_t)std::stoll(next());
    else if (a == "--verify-only") verify_only = true;
    else if (a == "--with-views") o.with_views = true;
    else if (a == "--cdc") o.cdc = true;
    else if (a == "--cdc-only") { o.cdc = true; cdc_only = true; }
    else if (a == "--cdc-ddl") o.cdc_ddl = true;
    else if (a == "--cdc-start") o.cdc_start = next();
    else if (a == "--cdc-drop-slot") o.cdc_drop_slot = true;
    else if (a == "--cdc-server-id") o.cdc_server_id = std::stoi(next());
    else if (a == "--cdc-max-events") o.cdc_max_events = std::stoi(next());
    else if (a == "--no-verify") o.do_verify = false;
    else if (a == "--connect-timeout") o.connect_timeout = std::stoi(next());
    else if (a == "--connect-retries") o.connect_retries = std::stoi(next());
    else if (a == "--truncate") o.truncate_target = true;
    else if (a == "--idempotent") o.idempotent = true;
    else if (a == "--skip-row-count") o.skip_row_count = true;
    else if (a == "--consistent-snapshot") { o.consistent_snapshot = true; o.workers = 1; o.pk_parallel = false; }
    else if (a == "--resume") { o.resume = true; o.pk_parallel = false; }
    else if (a == "--resume-dir") o.resume_dir = next();
    else if (a == "--repair") o.repair = true;
    else if (a == "--sslmode") o.sslmode = next();
    else if (a == "--mysql-ssl-mode") o.mysql_ssl_mode = next();
    else if (a == "--cdc-retries") o.cdc_retries = std::stoi(next());
    else if (a == "--cdc-slot") o.cdc_slot = next();
    else if (a == "--cdc-until") o.cdc_until = next();
    else if (a == "--cdc-until-lsn") o.cdc_until_lsn = next();
    else if (a == "--cdc-setup") o.cdc_setup = true;
    else if (a == "--cdc-poll-ms") o.cdc_poll_ms = std::stoi(next());
    else if (a == "--only-tables") o.only_tables = split_list(next());
    else if (a == "--exclude-tables") o.exclude_tables = split_list(next());
    else if (a == "--disable-modules") o.disabled_modules = split_list(next());
    else if (a == "--modules-config") o.modules_config_path = next();
    else if (a == "--exit") exit_after = true;
    else if (a == "--features") { print_features(); return 0; }
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); return 2; }
  }

  if (!o.modules_config_path.empty()) load_modules_config(o.modules_config_path, o);

  mig::Status vst = mig::validate_options(o);
  if (!vst.ok()) {
    std::fprintf(stderr, "[fatal] %s\n", vst.str().c_str());
    return 2;
  }

  // Dashboard credentials: --http-token-file > --http-token > env.
  if (!o.http_token_file.empty()) {
    const std::string t = read_secret_file(o.http_token_file);
    if (t.empty()) {
      std::fprintf(stderr, "[fatal] cannot read dashboard token from %s\n",
                   o.http_token_file.c_str());
      return 2;
    }
    o.http_token = t;
  }
  if (o.http_token.empty()) {
    const char* env = std::getenv("MIGRATOR_HTTP_TOKEN");
    if (env && *env) o.http_token = env;
  }

  // HA guard: refuse to run twice against the same state directory.
  int instance_lock_fd = -1;
  {
    std::string lerr;
    if (!acquire_instance_lock(o.instance_lock, instance_lock_fd, lerr)) {
      std::fprintf(stderr, "[fatal] %s\n", lerr.c_str());
      return 2;
    }
  }

  // Dashboard pre-flight: refuse an unsafe bind before a long migration starts.
  mig::HttpOptions hopts;
  hopts.port = o.http_port;
  hopts.bind = o.http_bind;
  hopts.token = o.http_token;
  hopts.audit_path = o.http_audit;
  hopts.tls_cert = o.tls_cert;
  hopts.tls_key = o.tls_key;
  hopts.allow_insecure = o.http_insecure;
  hopts.cors = o.http_cors;
  if (!exit_after) {
    std::string herr;
    const int hrc = mig::validate_http_options(hopts, herr);
    if (hrc != 0) {
      std::fprintf(stderr, "[fatal] %s\n", herr.c_str());
      return 2;
    }
  }

  if (o.consistent_snapshot && !o.module_enabled("snapshot")) {
    std::fprintf(stderr, "[fatal] snapshot module disabled\n");
    return 2;
  }
  if (o.resume && !o.module_enabled("resume")) {
    std::fprintf(stderr, "[fatal] resume module disabled\n");
    return 2;
  }
  if (o.repair && !o.module_enabled("repair")) {
    std::fprintf(stderr, "[fatal] repair module disabled\n");
    return 2;
  }

#ifndef MIG_ENABLE_SNAPSHOT
  if (o.consistent_snapshot) {
    std::fprintf(stderr, "[fatal] consistent snapshot module not built\n");
    return 2;
  }
#endif
#ifndef MIG_ENABLE_RESUME
  if (o.resume) {
    std::fprintf(stderr, "[fatal] resume module not built\n");
    return 2;
  }
#endif
#ifndef MIG_ENABLE_REPAIR
  if (o.repair) {
    std::fprintf(stderr, "[fatal] repair module not built\n");
    return 2;
  }
#endif

  if (o.resume && o.truncate_target) {
    std::fprintf(stderr, "[warn] --resume and --truncate are mutually exclusive; resume disabled\n");
    o.resume = false;
  }

  if (!o.export_type_rules.empty()) {
    mig::RuleSet rs;
    std::string err;
    if (!rs.save_file(o.export_type_rules, err)) {
      std::fprintf(stderr, "[fatal] %s\n", err.c_str());
      return 1;
    }
    std::printf("type rules exported: %s (%s)\n", o.export_type_rules.c_str(),
                rs.stats().c_str());
    return 0;
  }
  if (!o.emit_type_tests_dir.empty()) {
    mig::RuleSet rs;
    if (!o.type_rules_path.empty()) {
      std::string err;
      if (!rs.load_file(o.type_rules_path, err)) {
        std::fprintf(stderr, "[fatal] %s\n", err.c_str());
        return 1;
      }
    }
    std::string err;
    if (!rs.emit_test_manifest(o.emit_type_tests_dir, err)) {
      std::fprintf(stderr, "[fatal] %s\n", err.c_str());
      return 1;
    }
    std::printf("type test manifest: %s (%s)\n", o.emit_type_tests_dir.c_str(),
                rs.stats().c_str());
    return 0;
  }
  if (o.check_type_rules || o.dump_type_tests || o.prove_type_rules) {
    mig::RuleSet rs;
    if (!o.type_rules_path.empty()) {
      std::string err;
      if (!rs.load_file(o.type_rules_path, err)) {
        std::fprintf(stderr, "[fatal] %s\n", err.c_str());
        return 1;
      }
    }
    if (o.dump_type_tests) {
      for (const auto& line : rs.dump_tests()) std::printf("%s\n", line.c_str());
      return 0;
    }
    if (o.prove_type_rules) {
      int failed = 0, warned = 0, passed = 0;
      for (const auto& line : rs.prove()) {
        std::printf("%s\n", line.c_str());
        if (line.rfind("FAIL ", 0) == 0) ++failed;
        else if (line.rfind("WARN ", 0) == 0) ++warned;
        else ++passed;
      }
      std::printf("type rule proof: %s (passed=%d failed=%d warned=%d, %s)\n",
                  failed == 0 ? "ok" : "FAILED", passed, failed, warned,
                  rs.stats().c_str());
      return failed == 0 ? 0 : 1;
    }
    const auto issues = rs.validate();
    for (const auto& s : issues) std::printf("[issue] %s\n", s.c_str());
    std::printf("type rules: %s (%s)\n", issues.empty() ? "ok" : "issues found",
                rs.stats().c_str());
    return issues.empty() ? 0 : 1;
  }

  struct ModeSpec { const char* mode; const char* s; const char* t; };
  static const ModeSpec kModes[] = {
      {"sim", "sim", "sim"},          {"mysql2pg", "mysql", "pg"},
      {"mysql2og", "mysql", "og"},    {"mysql2opengauss", "mysql", "og"},
      {"pg2mysql", "pg", "mysql"},    {"pg2og", "pg", "og"},
      {"og2pg", "og", "pg"},          {"og2mysql", "og", "mysql"},
      {"mysql2mysql", "mysql", "mysql"},
      {"mysql2mssql", "mysql", "mssql"}, {"mssql2mysql", "mssql", "mysql"},
      {"mssql2mssql", "mssql", "mssql"},
      {"mysql2oracle", "mysql", "oracle"}, {"oracle2mysql", "oracle", "mysql"},
      {"oracle2oracle", "oracle", "oracle"},
      {"mysql", "mysql", ""},         {"postgres", "", "pg"},
      {"opengauss", "", "og"},         {"mssql", "mssql", ""},
      {"oracle", "oracle", ""},
  };
  for (const auto& ms : kModes) {
    if (o.mode == ms.mode) { o.source_kind = ms.s; o.target_kind = ms.t; break; }
  }
  if (o.source_kind.empty() && source_type_flag.empty()) {
    std::fprintf(stderr, "[fatal] unknown mode: %s\n", o.mode.c_str());
    return 2;
  }
  if (!source_type_flag.empty()) o.source_kind = source_type_flag;
  if (!target_type_flag.empty()) o.target_kind = target_type_flag;
  if (o.target_kind == "og") o.opengauss = true;
  if (o.source_kind == "pg" || o.source_kind == "og")
    o.pg_source_uses_source = true;

  // openGauss lacks sha256(bytea) and its JSON functions differ; the SQL
  // digest path is both slow and fragile. Client-side FNV is the reliable
  // default, while --verify-engine mmv remains available for experimentation.
  if (!verify_engine_explicit &&
      (o.source_kind == "og" || o.target_kind == "og")) {
    o.verify_engine = "fnv";
    std::fprintf(stderr,
                 "[warn] openGauss: using fnv verification "
                 "(mmv SQL digest is not openGauss-compatible)\n");
  }
  if (!verify_engine_explicit &&
      (o.source_kind == "mssql" || o.source_kind == "oracle" ||
       o.target_kind == "mssql" || o.target_kind == "oracle")) {
    o.verify_engine = "fnv";
    std::fprintf(stderr,
                 "[warn] ODBC backend: using fnv verification "
                 "(no mmv dialect support)\n");
  }
  if (o.cdc && o.source_kind != "mysql" && o.source_kind != "pg" &&
      o.source_kind != "og" && o.source_kind != "mssql" &&
      o.source_kind != "oracle") {
    std::fprintf(stderr,
                 "[fatal] CDC supports MySQL/PostgreSQL/openGauss/SQL Server/"
                 "Oracle sources only (source=%s)\n",
                 o.source_kind.c_str());
    return 2;
  }

  // L-05：全量 + 增量模式丢弃上次遗留的位点，避免重放旧事件。
  if (o.cdc && !cdc_only && o.cdc_start.empty()) {
    std::remove(o.cdc_position_path.c_str());
    std::remove(o.pg_cdc_position_path.c_str());
    std::remove(o.odbc_cdc_position_path.c_str());
  }

  if (!type_map_explicit && (o.source_kind == "pg" || o.source_kind == "og")) {
    o.type_map_path = (o.target_kind == "mysql")
                          ? "config/type_map.pg2mysql.json"
                          : "config/type_map.pg.json";
  }
  if (!type_map_explicit && o.source_kind == "mysql") {
    o.type_map_path = (o.target_kind == "mysql")
                          ? "config/type_map.mysql.json"
                          : "config/type_map.mysql2pg.json";
  }
  if (!type_map_explicit && o.source_kind == "mssql" &&
      o.target_kind == "mysql")
    o.type_map_path = "config/type_map.mssql2mysql.json";
  if (!type_map_explicit && o.source_kind == "oracle" &&
      o.target_kind == "mysql")
    o.type_map_path = "config/type_map.oracle2mysql.json";

  mig::TypeMap tm;
  mig::Status st = tm.load(o.type_map_path);
  if (!st.ok()) {
    std::fprintf(stderr, "[fatal] type map: %s\n", st.str().c_str());
    return 1;
  }

  try {
    o.exe_path = std::filesystem::absolute(argv[0]).string();
  } catch (...) {
    o.exe_path = argv[0];
  }

  std::error_code lec;
  std::filesystem::create_directories(o.out_dir, lec);
  int verify_lock_fd = -1;
  if (o.verify_status_file.empty()) {
    std::string lerr;
    verify_lock_fd = mig::acquire_verify_lock(
        o.out_dir + "/verify-async.lock", o.verify_wait, lerr);
    if (verify_lock_fd < 0) {
      std::fprintf(stderr, "[fatal] %s\n", lerr.c_str());
      return 1;
    }
  }

  mig::Status s1;
  auto src = mig::make_source(o, s1);
  if (!src) {
    std::fprintf(stderr, "[fatal] source: %s\n", s1.str().c_str());
    return 1;
  }
  mig::Status s2;
  auto tgt = mig::make_target(o, s2);
  if (!tgt) {
    std::fprintf(stderr, "[fatal] target: %s\n", s2.str().c_str());
    return 1;
  }

  if (o.explain_types) {
    mig::Status ec = src->connect();
    if (!ec.ok()) {
      std::fprintf(stderr, "[fatal] source: %s\n", ec.str().c_str());
      return 1;
    }
    std::vector<std::string> tables = o.only_tables;
    if (tables.empty() && !src->list_tables(tables).ok()) {
      std::fprintf(stderr, "[fatal] list_tables failed\n");
      return 1;
    }
    mig::RuleSet rules;
    if (!o.type_rules_path.empty()) {
      std::string err;
      if (!rules.load_file(o.type_rules_path, err)) {
        std::fprintf(stderr, "[fatal] %s\n", err.c_str());
        return 1;
      }
    }
    const std::string sd = (o.source_kind == "og") ? "pg" : o.source_kind;
    const std::string td = (o.target_kind == "og") ? "pg" : o.target_kind;
    int exact = 0, range = 0, precision = 0, repr = 0, unsup = 0;
    for (const auto& tn : tables) {
      mig::TableSchema ts;
      if (!src->read_schema(tn, ts).ok()) {
        std::fprintf(stderr, "[warn] read_schema %s failed\n", tn.c_str());
        continue;
      }
      for (const auto& c : ts.columns) {
        const auto r = rules.resolve(sd, c.src_type, td);
        std::printf("%s.%s  %s -> %s  rule=%s loss=%s", tn.c_str(), c.name.c_str(),
                    c.src_type.c_str(),
                    r.ok ? mig::RuleSet::type_name(r.dst).c_str() : "?",
                    r.rule_id.c_str(), mig::RuleSet::loss_name(r.loss));
        if (!r.guard.empty()) std::printf(" guard={%s}", r.guard.c_str());
        if (!r.witness.empty()) std::printf(" witness={%s}", r.witness.c_str());
        if (!r.verification.empty()) std::printf(" verify={%s}", r.verification.c_str());
        if (!r.note.empty()) std::printf(" note={%s}", r.note.c_str());
        std::printf("\n");
        switch (r.loss) {
          case mig::LossClass::kExact: ++exact; break;
          case mig::LossClass::kRangeLossless: ++range; break;
          case mig::LossClass::kPrecisionLoss: ++precision; break;
          case mig::LossClass::kRepresentationOnly: ++repr; break;
          default: ++unsup; break;
        }
      }
    }
    std::printf("type rules: exact=%d range_lossless=%d precision_loss=%d representation_only=%d unsupported=%d\n",
                exact, range, precision, repr, unsup);
    return 0;
  }

  auto* mheap = new mig::Metrics();
  mig::Metrics& m = *mheap;
  m.mode = o.mode;
  m.metrics_enabled = o.module_enabled("metrics");

  std::printf("========================================\n");
  std::printf(" mini-migrator\n");
  std::printf(" mode    : %s\n", o.mode.c_str());
  std::printf(" batch   : %d\n", o.batch_size);
  std::printf(" rows    : %lld\n", (long long)o.sim_rows);
  std::printf(" tables  : %d\n", o.sim_tables);
  std::printf(" workers : %d\n", o.workers);
  const char* panel_scheme = o.tls_cert.empty() ? "http" : "https";
  std::printf(" panel   : %s://%s:%d/ (auth=%s tls=%s)\n", panel_scheme,
              o.http_bind.c_str(), o.http_port,
              o.http_token.empty() ? "off" : "on",
              o.tls_cert.empty() ? "off" : "on");
  std::printf(" log dir : %s\n", o.out_dir.c_str());
  std::printf("========================================\n");
  std::fflush(stdout);

  if (verify_only) {
    m.started_at = mig::now_sec();
    src->connect();
    tgt->connect();
    mig::Status vr = mig::run_verify(o, *src, *tgt, m);
    if (o.repair) {
      mig::Status rr = mig::run_repair(o, *src, *tgt, m);
      if (!rr.ok()) vr = rr;
      else vr = mig::run_verify(o, *src, *tgt, m);
    }
    if (!o.verify_status_file.empty()) {
      std::string tl;
      {
        std::lock_guard<std::mutex> lk(m.mu);
        for (const auto& t : m.tables) {
          if (!t) continue;
          if (!tl.empty()) tl += ",";
          tl += t->name;
        }
      }
      if (tl.empty()) {
        for (const auto& t : o.only_tables) {
          if (!tl.empty()) tl += ",";
          tl += t;
        }
      }
#ifndef _WIN32
      const int64_t pid = (int64_t)mig::mig_pid();
#else
      const int64_t pid = 0;
#endif
      mig::write_verify_status(o.verify_status_file, vr.ok() ? "passed" : "failed",
                               pid, vr.ok() ? 0 : 1, vr.ok() ? "" : vr.str(), tl);
    }
    mig::write_log_file(m, o.out_dir);
    std::printf("\n verify-only: %s\n", vr.ok() ? "PASS" : "FAIL");
    std::fflush(stdout);
    return vr.ok() ? 0 : 1;
  }

  // L-02：--exit 模式下不启动面板线程，避免占用端口与阻塞退出。
  if (!exit_after) {
    std::thread http_thread([hopts, &m]() {
      const int rc = mig::run_http_server(hopts, m);
      if (rc != 0) std::fprintf(stderr, "[warn] http server failed, code=%d\n", rc);
    });
    http_thread.detach();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  auto t0 = std::chrono::steady_clock::now();
  bool progress_stop = false;
  std::thread progress([&]() {
    while (!progress_stop) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (progress_stop) break;
      const int64_t done = m.rows_done.load();
      const int64_t total = m.rows_total.load();
      const double pct = total > 0 ? 100.0 * (double)done / (double)total : 0.0;
      std::printf("[%6.1f%%] %10s / %10s rows   errors=%lld\n", pct,
                  fmt_num(done).c_str(), fmt_num(total).c_str(),
                  (long long)m.errors.load());
      std::fflush(stdout);
      m.sample();
    }
  });

  mig::Status rc = mig::Status::OK();
  if (!cdc_only) {
    rc = mig::run_migration(o, tm, *src, *tgt, m);
  }
  if (!cdc_only && rc.ok() && o.do_verify) {
    bool big = false;
    {
      std::lock_guard<std::mutex> lk(m.mu);
      for (const auto& t : m.tables) {
        if (t && t->total >= o.verify_async_threshold) { big = true; break; }
      }
    }
    const bool async = (o.verify_async || big) && !o.repair;
    if (async) {
      mig::Status as = mig::spawn_async_verify(o, m, verify_lock_fd);
      if (as.ok()) verify_lock_fd = -1;
      if (!as.ok()) {
        m.log("[verify] async spawn failed: " + as.str() + "; falling back to sync");
        mig::Status vr = mig::run_verify(o, *src, *tgt, m);
        if (!vr.ok()) rc = vr;
      }
    } else {
      mig::Status vr = mig::run_verify(o, *src, *tgt, m);
      if (!vr.ok()) rc = vr;
    }
  }

  if (!cdc_only && o.repair) {
    mig::Status rr = mig::run_repair(o, *src, *tgt, m);
    if (!rr.ok()) {
      rc = rr;
    } else if (o.do_verify) {
      mig::Status vr = mig::run_verify(o, *src, *tgt, m);
      if (!vr.ok()) rc = vr;
    }
  }

  if (!cdc_only) src->end_snapshot();

  if (rc.ok() && o.cdc) {
    // openGauss 与 PostgreSQL 共用逻辑复制路径（同一个 PgSource 实现）
    const bool pg_like = (o.source_kind == "pg" || o.source_kind == "og");
    const bool odbc_like =
        (o.source_kind == "mssql" || o.source_kind == "oracle");
    mig::Status cr =
        odbc_like ? mig::run_odbc_cdc(o, m, o.cdc_max_events)
                  : (pg_like ? mig::run_pg_cdc(o, m, o.cdc_max_events)
                             : mig::run_cdc(o, m, o.cdc_max_events));
    if (!cr.ok()) {
      std::fprintf(stderr, "[cdc] %s\n", cr.str().c_str());
      rc = cr;
    }
  }

  mig::write_log_file(m, o.out_dir);

  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  progress_stop = true;
  progress.join();

  std::printf("\n========================================\n");
  std::printf(" rows   : %s\n", fmt_num(m.rows_done.load()).c_str());
  std::printf(" errors : %lld\n", (long long)m.errors.load());
  std::printf(" time   : %.2f s\n", secs);
  std::printf(" speed  : %.0f rows/s\n",
              secs > 0 ? (double)m.rows_done.load() / secs : 0.0);
  std::printf(" verify : %s\n",
              m.verified.load()
                  ? (m.verify_mismatch.load() == 0 ? "PASS" : "FAIL")
                  : "skipped");
  std::printf(" panel  : %s://%s:%d/\n", panel_scheme, o.http_bind.c_str(),
              o.http_port);
  std::printf("========================================\n");
  std::fflush(stdout);

  if (!rc.ok()) std::fprintf(stderr, "[error] %s\n", rc.str().c_str());

  if (exit_after) {
    std::fflush(nullptr);
    std::_Exit(rc.ok() ? 0 : 1);
  }

  std::printf("\n面板仍在运行，按 Ctrl+C 退出。\n");
  std::fflush(stdout);
  while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
  return 0;
}
