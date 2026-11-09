#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "cdc.h"
#include "migrator.h"

namespace {

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
      "  --mode <sim|mysql|postgres|opengauss|mysql2pg|mysql2og>  运行模式（默认 sim）\n"
      "                                 mysql2og = MySQL -> openGauss\n"
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
      "  --type-map <path>             类型映射（默认 config/type_map.json）\n"
      "  --tables-config <path>        表级策略（copy/rebuild/skip）\n"
      "  --no-index                    不迁移二级索引\n"
      "  --no-analyze                  不执行 ANALYZE\n"
      "  --no-copy                     回退 INSERT 模式\n"
      "  --with-views                  迁移视图\n"
      "\n"
      "校验:\n"
      "  --no-checksum                 只比行数\n"
      "  --no-verify                   跳过校验\n"
      "  --verify-only                 只校验不迁移\n"
      "\n"
      "CDC 增量:\n"
      "  --cdc                         迁移后持续增量\n"
      "  --cdc-only                    只跑增量\n"
      "  --cdc-ddl                     同步 DDL\n"
      "  --cdc-start <file:pos>        起始位点\n"
      "  --cdc-server-id <N>           复制客户端 server_id\n"
      "  --cdc-max-events <N>          应用 N 个事件后退出（0=不限）\n"
      "\n"
      "其他:\n"
      "  --rows <N>                    模拟总行数（默认 200000）\n"
      "  --tables <N>                  模拟表数（默认 4）\n"
      "  --port <N>                    面板端口（默认 8123）\n"
      "  --exit                        迁移完退出\n"
      "  -h, --help                    显示帮助\n");
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
  bool exit_after = false;
  bool verify_only = false;
  bool cdc_only = false;

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
    else if (a == "--target-dialect") {
      const std::string d = next();
      if (d == "opengauss" || d == "og") o.opengauss = true;
      else if (d == "postgres" || d == "pg") o.opengauss = false;
    }
    else if (a == "--log-every") o.log_every = std::stoi(next());
    else if (a == "--out-dir") o.out_dir = next();
    else if (a == "--throttle") o.throttle_ms = std::stoi(next());
    else if (a == "--port") o.http_port = std::stoi(next());
    else if (a == "--type-map") o.type_map_path = next();
    else if (a == "--tables-config") o.table_policy_path = next();
    else if (a == "--no-index") o.build_indexes = false;
    else if (a == "--no-analyze") o.analyze_after = false;
    else if (a == "--no-copy") o.use_copy = false;
    else if (a == "--no-checksum") o.checksum = false;
    else if (a == "--verify-only") verify_only = true;
    else if (a == "--with-views") o.with_views = true;
    else if (a == "--cdc") o.cdc = true;
    else if (a == "--cdc-only") { o.cdc = true; cdc_only = true; }
    else if (a == "--cdc-ddl") o.cdc_ddl = true;
    else if (a == "--cdc-start") o.cdc_start = next();
    else if (a == "--cdc-server-id") o.cdc_server_id = std::stoi(next());
    else if (a == "--cdc-max-events") o.cdc_max_events = std::stoi(next());
    else if (a == "--no-verify") o.do_verify = false;
    else if (a == "--exit") exit_after = true;
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); return 2; }
  }

  if (o.mode == "mysql2og" || o.mode == "mysql2opengauss" ||
      o.mode == "opengauss")
    o.opengauss = true;

  mig::TypeMap tm;
  mig::Status st = tm.load(o.type_map_path);
  if (!st.ok()) {
    std::fprintf(stderr, "[fatal] type map: %s\n", st.str().c_str());
    return 1;
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

  auto* mheap = new mig::Metrics();
  mig::Metrics& m = *mheap;
  m.mode = o.mode;

  std::printf("========================================\n");
  std::printf(" mini-migrator\n");
  std::printf(" mode    : %s\n", o.mode.c_str());
  std::printf(" batch   : %d\n", o.batch_size);
  std::printf(" rows    : %lld\n", (long long)o.sim_rows);
  std::printf(" tables  : %d\n", o.sim_tables);
  std::printf(" workers : %d\n", o.workers);
  std::printf(" panel   : http://127.0.0.1:%d/\n", o.http_port);
  std::printf(" log dir : %s\n", o.out_dir.c_str());
  std::printf("========================================\n");
  std::fflush(stdout);

  if (verify_only) {
    m.started_at = mig::now_sec();
    src->connect();
    tgt->connect();
    mig::Status vr = mig::run_verify(o, *src, *tgt, m);
    mig::write_log_file(m, o.out_dir);
    std::printf("\n verify-only: %s\n", vr.ok() ? "PASS" : "FAIL");
    std::fflush(stdout);
    return vr.ok() ? 0 : 1;
  }

  std::thread http_thread([&]() {
    int rc = mig::run_http_server(o.http_port, m);
    if (rc != 0) std::fprintf(stderr, "[warn] http server failed, code=%d\n", rc);
  });
  http_thread.detach();

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
    mig::Status vr = mig::run_verify(o, *src, *tgt, m);
    if (!vr.ok()) rc = vr;
  }

  if (rc.ok() && o.cdc) {
    mig::Status cr = mig::run_cdc(o, m, o.cdc_max_events);
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
  std::printf(" panel  : http://127.0.0.1:%d/\n", o.http_port);
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
