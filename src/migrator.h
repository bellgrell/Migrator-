#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mig {

enum class Code { kOk = 0, kNotFound, kIO, kInvalid, kUnsupported, kDb };

struct Status {
  Code code = Code::kOk;
  std::string msg;
  Status() = default;
  Status(Code c, std::string m) : code(c), msg(std::move(m)) {}
  bool ok() const { return code == Code::kOk; }
  static Status OK() { return Status(); }
  static Status Err(Code c, std::string m) { return Status(c, std::move(m)); }
  std::string str() const;
};

enum class ValType { kNull = 0, kInt, kDouble, kString, kBool, kBytes };

struct Value {
  ValType type = ValType::kNull;
  int64_t i = 0;
  double d = 0.0;
  bool b = false;
  bool dec = false;  // kString 但语义是定点数（DECIMAL/NUMERIC）
  std::string s;  // kString 或 kBytes（原始字节）

  static Value Null();
  static Value Int(int64_t v);
  static Value Dbl(double v);
  static Value Str(std::string v);
  static Value Dec(std::string v);
  static Value Bool(bool v);
  static Value Bytes(std::string raw);

  std::string sql_literal() const;
  std::string json() const;
};

using Row = std::vector<Value>;

struct ColumnDef {
  std::string name;
  std::string src_type;
  std::string dst_type;
  bool nullable = true;
  bool primary = false;
  bool auto_increment = false;
  bool binary = false;
};

struct IndexDef {
  std::string name;
  std::vector<std::string> columns;
  bool unique = false;
};

struct TableSchema {
  std::string name;
  std::vector<ColumnDef> columns;
  std::vector<std::string> primary_key;
  std::vector<IndexDef> indexes;
};

struct TablePolicy {
  std::string strategy = "copy";  // copy | rebuild | skip
  std::string rebuild_sql;        // strategy=rebuild 时要执行的 SQL 文件
};

struct Options {
  std::string mode = "sim";
  std::string source = "";
  std::string target = "";
  // 源/目标类型：mysql | pg | og | sim（由 mode 推导，也可 --source-type/--target-type 覆盖）
  std::string source_kind = "sim";
  std::string target_kind = "sim";
  bool pg_source_uses_source = false;  // PgSource 读 o.source（迁移源）而不是 o.target（校验源）
  std::string mysql_source_dsn;        // 非空时 MySqlSource 用它（校验 MySQL 目标用）
  std::string type_map_path = "config/type_map.json";
  std::string table_policy_path = "config/tables.json";
  int batch_size = 1000;
  int commit_every = 10;
  int throttle_ms = 0;
  std::string io_scheduler = "fair";  // off | fair | scan（批 I/O 调度策略）
  int io_queue_depth = 0;              // 并发读批次数，0 = workers
  int io_prefetch = 0;                 // 每区间预取批数（0=关闭；远程库通常无收益）
  std::string load_mode = "auto";      // auto | insert | ignore | replace
  int io_window = 0;                   // SCAN 窗口，0 = 自动
  int workers = 1;
  int pk_min_rows = 1000000;  // 单表主键区间并行阈值（小表顺序更快）
  bool pk_parallel = true;   // 单表按主键区间并行
  int log_every = 10;
  int http_port = 8123;
  std::string http_bind = "127.0.0.1";
  std::string http_token;                 // bearer token for the dashboard
  std::string http_token_file;            // read token from a file
  std::string http_audit = "out/audit.log";
  bool http_insecure = false;             // allow remote plaintext (explicit)
  bool http_cors = false;
  std::string tls_cert;
  std::string tls_key;
  std::string instance_lock;              // flock-based single-instance guard
  int connect_timeout = 10;      // 连接/读写超时（秒）
  int connect_retries = 3;       // 连接失败重试次数
  int connect_backoff_ms = 500;  // 重试退避基数（毫秒）
  bool truncate_target = false;  // 迁移前 DROP 目标表，便于失败后重跑
  bool idempotent = false;       // 重放安全：已存在的行跳过，不重复写入
  bool skip_row_count = false;   // 大表跳过进度用的全表 COUNT(*)
  bool consistent_snapshot = false;  // MySQL 全局一致性快照（单连接）
  bool resume = false;               // 按主键水位断点续传
  bool repair = false;               // 校验后按主键补/删
  std::string resume_dir = "out/resume";
  std::vector<std::string> only_tables;     // 仅迁移这些表
  std::vector<std::string> exclude_tables;  // 排除这些表
  std::vector<std::string> disabled_modules;  // 运行时禁用的模块
  std::string modules_config_path = "config/modules.json";

  bool module_enabled(const std::string& name) const {
    for (const auto& m : disabled_modules)
      if (m == name) return false;
    return true;
  }
  std::string snapshot_position_path = "out/snapshot-position.txt";
  int cdc_retries = 10;          // CDC 断线重连次数
  std::string cdc_slot = "migrator_slot";
  std::string pg_cdc_position_path = "out/pg-cdc-position.txt";
  std::string sslmode = "prefer";           // PostgreSQL/openGauss
  std::string mysql_ssl_mode = "PREFERRED"; // DISABLED/PREFERRED/REQUIRED
  bool do_verify = true;
  bool build_indexes = true;
  bool analyze_after = true;
  bool use_copy = true;
  bool checksum = true;
  std::string verify_engine = "mmv";   // mmv（默认）| fnv（兼容回退）
  bool verify_async = false;           // 迁移完成后异步全量校验（不阻塞退出）
  int verify_async_threshold = 10000000;  // 行数 >= 阈值时自动异步
  bool verify_wait = false;            // 锁被占用时等待而不是失败
  bool explain_types = false;           // 只打印类型规则解析结果
  bool use_type_rules = false;          // 用类型规则生成目标 DDL
  bool check_type_rules = false;        // 检查规则冲突/覆盖
  bool prove_type_rules = false;        // 形式化不变量检查（P1..P8）
  bool dump_type_tests = false;         // 输出规则边界测试表
  std::string type_rules_path;          // 外部规则文件（TSV）
  std::string export_type_rules;        // 导出规则到 TSV
  std::string emit_type_tests_dir;      // 生成边界测试清单目录
  std::string verify_status_file;      // 异步子进程写出的状态文件
  std::string exe_path;                 // 当前可执行文件绝对路径（子进程复用）
  int checksum_batch_bytes = 2097152;  // 校验批次字节预算
  std::size_t checksum_max_rows = 0;   // 每表最多校验行数（0 = 不限）
  bool with_views = false;
  bool cdc = false;
  bool opengauss = false;  // 目标方言：openGauss（PG 协议 + 差异处理）
  std::string cdc_start;
  bool cdc_drop_slot = false;  // CDC 正常结束后删除逻辑复制槽（一次性迁移用）
  std::string cdc_position_path = "out/cdc-position.txt";
  int cdc_server_id = 987654321;
  int cdc_max_events = 0;
  std::string cdc_until;      // MySQL CDC cutover file:pos
  std::string cdc_until_lsn;  // PG CDC cutover LSN
  bool cdc_ddl = false;
  // ODBC（SQL Server / Oracle）CDC
  bool cdc_setup = false;                     // 开跑前启用源端日志（sp_cdc_enable_* / 补充日志）
  int cdc_poll_ms = 1000;                     // 无新事件时的轮询间隔
  std::string odbc_cdc_position_path = "out/odbc-cdc-position.txt";
  int64_t sim_rows = 200000;
  int sim_tables = 4;
  std::string out_dir = "out";
};

struct TableMetric {
  std::string name;
  int64_t total = 0;
  std::atomic<int64_t> done{0};
  std::atomic<int64_t> errors{0};
  std::atomic<int> state{0};  // 0=pending 1=running 2=done 3=failed
  double started_at = 0.0;
  double finished_at = 0.0;
};

struct RepairMismatch {
  std::string kind;
  std::string key;
  std::string column;
  std::string source_value;
  std::string target_value;
  std::vector<std::string> key_values;
};

struct Metrics {
  std::atomic<int64_t> rows_done{0};
  std::atomic<int64_t> rows_total{0};
  std::atomic<int64_t> errors{0};
  std::atomic<int64_t> batches{0};
  std::atomic<int64_t> bytes_written{0};
  std::atomic<bool> running{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> verifying{false};
  std::atomic<bool> metrics_enabled{true};
  std::atomic<bool> verified{false};
  std::atomic<int64_t> verify_mismatch{0};
  // Filled by the mmv verification engine; run_repair uses it to target only
  // the tables that actually failed.
  std::atomic<bool> mmv_verified{false};
  std::atomic<int64_t> mmv_mismatches{0};
  std::vector<std::string> mmv_failed_tables;  // guarded by mu
  std::map<std::string, std::vector<RepairMismatch>> mmv_table_mismatches;  // guarded by mu
  std::atomic<int64_t> cdc_lag_bytes{0};
  double started_at = 0.0;
  double finished_at = 0.0;

  mutable std::mutex mu;
  std::string phase = "idle";
  std::string mode = "sim";
  std::string source_desc;
  std::string target_desc;
  std::vector<std::shared_ptr<TableMetric>> tables;
  std::deque<std::string> events;
  std::deque<std::pair<double, int64_t>> series;

  void log(const std::string& s);
  void add_rows(int64_t n) { rows_done.fetch_add(n, std::memory_order_relaxed); }
  void add_batch() { batches.fetch_add(1, std::memory_order_relaxed); }
  void add_bytes(int64_t n) { bytes_written.fetch_add(n, std::memory_order_relaxed); }
  void inc_error() { errors.fetch_add(1, std::memory_order_relaxed); }
  void sample();
  std::string status_json();
  std::string tables_json();
  std::string events_json();
  std::string series_json();
  std::string prometheus();
};

class ISource {
 public:
  virtual ~ISource() = default;
  virtual Status connect() = 0;
  virtual Status list_tables(std::vector<std::string>& out) = 0;
  virtual Status read_schema(const std::string& table, TableSchema& out) = 0;
  virtual Status open_cursor(const std::string& table) = 0;
  virtual Status fetch_batch(int n, std::vector<Row>& out) = 0;
  virtual void close_cursor() = 0;
  // 单表主键区间并行（可选能力，闭区间 [lo, hi]）
  virtual Status pk_bounds(const std::string& table, const std::string& column,
                           int64_t& lo, int64_t& hi) {
    (void)table; (void)column; (void)lo; (void)hi;
    return Status::Err(Code::kUnsupported, "pk range not supported");
  }
  // 单列整数主键 -> 区间并行的列名；能力由源端决定
  virtual Status pk_range_column(const TableSchema& s, std::string& column) {
    if (s.primary_key.size() != 1) {
      return Status::Err(Code::kUnsupported, "no single primary key");
    }
    for (const auto& c : s.columns) {
      if (c.name != s.primary_key[0]) continue;
      const std::string& t = c.src_type;
      const bool int_type =
          t.rfind("tinyint", 0) == 0 || t.rfind("smallint", 0) == 0 ||
          t.rfind("mediumint", 0) == 0 || t.rfind("int", 0) == 0 ||
          t.rfind("bigint", 0) == 0;
      if (int_type) {
        column = c.name;
        return Status::OK();
      }
    }
    return Status::Err(Code::kUnsupported, "primary key is not integer");
  }
  virtual Status open_cursor_ordered(const std::string& table,
                                     const std::string& column) {
    (void)table; (void)column;
    return Status::Err(Code::kUnsupported, "ordered cursor not supported");
  }
  virtual Status open_cursor_from(const std::string& table,
                                  const std::string& pk_col,
                                  const std::string& after_literal) {
    (void)table; (void)pk_col; (void)after_literal;
    return Status::Err(Code::kUnsupported, "resume cursor not supported");
  }
  virtual Status begin_snapshot(std::string& file, uint64_t& pos) {
    (void)file; (void)pos;
    return Status::Err(Code::kUnsupported, "consistent snapshot not supported");
  }
  virtual void end_snapshot() {}
  virtual Status open_cursor_range(const std::string& table,
                                   const std::string& column, int64_t lo,
                                   int64_t hi) {
    (void)table; (void)column; (void)lo; (void)hi;
    return Status::Err(Code::kUnsupported, "pk range not supported");
  }
  virtual int64_t total_rows(const std::string& table) = 0;
  virtual Status list_views(std::vector<std::string>& out) {
    out.clear();
    return Status::OK();
  }
  virtual Status read_view_sql(const std::string& name, std::string& sql) {
    return Status::Err(Code::kUnsupported, "views not supported");
  }
  virtual std::string describe() = 0;
};

std::string build_index_ddl(const std::string& table, const IndexDef& idx);

class ITarget {
 public:
  virtual ~ITarget() = default;
  virtual Status connect() = 0;
  virtual Status create_table(const TableSchema& s, const std::string& ddl) = 0;
  virtual Status write_batch(const std::string& table, const std::vector<Row>& rows) = 0;
  virtual Status exec_sql(const std::string& sql) = 0;
  virtual Status commit() = 0;
  virtual Status rollback() = 0;
  virtual void close() = 0;
  virtual int64_t written(const std::string& table) = 0;
  virtual void reset_counters() {}
  // 批量写失败后切换为幂等模式再重试（跳过已存在行，不产生重复）
  virtual void enable_idempotent() {}
  // 目标端当前的实际列（用于增量漂移时判断缺哪一列）
  virtual Status table_columns(const std::string& table,
                               std::vector<std::string>& out) {
    (void)table; (void)out;
    return Status::Err(Code::kUnsupported, "table_columns not supported");
  }
  // 一轮迁移开始时清空目标端的名字注册表（索引名等）
  virtual void reset_names() {}
  // 目标方言的建表 DDL（默认 PostgreSQL/openGauss 方言）
  virtual std::string build_ddl(const TableSchema& s);
  // 是否支持 INSERT ... ON CONFLICT（openGauss 不支持）
  virtual bool supports_upsert() const { return true; }
  // 目标方言的索引 / ANALYZE 语句
  virtual std::string index_ddl(const std::string& table, const IndexDef& idx) {
    return build_index_ddl(table, idx);
  }
  virtual std::string analyze_sql(const std::string& table) {
    return "ANALYZE \"" + table + "\";";
  }
  virtual std::string delete_sql(const std::string& table,
                                 const std::string& pk,
                                 const std::string& literal) {
    return "DELETE FROM \"" + table + "\" WHERE \"" + pk + "\" = " + literal + ";";
  }
  // 行变更 SQL：CDC/repair 使用，由目标端决定 UPSERT/DELETE 方言
  virtual std::string upsert_sql(const TableSchema& s, const Row& row);
  virtual std::string delete_row_sql(const TableSchema& s, const Row& row);
  virtual Status drop_table(const std::string& table) {
    (void)table;
    return Status::Err(Code::kUnsupported, "drop_table not supported");
  }
  virtual std::string describe() = 0;
};

struct Checksum {
  int64_t rows = 0;
  uint64_t sum = 0;
  uint64_t xr = 0;
};

struct TypeMap {
  std::unordered_map<std::string, std::string> global_map;
  std::unordered_map<std::string, std::string> column_map;

  Status load(const std::string& path);
  std::string map_type(const std::string& src_type) const;
  std::string map_column(const std::string& table, const std::string& col,
                         const std::string& src_type) const;
  std::string to_json() const;
};

Status compute_checksum(ISource& src, const std::string& table, int batch,
                        Checksum& out, std::size_t max_bytes = 8 * 1024 * 1024,
                        std::size_t max_rows = 0,
                        const std::string& order_col = std::string());
bool checksum_equal(const Checksum& a, const Checksum& b);

bool module_compiled(const std::string& name);
Status load_modules_config(const std::string& path, Options& o);
Status load_verify_config(const std::string& path, Options& o);
bool write_text_atomic(const std::string& path, const std::string& data);

std::string build_ddl(const TableSchema& s);
std::string build_ddl_mysql(const TableSchema& s);
std::string convert_view_sql(const std::string& sql,
                             const std::string& source_kind,
                             const std::string& target_kind);
std::string build_index_ddl(const std::string& table, const IndexDef& idx);
std::string json_escape(const std::string& s);
double now_sec();

Status load_table_policies(const std::string& path,
                           std::unordered_map<std::string, TablePolicy>& out);

std::shared_ptr<ISource> make_source(const Options& o, Status& st);
std::shared_ptr<ISource> make_pg_source(const Options& o, Status& st);
std::shared_ptr<ITarget> make_target(const Options& o, Status& st);

Status run_migration(const Options& o, const TypeMap& tm, ISource& src, ITarget& tgt,
                     Metrics& m);
Status run_verify(const Options& o, ISource& src, ITarget& tgt, Metrics& m);
Status spawn_async_verify(const Options& o, Metrics& m, int lock_fd);
int acquire_verify_lock(const std::string& path, bool wait, std::string& err);
void write_verify_status(const std::string& path, const std::string& state,
                         int64_t pid, int code, const std::string& error,
                         const std::string& tables);
Status run_repair(const Options& o, ISource& src, ITarget& tgt, Metrics& m);

void write_log_file(Metrics& m, const std::string& dir);

// Dashboard server lives in http.h (HttpOptions + run_http_server).

}  // namespace mig
