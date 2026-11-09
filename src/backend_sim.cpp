#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <sstream>

#include "backends.h"

namespace mig {
namespace {

struct TableSpec {
  std::string name;
  int64_t rows;
};

class SimSource : public ISource {
 public:
  explicit SimSource(const Options& o) : opts_(o) {
    const int n = std::max(1, o.sim_tables);
    const int64_t per = std::max<int64_t>(1, o.sim_rows / n);
    static const char* names[] = {"users", "orders", "products", "events",
                                  "sessions", "payments"};
    for (int i = 0; i < n; ++i) {
      TableSpec s;
      s.name = names[i % 6];
      if (i >= 6) s.name += std::to_string(i / 6);
      s.rows = per;
      tables_.push_back(s);
    }
  }

  Status connect() override { return Status::OK(); }

  Status list_tables(std::vector<std::string>& out) override {
    out.clear();
    for (auto& t : tables_) out.push_back(t.name);
    return Status::OK();
  }

  Status read_schema(const std::string& table, TableSchema& out) override {
    out = TableSchema();
    out.name = table;
    out.columns = {
        {"id", "bigint", "", false, true, true},
        {"name", "varchar(64)", "", false, false, false},
        {"email", "varchar(128)", "", true, false, false},
        {"created_at", "datetime", "", true, false, false},
        {"score", "decimal(10,2)", "", true, false, false},
        {"bio", "text", "", true, false, false},
        {"active", "tinyint(1)", "", true, false, false},
    };
    out.primary_key = {"id"};
    return Status::OK();
  }

  Status open_cursor(const std::string& table) override {
    cursor_table_ = table;
    cursor_pos_ = 0;
    cursor_total_ = 0;
    for (auto& t : tables_) {
      if (t.name == table) { cursor_total_ = t.rows; break; }
    }
    return Status::OK();
  }

  Status fetch_batch(int n, std::vector<Row>& out) override {
    out.clear();
    if (n <= 0) return Status::OK();
    for (int k = 0; k < n && cursor_pos_ < cursor_total_; ++k, ++cursor_pos_) {
      out.push_back(make_row(cursor_table_, cursor_pos_));
    }
    return Status::OK();
  }

  void close_cursor() override { cursor_table_.clear(); }

  int64_t total_rows(const std::string& table) override {
    for (auto& t : tables_) {
      if (t.name == table) return t.rows;
    }
    return 0;
  }

  std::string describe() override {
    return "sim source (" + std::to_string(tables_.size()) + " tables, " +
           std::to_string(opts_.sim_rows) + " rows)";
  }

 private:
  static Row make_row(const std::string& table, int64_t i) {
    Row r;
    r.push_back(Value::Int(i + 1));
    r.push_back(Value::Str(table + "_name_" + std::to_string(i % 997)));
    r.push_back(Value::Str("user" + std::to_string(i) + "@example.com"));
    char ts[32];
    std::snprintf(ts, sizeof(ts), "2026-%02d-%02d %02d:%02d:%02d",
                  1 + (int)(i % 12), 1 + (int)(i % 28), (int)(i % 24),
                  (int)(i % 60), (int)(i % 60));
    r.push_back(Value::Str(ts));
    r.push_back(Value::Dbl(((double)(i % 10000)) / 100.0));
    r.push_back(Value::Str("bio text for row " + std::to_string(i) +
                           " lorem ipsum dolor sit amet"));
    r.push_back(Value::Bool((i % 3) == 0));
    return r;
  }

  Options opts_;
  std::vector<TableSpec> tables_;
  std::string cursor_table_;
  int64_t cursor_pos_ = 0;
  int64_t cursor_total_ = 0;
};

// 所有 SimTarget 实例共享一份计数器，便于并行迁移后校验
std::map<std::string, int64_t>& sim_registry() {
  static std::map<std::string, int64_t> m;
  return m;
}
std::mutex& sim_registry_mu() {
  static std::mutex m;
  return m;
}

class SimTarget : public ITarget {
 public:
  explicit SimTarget(const Options& o) : opts_(o) {}

  Status connect() override { return Status::OK(); }

  void reset_counters() override {
    std::lock_guard<std::mutex> lk(sim_registry_mu());
    sim_registry().clear();
  }

  Status exec_sql(const std::string&) override { return Status::OK(); }

  Status create_table(const TableSchema& s, const std::string& ddl) override {
    std::lock_guard<std::mutex> lk(sim_registry_mu());
    sim_registry()[s.name] += 0;
    ddl_[s.name] = ddl;
    return Status::OK();
  }

  Status write_batch(const std::string& table, const std::vector<Row>& rows) override {
    std::lock_guard<std::mutex> lk(sim_registry_mu());
    sim_registry()[table] += (int64_t)rows.size();
    return Status::OK();
  }

  Status commit() override { return Status::OK(); }
  Status rollback() override { return Status::OK(); }
  void close() override {}

  int64_t written(const std::string& table) override {
    std::lock_guard<std::mutex> lk(sim_registry_mu());
    auto it = sim_registry().find(table);
    return it == sim_registry().end() ? 0 : it->second;
  }

  std::string describe() override { return "sim target (in-memory counters)"; }

 private:
  Options opts_;
  std::map<std::string, std::string> ddl_;
};

}  // namespace

std::shared_ptr<ISource> make_sim_source(const Options& o) {
  return std::make_shared<SimSource>(o);
}

std::shared_ptr<ITarget> make_sim_target(const Options& o) {
  return std::make_shared<SimTarget>(o);
}

}  // namespace mig
