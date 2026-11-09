#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
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
  std::string s;  // kString 或 kBytes（原始字节）

  static Value Null();
  static Value Int(int64_t v);
  static Value Dbl(double v);
  static Value Str(std::string v);
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
  std::string type_map_path = "config/type_map.json";
  std::string table_policy_path = "config/tables.json";
  int batch_size = 1000;
  int commit_every = 10;
  int throttle_ms = 0;
  int workers = 1;
  int pk_min_rows = 1000000;  // 单表主键区间并行阈值（小表顺序更快）
  bool pk_parallel = true;   // 单表按主键区间并行
  int log_every = 10;
  int http_port = 8123;
  bool do_verify = true;
  bool build_indexes = true;
  bool analyze_after = true;
  bool use_copy = true;
  bool checksum = true;
  bool with_views = false;
  bool cdc = false;
  bool opengauss = false;  // 目标方言：openGauss（PG 协议 + 差异处理）
  std::string cdc_start;
  std::string cdc_position_path = "out/cdc-position.txt";
  int cdc_server_id = 987654321;
  int cdc_max_events = 0;
  bool cdc_ddl = false;
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

struct Metrics {
  std::atomic<int64_t> rows_done{0};
  std::atomic<int64_t> rows_total{0};
  std::atomic<int64_t> errors{0};
  std::atomic<int64_t> batches{0};
  std::atomic<int64_t> bytes_written{0};
  std::atomic<bool> running{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> verifying{false};
  std::atomic<bool> verified{false};
  std::atomic<int64_t> verify_mismatch{0};
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
  void sample();
  std::string status_json();
  std::string tables_json();
  std::string events_json();
  std::string series_json();
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
                        Checksum& out);
bool checksum_equal(const Checksum& a, const Checksum& b);

std::string build_ddl(const TableSchema& s);
std::string convert_view_sql(const std::string& mysql_sql);
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

void write_log_file(Metrics& m, const std::string& dir);

int run_http_server(int port, Metrics& m);

}  // namespace mig
