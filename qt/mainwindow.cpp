#include "mainwindow.h"

#include <QLinearGradient>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStringList>
#include <QStyle>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

// ---------------- 吞吐折线图 ----------------
class SparkChart : public QWidget {
 public:
  explicit SparkChart(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumHeight(130);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }

  void setData(const std::vector<std::pair<double, int64_t>>& data) {
    data_ = data;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing, true);
    g.fillRect(rect(), QColor("#0e1527"));

    g.setPen(QColor("#1b2740"));
    for (int i = 0; i <= 4; ++i) {
      const int y = 10 + (height() - 30) * i / 4;
      g.drawLine(0, y, width(), y);
    }

    if (data_.size() < 2) {
      g.setPen(QColor("#8b9bb4"));
      g.drawText(10, 22, QStringLiteral("等待采样…"));
      return;
    }

    std::vector<double> pts;
    pts.reserve(data_.size());
    for (size_t i = 1; i < data_.size(); ++i) {
      const double dt = data_[i].first - data_[i - 1].first;
      const double dr = (double)(data_[i].second - data_[i - 1].second);
      pts.push_back(dt > 0 ? dr / dt : 0.0);
    }

    double mx = 1.0;
    for (double v : pts) mx = std::max(mx, v);
    mx *= 1.15;

    const int pad = 10;
    const int W = width() - 2 * pad;
    const int H = height() - 30;

    QPainterPath path;
    for (size_t i = 0; i < pts.size(); ++i) {
      const double x = pad + (double)W * i / std::max<size_t>(1, pts.size() - 1);
      const double y = 10 + H - H * (pts[i] / mx);
      if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
    }

    QPainterPath fill = path;
    fill.lineTo(pad + W, 10 + H);
    fill.lineTo(pad, 10 + H);
    fill.closeSubpath();

    QLinearGradient grad(0, 0, 0, 10 + H);
    grad.setColorAt(0, QColor(76, 201, 240, 70));
    grad.setColorAt(1, QColor(76, 201, 240, 0));
    g.fillPath(fill, grad);

    g.setPen(QPen(QColor("#4cc9f0"), 2));
    g.drawPath(path);

    g.setPen(QColor("#8b9bb4"));
    g.drawText(pad, 14, QString::number((long long)mx) + " r/s");
  }

 private:
  std::vector<std::pair<double, int64_t>> data_;
};

// ---------------- 工具 ----------------
namespace {

QLabel* makeValueLabel(QWidget* parent) {
  auto* l = new QLabel("—", parent);
  l->setStyleSheet("font-size:22px;font-weight:700;font-family:Menlo,monospace;");
  return l;
}

QFrame* makeCard(const QString& title, QLabel* value, QWidget* parent) {
  auto* f = new QFrame(parent);
  f->setObjectName("card");
  auto* v = new QVBoxLayout(f);
  v->setContentsMargins(12, 10, 12, 10);
  v->setSpacing(4);
  auto* t = new QLabel(title, f);
  t->setStyleSheet("color:#8b9bb4;font-size:12px;");
  v->addWidget(t);
  v->addWidget(value);
  return f;
}

QString fmtNum(long long v) {
  return QString::number(v).replace(QRegularExpression("(\\d)(?=(\\d{3})+(?!\\d))"), "\\1,");
}

QString fmtDur(double s) {
  if (!(s > 0) || !std::isfinite(s)) return QStringLiteral("—");
  const int t = (int)s;
  if (t < 60) return QString::number(t) + "s";
  if (t < 3600) return QString("%1m %2s").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
  return QString("%1h %2m").arg(t / 3600).arg((t % 3600) / 60, 2, 10, QChar('0'));
}

QString fmtBytes(long long b) {
  const char* u[] = {"B", "KB", "MB", "GB", "TB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
  return QString::number(v, 'f', i == 0 ? 0 : 1) + " " + u[i];
}

}  // namespace

// ---------------- MainWindow ----------------
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("mini-migrator — 数据库迁移");
  buildUi();

  timer_ = new QTimer(this);
  timer_->setInterval(400);
  connect(timer_, &QTimer::timeout, this, &MainWindow::onTick);
}

MainWindow::~MainWindow() {
  if (worker_.joinable()) worker_.detach();
}

QString MainWindow::findTypeMap() const {
  const QStringList cands = {"config/type_map.json", "../config/type_map.json",
                             "../../config/type_map.json"};
  for (const auto& p : cands) {
    if (QFile::exists(p)) return p;
  }
  return "config/type_map.json";
}

void MainWindow::appendLog(const QString& line) {
  log_->appendPlainText(line);
  log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
}

void MainWindow::buildUi() {
  setStyleSheet(R"(
    QMainWindow, QWidget { background:#0b1020; color:#e6edf3; font-size:13px; }
    QLabel#title { font-size:20px; font-weight:700; }
    QLabel#sub { color:#8b9bb4; }
    QLabel#badge { background:#1b2740; color:#8b9bb4; border:1px solid #22304d;
                   border-radius:10px; padding:4px 10px; }
    QLabel#badgeRun { background:rgba(76,201,240,0.15); color:#4cc9f0;
                      border:1px solid #2b6f8a; border-radius:10px; padding:4px 10px; }
    QLabel#badgeOk { background:rgba(61,220,151,0.15); color:#3ddc97;
                     border:1px solid #2b7a5c; border-radius:10px; padding:4px 10px; }
    QLabel#badgeErr { background:rgba(255,93,115,0.15); color:#ff5d73;
                      border:1px solid #8a2b3a; border-radius:10px; padding:4px 10px; }
    QFrame#card { background:#121a30; border:1px solid #22304d; border-radius:10px; }
    QLineEdit, QComboBox, QSpinBox { background:#121a30; border:1px solid #22304d;
      border-radius:6px; padding:5px 8px; color:#e6edf3; }
    QPushButton { background:#1f6feb; border:none; border-radius:8px;
                  padding:9px 20px; font-weight:600; }
    QPushButton:disabled { background:#1b2740; color:#6b7a94; }
    QProgressBar { background:#121a30; border:1px solid #22304d; border-radius:8px;
                   height:20px; text-align:center; }
    QProgressBar::chunk { background:#4cc9f0; border-radius:7px; }
    QTableWidget { background:#121a30; border:1px solid #22304d;
                   gridline-color:#1b2740; }
    QHeaderView::section { background:#0e1527; color:#8b9bb4; border:none; padding:6px; }
    QPlainTextEdit { background:#0e1527; border:1px solid #22304d; color:#b9c6da;
                     font-family:Menlo,monospace; font-size:12px; }
  )");

  auto* central = new QWidget(this);
  auto* root = new QVBoxLayout(central);
  root->setContentsMargins(16, 14, 16, 14);
  root->setSpacing(12);

  // 标题栏
  auto* head = new QHBoxLayout;
  auto* title = new QLabel("mini-migrator", central);
  title->setObjectName("title");
  auto* sub = new QLabel("数据库迁移 · 实时监控", central);
  sub->setObjectName("sub");
  head->addWidget(title);
  head->addWidget(sub);
  head->addStretch();
  phase_ = new QLabel("idle", central);
  phase_->setObjectName("badge");
  head->addWidget(phase_);
  root->addLayout(head);

  // 配置区
  auto* form = new QGridLayout;
  form->setHorizontalSpacing(10);
  form->setVerticalSpacing(8);

  mode_ = new QComboBox(central);
  mode_->addItems({"sim", "mysql2pg", "mysql", "postgres", "opengauss"});

  source_ = new QLineEdit(central);
  source_->setText("host=127.0.0.1;port=3306;user=root;db=demo");
  target_ = new QLineEdit(central);
  target_->setText("host=127.0.0.1;port=5432;user=bellgrell;db=demo_dst");

  rowsSpin_ = new QSpinBox(central);
  rowsSpin_->setRange(1, 100000000);
  rowsSpin_->setValue(600000);
  rowsSpin_->setSingleStep(10000);

  tablesSpin_ = new QSpinBox(central);
  tablesSpin_->setRange(1, 32);
  tablesSpin_->setValue(6);

  batchSpin_ = new QSpinBox(central);
  batchSpin_->setRange(1, 100000);
  batchSpin_->setValue(500);

  throttleSpin_ = new QSpinBox(central);
  throttleSpin_->setRange(0, 5000);
  throttleSpin_->setValue(120);
  throttleSpin_->setSuffix(" ms");

  workersSpin_ = new QSpinBox(central);
  workersSpin_->setRange(1, 32);
  workersSpin_->setValue(2);
  workersSpin_->setToolTip("并行迁移线程数（按表并行）");

  verifyChk_ = new QCheckBox("迁移后校验", central);
  verifyChk_->setChecked(true);

  startBtn_ = new QPushButton("开始迁移", central);
  connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStart);

  int r = 0;
  form->addWidget(new QLabel("模式", central), r, 0);
  form->addWidget(mode_, r, 1);
  form->addWidget(new QLabel("行数", central), r, 2);
  form->addWidget(rowsSpin_, r, 3);
  form->addWidget(new QLabel("表数", central), r, 4);
  form->addWidget(tablesSpin_, r, 5);
  form->addWidget(startBtn_, r, 6, 2, 1);

  ++r;
  form->addWidget(new QLabel("源 DSN", central), r, 0);
  form->addWidget(source_, r, 1, 1, 3);
  form->addWidget(new QLabel("批大小", central), r, 4);
  form->addWidget(batchSpin_, r, 5);

  ++r;
  form->addWidget(new QLabel("目标 DSN", central), r, 0);
  form->addWidget(target_, r, 1, 1, 3);
  form->addWidget(new QLabel("节流", central), r, 4);
  form->addWidget(throttleSpin_, r, 5);

  ++r;
  form->addWidget(new QLabel("并行", central), r, 0);
  form->addWidget(workersSpin_, r, 1);
  form->addWidget(verifyChk_, r, 6);

  form->setColumnStretch(1, 3);
  form->setColumnStretch(3, 3);
  root->addLayout(form);

  // 进度条
  progress_ = new QProgressBar(central);
  progress_->setRange(0, 1000);
  progress_->setValue(0);
  progress_->setFormat("0.0%");
  root->addWidget(progress_);

  // 指标卡片
  auto* cards = new QHBoxLayout;
  rowsVal_ = makeValueLabel(central);
  speedVal_ = makeValueLabel(central);
  etaVal_ = makeValueLabel(central);
  errVal_ = makeValueLabel(central);
  verifyVal_ = makeValueLabel(central);

  cards->addWidget(makeCard("已迁移 / 总行数", rowsVal_, central));
  cards->addWidget(makeCard("吞吐 rows/s", speedVal_, central));
  cards->addWidget(makeCard("预计剩余", etaVal_, central));
  cards->addWidget(makeCard("错误", errVal_, central));
  cards->addWidget(makeCard("校验", verifyVal_, central));
  root->addLayout(cards);

  srcDst_ = new QLabel("—", central);
  srcDst_->setStyleSheet("color:#8b9bb4;font-size:12px;");
  root->addWidget(srcDst_);

  // 图表
  chart_ = new SparkChart(central);
  root->addWidget(chart_);

  // 表进度
  table_ = new QTableWidget(0, 5, central);
  table_->setHorizontalHeaderLabels({"表名", "状态", "已迁移", "总数", "进度"});
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
  table_->verticalHeader()->setVisible(false);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setMaximumHeight(180);
  root->addWidget(table_);

  // 日志
  log_ = new QPlainTextEdit(central);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(2000);
  root->addWidget(log_, 1);

  setCentralWidget(central);
}

void MainWindow::setRunning(bool running) {
  running_ = running;
  startBtn_->setEnabled(!running);
  startBtn_->setText(running ? "迁移中…" : "开始迁移");
  mode_->setEnabled(!running);
  source_->setEnabled(!running);
  target_->setEnabled(!running);
  rowsSpin_->setEnabled(!running);
  tablesSpin_->setEnabled(!running);
  batchSpin_->setEnabled(!running);
  throttleSpin_->setEnabled(!running);
  workersSpin_->setEnabled(!running);
}

void MainWindow::onStart() {
  if (running_) return;

  if (metrics_) { delete metrics_; metrics_ = nullptr; }
  metrics_ = new mig::Metrics();
  metrics_->mode = mode_->currentText().toStdString();

  mig::Options o;
  o.mode = mode_->currentText().toStdString();
  o.source = source_->text().toStdString();
  o.target = target_->text().toStdString();
  o.batch_size = batchSpin_->value();
  o.throttle_ms = throttleSpin_->value();
  o.workers = workersSpin_->value();
  o.sim_rows = rowsSpin_->value();
  o.sim_tables = tablesSpin_->value();
  o.do_verify = verifyChk_->isChecked();
  o.type_map_path = findTypeMap().toStdString();

  mig::TypeMap tm;
  mig::Status ts = tm.load(o.type_map_path);
  if (!ts.ok()) {
    appendLog("[fatal] 类型映射: " + QString::fromStdString(ts.str()));
    return;
  }

  mig::Status s1;
  auto src = mig::make_source(o, s1);
  if (!src) {
    appendLog("[fatal] 源端: " + QString::fromStdString(s1.str()));
    return;
  }
  mig::Status s2;
  auto tgt = mig::make_target(o, s2);
  if (!tgt) {
    appendLog("[fatal] 目标端: " + QString::fromStdString(s2.str()));
    return;
  }

  log_->clear();
  table_->setRowCount(0);
  progress_->setValue(0);
  appendLog("开始迁移: " + mode_->currentText());
  appendLog("type map: " + QString::fromStdString(o.type_map_path));
  setRunning(true);

  mig::Metrics* mm = metrics_;
  worker_ = std::thread([o, tm, src, tgt, mm]() mutable {
    mig::Status rc = mig::run_migration(o, tm, *src, *tgt, *mm);
    if (rc.ok() && o.do_verify) {
      mig::run_verify(o, *src, *tgt, *mm);
    }
    mig::write_log_file(*mm, o.out_dir);
  });
  worker_.detach();

  timer_->start();
}

void MainWindow::onTick() {
  if (!metrics_) return;

  const QByteArray statusBytes = QByteArray::fromStdString(metrics_->status_json());
  const QJsonObject o = QJsonDocument::fromJson(statusBytes).object();

  const double pct = o.value("percent").toDouble();
  progress_->setValue((int)(pct * 10.0));
  progress_->setFormat(QString::number(pct, 'f', 1) + "%");

  rowsVal_->setText(fmtNum((long long)o.value("rows_done").toDouble()) + " / " +
                    fmtNum((long long)o.value("rows_total").toDouble()));
  speedVal_->setText(QString::number((long long)o.value("rows_per_sec").toDouble()));
  etaVal_->setText(o.value("running").toBool()
                       ? fmtDur(o.value("eta").toDouble())
                       : QStringLiteral("—"));

  const int errs = (int)o.value("errors").toDouble();
  errVal_->setText(QString::number(errs));
  errVal_->setStyleSheet(errs > 0
                             ? "font-size:22px;font-weight:700;color:#ff5d73;"
                             : "font-size:22px;font-weight:700;color:#3ddc97;");

  const QJsonObject v = o.value("verify").toObject();
  if (v.value("running").toBool()) verifyVal_->setText("校验中");
  else if (v.value("done").toBool())
    verifyVal_->setText(v.value("mismatch").toDouble() > 0 ? "失败" : "通过");
  else verifyVal_->setText("—");

  const QString phase = o.value("phase").toString();
  phase_->setText(phase);
  const bool finished = o.value("finished").toBool();
  if (finished) {
    const bool ok = !v.value("done").toBool() || v.value("mismatch").toDouble() == 0;
    phase_->setObjectName(ok ? "badgeOk" : "badgeErr");
  } else if (o.value("running").toBool()) {
    phase_->setObjectName("badgeRun");
  } else {
    phase_->setObjectName("badge");
  }
  phase_->style()->unpolish(phase_);
  phase_->style()->polish(phase_);

  srcDst_->setText(QString::fromStdString(metrics_->source_desc) + "  →  " +
                   QString::fromStdString(metrics_->target_desc));

  // 表
  const QJsonArray tables =
      QJsonDocument::fromJson(QByteArray::fromStdString(metrics_->tables_json())).array();
  table_->setRowCount(tables.size());
  for (int i = 0; i < tables.size(); ++i) {
    const QJsonObject t = tables[i].toObject();
    const QString name = t.value("name").toString();
    const QString state = t.value("state").toString();
    const long long done = (long long)t.value("done").toDouble();
    const long long total = (long long)t.value("total").toDouble();
    const double p = t.value("percent").toDouble();

    auto* c0 = new QTableWidgetItem(name);
    auto* c1 = new QTableWidgetItem(state);
    auto* c2 = new QTableWidgetItem(fmtNum(done));
    auto* c3 = new QTableWidgetItem(fmtNum(total));
    auto* c4 = new QTableWidgetItem(QString::number(p, 'f', 1) + "%");
    if (state == "done") c1->setForeground(QColor("#3ddc97"));
    else if (state == "running") c1->setForeground(QColor("#4cc9f0"));
    else if (state == "failed") c1->setForeground(QColor("#ff5d73"));
    table_->setItem(i, 0, c0);
    table_->setItem(i, 1, c1);
    table_->setItem(i, 2, c2);
    table_->setItem(i, 3, c3);
    table_->setItem(i, 4, c4);
  }

  // 事件
  const QJsonArray events =
      QJsonDocument::fromJson(QByteArray::fromStdString(metrics_->events_json())).array();
  QStringList lines;
  for (const auto& e : events) lines << e.toString();
  const QString joined = lines.join("\n");
  if (joined != log_->toPlainText()) {
    log_->setPlainText(joined);
    log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
  }

  // 吞吐曲线
  const QJsonArray series =
      QJsonDocument::fromJson(QByteArray::fromStdString(metrics_->series_json())).array();
  std::vector<std::pair<double, int64_t>> pts;
  pts.reserve(series.size());
  for (const auto& s : series) {
    const QJsonArray pair = s.toArray();
    if (pair.size() == 2) {
      pts.emplace_back(pair[0].toDouble(), (int64_t)pair[1].toDouble());
    }
  }
  chart_->setData(pts);

  if (finished && running_) {
    setRunning(false);
    timer_->stop();
    appendLog("—— 迁移结束 ——");
  }
}
