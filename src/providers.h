#pragma once
#include <string>

namespace mig {

// 外部依赖的可配置提供者：驱动名、bcp/sqlldr 路径。
// 读取顺序：环境变量 > config/providers.ini > 内置默认值。
struct Providers {
  std::string mssql_driver = "ODBC Driver 18 for SQL Server";
  std::string oracle_driver = "Oracle 23 ODBC Driver";
  std::string bcp_path;
  std::string sqlldr_path;

  static Providers load(const std::string& path = "");
};

const Providers& providers();

}  // namespace mig
