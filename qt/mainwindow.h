#pragma once

#include <QMainWindow>

#include <thread>
#include <vector>

#include "migrator.h"

class QComboBox;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QProgressBar;
class QLabel;
class QTableWidget;
class QPlainTextEdit;
class QCheckBox;
class QListWidget;
class QGroupBox;
class QTimer;
class SparkChart;

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;

 private slots:
  void onStart();
  void onTick();
  void onLoadTables();
  void onSelectAllTables();
  void onSelectNoneTables();

 private:
  void buildUi();
  void setRunning(bool running);
  QString findTypeMap() const;
  void appendLog(const QString& line);
  void loadModuleConfig();
  void saveModuleConfig() const;

  QComboBox* mode_ = nullptr;
  QLineEdit* source_ = nullptr;
  QLineEdit* target_ = nullptr;
  QSpinBox* rowsSpin_ = nullptr;
  QSpinBox* tablesSpin_ = nullptr;
  QSpinBox* batchSpin_ = nullptr;
  QSpinBox* throttleSpin_ = nullptr;
  QSpinBox* workersSpin_ = nullptr;
  QCheckBox* verifyChk_ = nullptr;
  QCheckBox* pgCdcChk_ = nullptr;
  QCheckBox* repairChk_ = nullptr;
  QCheckBox* metricsChk_ = nullptr;
  QCheckBox* snapshotChk_ = nullptr;
  QCheckBox* resumeChk_ = nullptr;
  QPushButton* startBtn_ = nullptr;
  QListWidget* tableList_ = nullptr;
  QPushButton* loadTablesBtn_ = nullptr;
  QPushButton* allTablesBtn_ = nullptr;
  QPushButton* noneTablesBtn_ = nullptr;

  QProgressBar* progress_ = nullptr;
  QLabel* phase_ = nullptr;
  QLabel* rowsVal_ = nullptr;
  QLabel* speedVal_ = nullptr;
  QLabel* etaVal_ = nullptr;
  QLabel* errVal_ = nullptr;
  QLabel* verifyVal_ = nullptr;
  QLabel* srcDst_ = nullptr;

  SparkChart* chart_ = nullptr;
  QTableWidget* table_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QTimer* timer_ = nullptr;

  mig::Metrics* metrics_ = nullptr;
  std::thread worker_;
  bool running_ = false;
};
