#include <QApplication>

#include "mainwindow.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  app.setApplicationName("mini-migrator");
  app.setApplicationDisplayName("mini-migrator");

  MainWindow w;
  w.resize(1120, 840);
  w.show();
  return app.exec();
}
