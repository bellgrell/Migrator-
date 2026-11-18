#include "type_system.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace mig {
namespace {

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool ends_with(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

std::string base_name(std::string s) {
  s = lower(trim(s));
  // strip trailing array markers / unsigned / zerofill / parameters
  if (ends_with(s, "[]")) return s;  // caller marks array
  size_t p = s.find('(');
  if (p != std::string::npos) s = s.substr(0, p);
  size_t u = s.find(" unsigned");
  if (u != std::string::npos) s = s.substr(0, u);
  return trim(s);
}

void parse_args(const std::string& raw, int& a, int& b) {
  a = b = -1;
  size_t p = raw.find('(');
  if (p == std::string::npos) return;
  size_t q = raw.find(')', p);
  if (q == std::string::npos) return;
  std::string inside = raw.substr(p + 1, q - p - 1);
  for (char& c : inside) if (c == ',') c = ' ';
  std::istringstream is(inside);
  is >> a;
  is >> b;
}

}  // namespace

TypeDesc RuleSet::parse(const std::string& dialect, const std::string& raw) {
  TypeDesc t;
  t.dialect = dialect;
  t.raw = raw;
  std::string s = lower(trim(raw));
  t.is_array = ends_with(s, "[]");
  if (t.is_array) s = trim(s.substr(0, s.size() - 2));
  t.unsigned_ = s.find("unsigned") != std::string::npos;
  t.with_tz = s.find("with time zone") != std::string::npos ||
              s.find("timestamptz") != std::string::npos ||
              s.find("timetz") != std::string::npos;
  t.tinyint1 = lower(raw).rfind("tinyint(1)", 0) == 0;
  t.year = lower(raw).rfind("year", 0) == 0;

  int a = -1, b = -1;
  parse_args(s, a, b);
  std::string base = base_name(s);
  if (base.empty()) base = s;

  if (dialect == "mysql") {
    if (base == "tinyint") { t.family = TypeFamily::kInt; t.bits = 8; }
    else if (base == "smallint") { t.family = TypeFamily::kInt; t.bits = 16; }
    else if (base == "mediumint") { t.family = TypeFamily::kInt; t.bits = 24; }
    else if (base == "int" || base == "integer") { t.family = TypeFamily::kInt; t.bits = 32; }
    else if (base == "bigint") { t.family = TypeFamily::kInt; t.bits = 64; }
    else if (base == "year") { t.family = TypeFamily::kInt; t.bits = 16; t.year = true; }
    else if (base == "decimal" || base == "numeric") { t.family = TypeFamily::kDecimal; t.precision = a; t.scale = b; }
    else if (base == "float") { t.family = TypeFamily::kFloat; t.bits = 32; }
    else if (base == "double") { t.family = TypeFamily::kFloat; t.bits = 64; }
    else if (base == "varchar") { t.family = TypeFamily::kString; t.length = a; }
    else if (base == "char") { t.family = TypeFamily::kChar; t.length = a; }
    else if (base == "text" || base == "tinytext" || base == "mediumtext" || base == "longtext") t.family = TypeFamily::kString;
    else if (base == "binary" || base == "varbinary") { t.family = TypeFamily::kBytes; t.length = a; }
    else if (base == "blob" || base == "tinyblob" || base == "mediumblob" || base == "longblob") t.family = TypeFamily::kBytes;
    else if (base == "date") t.family = TypeFamily::kDate;
    else if (base == "time") t.family = TypeFamily::kTime;
    else if (base == "datetime") t.family = TypeFamily::kTimestamp;
    else if (base == "timestamp") { t.family = TypeFamily::kTimestamp; t.with_tz = true; }
    else if (base == "json") t.family = TypeFamily::kJson;
    else if (base == "enum") t.family = TypeFamily::kEnum;
    else if (base == "set") t.family = TypeFamily::kSet;
    else if (base == "bit") { t.family = TypeFamily::kBit; t.length = a; }
    else if (base == "geometry" || base == "point" || base == "linestring" ||
             base == "polygon" || base == "multipoint" || base == "multilinestring" ||
             base == "multipolygon" || base == "geometrycollection") t.family = TypeFamily::kGeometry;
  } else {
    if (base == "smallint" || base == "int2") { t.family = TypeFamily::kInt; t.bits = 16; }
    else if (base == "integer" || base == "int" || base == "int4") { t.family = TypeFamily::kInt; t.bits = 32; }
    else if (base == "bigint" || base == "int8") { t.family = TypeFamily::kInt; t.bits = 64; }
    else if (base == "boolean" || base == "bool") t.family = TypeFamily::kBool;
    else if (base == "numeric" || base == "decimal") { t.family = TypeFamily::kDecimal; t.precision = a; t.scale = b; }
    else if (base == "real" || base == "float4" || base == "float") { t.family = TypeFamily::kFloat; t.bits = 32; }
    else if (base == "double precision" || base == "float8") { t.family = TypeFamily::kFloat; t.bits = 64; }
    else if (base == "character varying" || base == "varchar") { t.family = TypeFamily::kString; t.length = a; }
    else if (base == "character" || base == "char") { t.family = TypeFamily::kChar; t.length = a; }
    else if (base == "text") t.family = TypeFamily::kString;
    else if (base == "bytea") t.family = TypeFamily::kBytes;
    else if (base == "date") t.family = TypeFamily::kDate;
    else if (base == "time" || base == "time without time zone" || base == "time with time zone" || base == "timetz") t.family = TypeFamily::kTime;
    else if (base == "timestamp" || base == "timestamp without time zone") t.family = TypeFamily::kTimestamp;
    else if (base == "timestamptz" || base == "timestamp with time zone") t.family = TypeFamily::kTimestampTz;
    else if (base == "json" || base == "jsonb") t.family = TypeFamily::kJson;
    else if (base == "uuid") t.family = TypeFamily::kUuid;
    else if (base == "bit") { t.family = TypeFamily::kBit; t.length = a; }
    else if (base == "bit varying" || base == "varbit") { t.family = TypeFamily::kBit; t.length = a; }
    else if (base == "inet" || base == "cidr" || base == "macaddr" || base == "macaddr8" ||
             base == "interval" || base == "xml" || base == "money" || base == "oid") t.family = TypeFamily::kString;
  }
  return t;
}

std::string RuleSet::type_name(const TypeDesc& t) {
  auto num = [](int v) { return std::to_string(v); };
  if (t.dialect == "mysql") {
    switch (t.family) {
      case TypeFamily::kInt:
        if (t.year) return "year";
        if (t.tinyint1) return "tinyint(1)";
        if (t.bits <= 8) return "tinyint";
        if (t.bits <= 16) return "smallint";
        if (t.bits <= 24) return "mediumint";
        if (t.bits <= 32) return "int";
        return "bigint";
      case TypeFamily::kDecimal: return "decimal(" + num(t.precision) + "," + num(t.scale) + ")";
      case TypeFamily::kFloat: return "double";
      case TypeFamily::kBool: return "tinyint(1)";
      case TypeFamily::kString: return t.length >= 0 ? "varchar(" + num(t.length) + ")" : "longtext";
      case TypeFamily::kChar: return t.length >= 0 ? "char(" + num(t.length) + ")" : "char(1)";
      case TypeFamily::kBytes: return "longblob";
      case TypeFamily::kDate: return "date";
      case TypeFamily::kTime: return "time(6)";
      case TypeFamily::kTimestamp: return t.with_tz ? "timestamp" : "datetime(6)";
      case TypeFamily::kJson: return "json";
      case TypeFamily::kUuid: return "char(36)";
      case TypeFamily::kBit: return t.length >= 0 ? "bit(" + num(t.length) + ")" : "bit(1)";
      default: return "longtext";
    }
  }
  switch (t.family) {
    case TypeFamily::kInt:
      if (t.bits <= 16) return "smallint";
      if (t.bits <= 32) return "integer";
      return "bigint";
    case TypeFamily::kDecimal: return "numeric(" + num(t.precision) + "," + num(t.scale) + ")";
    case TypeFamily::kFloat: return "double precision";
    case TypeFamily::kBool: return "boolean";
    case TypeFamily::kString: return t.length >= 0 ? "varchar(" + num(t.length) + ")" : "text";
    case TypeFamily::kChar: return t.length >= 0 ? "char(" + num(t.length) + ")" : "char(1)";
    case TypeFamily::kBytes: return "bytea";
    case TypeFamily::kDate: return "date";
    case TypeFamily::kTime: return t.with_tz ? "time with time zone" : "time";
    case TypeFamily::kTimestamp: return "timestamp";
    case TypeFamily::kTimestampTz: return "timestamptz";
    case TypeFamily::kJson: return "jsonb";
    case TypeFamily::kUuid: return "uuid";
    case TypeFamily::kBit: return t.length >= 0 ? "bit(" + num(t.length) + ")" : "bit(1)";
    default: return "text";
  }
}

const char* RuleSet::loss_name(LossClass l) {
  switch (l) {
    case LossClass::kExact: return "exact";
    case LossClass::kRangeLossless: return "range_lossless";
    case LossClass::kPrecisionLoss: return "precision_loss";
    case LossClass::kRepresentationOnly: return "representation_only";
    default: return "unsupported";
  }
}

void RuleSet::add(const TypeRule& r) { rules_.push_back(r); }

RuleSet::RuleSet() {
  auto R = [&](const char* id, const char* sd, const char* dd, TypeFamily sf,
               int bits_min, int bits_max, int uns, int tiny1, int yr, int tz,
               TypeFamily df, LossClass loss, const char* guard, const char* witness,
               const char* verify, int prio) {
    TypeRule r;
    r.id = id; r.src_dialect = sd; r.dst_dialect = dd; r.src_family = sf;
    r.src_bits_min = bits_min; r.src_bits_max = bits_max;
    r.src_unsigned = uns; r.src_tinyint1 = tiny1; r.src_year = yr; r.src_with_tz = tz;
    r.dst.dialect = dd; r.dst.family = df;
    r.loss = loss; r.guard = guard; r.witness = witness; r.verification = verify;
    r.priority = prio;
    add(r);
  };

  // MySQL -> PostgreSQL/openGauss
  R("mysql.int.small->pg.smallint", "mysql", "pg", TypeFamily::kInt, 0, 16, 0, 0, 0, -1, TypeFamily::kInt, LossClass::kExact, "range fits", "32767", "decimal_text", 10);
  R("mysql.int.32->pg.integer", "mysql", "pg", TypeFamily::kInt, 17, 32, 0, 0, 0, -1, TypeFamily::kInt, LossClass::kExact, "range fits", "2147483647", "decimal_text", 10);
  R("mysql.int.64->pg.bigint", "mysql", "pg", TypeFamily::kInt, 33, 64, 0, 0, 0, -1, TypeFamily::kInt, LossClass::kExact, "range fits", "9223372036854775807", "decimal_text", 10);
  R("mysql.tinyint1->pg.smallint", "mysql", "pg", TypeFamily::kInt, 0, 8, 0, 1, 0, -1, TypeFamily::kInt, LossClass::kExact, "0/1 or -128..127", "1", "decimal_text", 15);
  R("mysql.uint.32->pg.bigint", "mysql", "pg", TypeFamily::kInt, 0, 32, 1, 0, 0, -1, TypeFamily::kInt, LossClass::kRangeLossless, "0..2^32-1", "4294967295", "decimal_text", 20);
  R("mysql.uint.64->pg.numeric20", "mysql", "pg", TypeFamily::kInt, 33, 64, 1, 0, 0, -1, TypeFamily::kDecimal, LossClass::kRangeLossless, "0..2^64-1", "18446744073709551615", "decimal_text", 20);
  R("mysql.year->pg.smallint", "mysql", "pg", TypeFamily::kInt, 0, 64, 0, 0, 1, -1, TypeFamily::kInt, LossClass::kExact, "1901..2155", "2155", "decimal_text", 30);
  R("mysql.decimal->pg.numeric", "mysql", "pg", TypeFamily::kDecimal, -1, -1, -1, 0, 0, -1, TypeFamily::kDecimal, LossClass::kExact, "p<=1000", "999999999999999999", "decimal_text", 10);
  R("mysql.float->pg.real", "mysql", "pg", TypeFamily::kFloat, -1, -1, -1, 0, 0, -1, TypeFamily::kFloat, LossClass::kPrecisionLoss, "binary32", "0.1", "epsilon", 5);
  R("mysql.double->pg.double", "mysql", "pg", TypeFamily::kFloat, 64, 64, -1, 0, 0, -1, TypeFamily::kFloat, LossClass::kExact, "binary64", "0.1", "epsilon", 15);
  R("mysql.varchar->pg.varchar", "mysql", "pg", TypeFamily::kString, -1, -1, -1, 0, 0, -1, TypeFamily::kString, LossClass::kExact, "charset utf8mb4", "正常", "text", 10);
  R("mysql.char->pg.char", "mysql", "pg", TypeFamily::kChar, -1, -1, -1, 0, 0, -1, TypeFamily::kChar, LossClass::kRepresentationOnly, "trailing spaces", "a  ", "rtrim", 10);
  R("mysql.text->pg.text", "mysql", "pg", TypeFamily::kString, -1, -1, -1, 0, 0, -1, TypeFamily::kString, LossClass::kExact, "charset utf8mb4", "a,b", "text", 5);
  R("mysql.binary->pg.bytea", "mysql", "pg", TypeFamily::kBytes, -1, -1, -1, 0, 0, -1, TypeFamily::kBytes, LossClass::kExact, "raw bytes", "00ff", "hex", 10);
  R("mysql.date->pg.date", "mysql", "pg", TypeFamily::kDate, -1, -1, -1, 0, 0, -1, TypeFamily::kDate, LossClass::kExact, "no 0000-00-00", "2024-01-01", "date", 10);
  R("mysql.datetime->pg.timestamp", "mysql", "pg", TypeFamily::kTimestamp, -1, -1, -1, 0, 0, 0, TypeFamily::kTimestamp, LossClass::kExact, "no zero date, scale<=6", "2024-01-01 00:00:00", "timestamp", 10);
  R("mysql.timestamp->pg.timestamptz", "mysql", "pg", TypeFamily::kTimestamp, -1, -1, -1, 0, 0, 1, TypeFamily::kTimestampTz, LossClass::kRepresentationOnly, "session TZ=UTC", "2024-01-01 00:00:00", "timestamp", 10);
  R("mysql.time->pg.time", "mysql", "pg", TypeFamily::kTime, -1, -1, -1, 0, 0, -1, TypeFamily::kTime, LossClass::kExact, "-838..838h", "12:00:00", "time", 10);
  R("mysql.json->pg.jsonb", "mysql", "pg", TypeFamily::kJson, -1, -1, -1, 0, 0, -1, TypeFamily::kJson, LossClass::kPrecisionLoss, "dup keys/numbers", "{\"n\":1.0}", "json_canonical", 5);
  R("mysql.enum->pg.text", "mysql", "pg", TypeFamily::kEnum, -1, -1, -1, 0, 0, -1, TypeFamily::kString, LossClass::kRepresentationOnly, "order not preserved", "'a'", "text", 10);
  R("mysql.set->pg.text", "mysql", "pg", TypeFamily::kSet, -1, -1, -1, 0, 0, -1, TypeFamily::kString, LossClass::kRepresentationOnly, "set order not preserved", "'x,y'", "text", 10);
  R("mysql.bit->pg.bytea", "mysql", "pg", TypeFamily::kBit, -1, -1, -1, 0, 0, -1, TypeFamily::kBytes, LossClass::kRepresentationOnly, "no native bit ops", "10101010", "hex", 10);
  R("mysql.geometry->pg.bytea", "mysql", "pg", TypeFamily::kGeometry, -1, -1, -1, 0, 0, -1, TypeFamily::kBytes, LossClass::kRepresentationOnly, "SRID/WKB bytes only", "POINT(1 2)", "hex", 10);

  // PostgreSQL -> MySQL
  R("pg.int.small->mysql.smallint", "pg", "mysql", TypeFamily::kInt, 0, 16, -1, 0, 0, -1, TypeFamily::kInt, LossClass::kExact, "range fits", "32767", "decimal_text", 10);
  R("pg.int.32->mysql.int", "pg", "mysql", TypeFamily::kInt, 17, 32, -1, 0, 0, -1, TypeFamily::kInt, LossClass::kExact, "range fits", "2147483647", "decimal_text", 10);
  R("pg.int.64->mysql.bigint", "pg", "mysql", TypeFamily::kInt, 33, 64, -1, 0, 0, -1, TypeFamily::kInt, LossClass::kExact, "range fits", "9223372036854775807", "decimal_text", 10);
  R("pg.bool->mysql.tinyint1", "pg", "mysql", TypeFamily::kBool, -1, -1, -1, 0, 0, -1, TypeFamily::kBool, LossClass::kExact, "0/1", "true", "bool", 10);
  R("pg.decimal->mysql.decimal", "pg", "mysql", TypeFamily::kDecimal, -1, -1, -1, 0, 0, -1, TypeFamily::kDecimal, LossClass::kPrecisionLoss, "p<=65,s<=30", "12345678901234567890", "decimal_text", 5);
  R("pg.varchar->mysql.varchar", "pg", "mysql", TypeFamily::kString, -1, -1, -1, 0, 0, -1, TypeFamily::kString, LossClass::kExact, "utf8mb4", "正常", "text", 10);
  R("pg.char->mysql.char", "pg", "mysql", TypeFamily::kChar, -1, -1, -1, 0, 0, -1, TypeFamily::kChar, LossClass::kRepresentationOnly, "trailing spaces", "a  ", "rtrim", 10);
  R("pg.text->mysql.longtext", "pg", "mysql", TypeFamily::kString, -1, -1, -1, 0, 0, -1, TypeFamily::kString, LossClass::kExact, "utf8mb4", "a,b", "text", 5);
  R("pg.bytea->mysql.longblob", "pg", "mysql", TypeFamily::kBytes, -1, -1, -1, 0, 0, -1, TypeFamily::kBytes, LossClass::kExact, "raw bytes", "00ff", "hex", 10);
  R("pg.date->mysql.date", "pg", "mysql", TypeFamily::kDate, -1, -1, -1, 0, 0, -1, TypeFamily::kDate, LossClass::kExact, "year 1000..9999", "2024-01-01", "date", 10);
  R("pg.timestamp->mysql.datetime", "pg", "mysql", TypeFamily::kTimestamp, -1, -1, -1, 0, 0, 0, TypeFamily::kTimestamp, LossClass::kExact, "scale<=6", "2024-01-01 00:00:00", "timestamp", 10);
  R("pg.timestamptz->mysql.datetime", "pg", "mysql", TypeFamily::kTimestampTz, -1, -1, -1, 0, 0, 1, TypeFamily::kTimestamp, LossClass::kRepresentationOnly, "UTC session", "2024-01-01 00:00:00+00", "timestamp", 10);
  R("pg.jsonb->mysql.json", "pg", "mysql", TypeFamily::kJson, -1, -1, -1, 0, 0, -1, TypeFamily::kJson, LossClass::kPrecisionLoss, "numbers > 53-bit", "{\"n\":1.0}", "json_canonical", 5);
  R("pg.uuid->mysql.char36", "pg", "mysql", TypeFamily::kUuid, -1, -1, -1, 0, 0, -1, TypeFamily::kUuid, LossClass::kExact, "canonical text", "550e8400-e29b-41d4-a716-446655440000", "uuid_text", 10);
  R("pg.bit->mysql.bit", "pg", "mysql", TypeFamily::kBit, -1, -1, -1, 0, 0, -1, TypeFamily::kBit, LossClass::kExact, "length preserved", "10101010", "hex", 10);
}

TypeResolution RuleSet::resolve(const std::string& src_dialect,
                               const std::string& src_raw,
                               const std::string& dst_dialect) const {
  TypeResolution out;
  out.src = parse(src_dialect, src_raw);
  if (out.src.is_array) {
    out.note = "array types are unsupported";
    return out;
  }
  const TypeRule* best = nullptr;
  for (const auto& r : rules_) {
    if (r.src_dialect != out.src.dialect || r.dst_dialect != dst_dialect) continue;
    if (r.src_family != out.src.family) continue;
    if (r.src_bits_min >= 0 && out.src.bits < r.src_bits_min) continue;
    if (r.src_bits_max >= 0 && out.src.bits > r.src_bits_max) continue;
    if (r.src_unsigned >= 0 && (out.src.unsigned_ ? 1 : 0) != r.src_unsigned) continue;
    if (r.src_tinyint1 >= 0 && (out.src.tinyint1 ? 1 : 0) != r.src_tinyint1) continue;
    if (r.src_year >= 0 && (out.src.year ? 1 : 0) != r.src_year) continue;
    if (r.src_with_tz >= 0 && (out.src.with_tz ? 1 : 0) != r.src_with_tz) continue;
    if (!best || r.priority > best->priority) best = &r;
  }
  if (!best) {
    out.note = "no rule for " + src_dialect + " " + src_raw + " -> " + dst_dialect;
    return out;
  }
  out.ok = true;
  out.rule_id = best->id;
  out.loss = best->loss;
  out.guard = best->guard;
  out.witness = best->witness;
  out.verification = best->verification;
  out.dst = best->dst;
  out.dst.dialect = dst_dialect;
  if (out.dst.family == TypeFamily::kInt && out.dst.bits == 0) {
    if (best->id.find("bigint") != std::string::npos) out.dst.bits = 64;
    else if (best->id.find("smallint") != std::string::npos) out.dst.bits = 16;
    else out.dst.bits = 32;
  }
  if (out.dst.length < 0) out.dst.length = out.src.length;
  if (out.dst.precision < 0) {
    if (out.dst.family == TypeFamily::kDecimal &&
        best->id.find("numeric20") != std::string::npos) {
      out.dst.precision = 20;
      out.dst.scale = 0;
    } else {
      out.dst.precision = out.src.precision;
      out.dst.scale = out.src.scale;
    }
  }
  if (out.dst.family == TypeFamily::kDecimal && out.dst.precision > 65) {
    out.loss = LossClass::kPrecisionLoss;
    out.note = "target decimal precision limit is 65";
  }
  if (out.dst.family == TypeFamily::kString && out.dst.length > 0 &&
      out.src.length > 0 && out.src.length > out.dst.length) {
    out.loss = LossClass::kPrecisionLoss;
    out.note = "string length would truncate";
  }
  return out;
}

std::vector<std::string> RuleSet::validate() const {
  std::vector<std::string> issues;
  std::vector<std::string> ids;
  for (const auto& r : rules_) {
    if (std::find(ids.begin(), ids.end(), r.id) != ids.end()) {
      issues.push_back("duplicate rule id: " + r.id);
    } else {
      ids.push_back(r.id);
    }
    if (r.dst.family == TypeFamily::kUnknown)
      issues.push_back("rule with unknown target family: " + r.id);
    if (r.witness.empty())
      issues.push_back("rule without witness: " + r.id);
  }
  return issues;
}

std::vector<std::string> RuleSet::dump_tests() const {
  std::vector<std::string> lines;
  for (const auto& r : rules_) {
    lines.push_back(r.id + "\t" + r.src_dialect + "\t" + r.dst_dialect +
                    "\t" + r.witness + "\t" + r.guard + "\t" + r.verification);
  }
  return lines;
}

const char* RuleSet::family_name(TypeFamily f) {
  switch (f) {
    case TypeFamily::kInt: return "int";
    case TypeFamily::kDecimal: return "decimal";
    case TypeFamily::kFloat: return "float";
    case TypeFamily::kBool: return "bool";
    case TypeFamily::kString: return "string";
    case TypeFamily::kChar: return "char";
    case TypeFamily::kBytes: return "bytes";
    case TypeFamily::kDate: return "date";
    case TypeFamily::kTime: return "time";
    case TypeFamily::kTimestamp: return "timestamp";
    case TypeFamily::kTimestampTz: return "timestamptz";
    case TypeFamily::kJson: return "json";
    case TypeFamily::kUuid: return "uuid";
    case TypeFamily::kEnum: return "enum";
    case TypeFamily::kSet: return "set";
    case TypeFamily::kBit: return "bit";
    case TypeFamily::kGeometry: return "geometry";
    default: return "unknown";
  }
}

TypeFamily RuleSet::family_from(const std::string& s) {
  if (s == "int") return TypeFamily::kInt;
  if (s == "decimal") return TypeFamily::kDecimal;
  if (s == "float") return TypeFamily::kFloat;
  if (s == "bool") return TypeFamily::kBool;
  if (s == "string") return TypeFamily::kString;
  if (s == "char") return TypeFamily::kChar;
  if (s == "bytes") return TypeFamily::kBytes;
  if (s == "date") return TypeFamily::kDate;
  if (s == "time") return TypeFamily::kTime;
  if (s == "timestamp") return TypeFamily::kTimestamp;
  if (s == "timestamptz") return TypeFamily::kTimestampTz;
  if (s == "json") return TypeFamily::kJson;
  if (s == "uuid") return TypeFamily::kUuid;
  if (s == "enum") return TypeFamily::kEnum;
  if (s == "set") return TypeFamily::kSet;
  if (s == "bit") return TypeFamily::kBit;
  if (s == "geometry") return TypeFamily::kGeometry;
  return TypeFamily::kUnknown;
}

LossClass RuleSet::loss_from(const std::string& s) {
  if (s == "exact") return LossClass::kExact;
  if (s == "range_lossless") return LossClass::kRangeLossless;
  if (s == "precision_loss") return LossClass::kPrecisionLoss;
  if (s == "representation_only") return LossClass::kRepresentationOnly;
  return LossClass::kUnsupported;
}

namespace {
std::string sanitize_tsv(std::string s) {
  for (char& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  return s;
}

std::string sample_src_type(const TypeRule& r) {
  std::string u = (r.src_unsigned == 1) ? " unsigned" : "";
  if (r.src_dialect == "mysql") {
    switch (r.src_family) {
      case TypeFamily::kInt:
        if (r.src_year == 1) return "year";
        if (r.src_bits_min <= 8 && r.src_bits_max >= 8) return "tinyint" + u;
        if (r.src_bits_min <= 16 && r.src_bits_max >= 16) return "smallint" + u;
        if (r.src_bits_min <= 24 && r.src_bits_max >= 24) return "mediumint" + u;
        if (r.src_bits_min <= 32 && r.src_bits_max >= 32) return "int" + u;
        return "bigint" + u;
      case TypeFamily::kDecimal: return "decimal(10,2)";
      case TypeFamily::kFloat: return "double";
      case TypeFamily::kString: return "varchar(64)";
      case TypeFamily::kChar: return "char(8)";
      case TypeFamily::kBytes: return "blob";
      case TypeFamily::kDate: return "date";
      case TypeFamily::kTime: return "time";
      case TypeFamily::kTimestamp: return "datetime";
      case TypeFamily::kJson: return "json";
      case TypeFamily::kEnum: return "enum('a','b')";
      case TypeFamily::kSet: return "set('x','y')";
      case TypeFamily::kBit: return "bit(8)";
      case TypeFamily::kGeometry: return "geometry";
      default: return "int";
    }
  }
  switch (r.src_family) {
    case TypeFamily::kInt:
      if (r.src_bits_min <= 16) return "smallint";
      if (r.src_bits_min <= 32) return "integer";
      return "bigint";
    case TypeFamily::kDecimal: return "numeric(10,2)";
    case TypeFamily::kFloat: return "double precision";
    case TypeFamily::kBool: return "boolean";
    case TypeFamily::kString: return "varchar(64)";
    case TypeFamily::kChar: return "char(8)";
    case TypeFamily::kBytes: return "bytea";
    case TypeFamily::kDate: return "date";
    case TypeFamily::kTime: return "time";
    case TypeFamily::kTimestamp: return "timestamp";
    case TypeFamily::kTimestampTz: return "timestamptz";
    case TypeFamily::kJson: return "jsonb";
    case TypeFamily::kUuid: return "uuid";
    case TypeFamily::kBit: return "bit(8)";
    default: return "integer";
  }
}

std::string dst_type_name(const TypeRule& r) {
  TypeDesc d = r.dst;
  if (d.family == TypeFamily::kInt && d.bits == 0) {
    if (r.id.find("bigint") != std::string::npos) d.bits = 64;
    else if (r.id.find("smallint") != std::string::npos) d.bits = 16;
    else d.bits = 32;
  }
  if (d.family == TypeFamily::kDecimal && d.precision < 0 &&
      r.id.find("numeric20") != std::string::npos) {
    d.precision = 20;
    d.scale = 0;
  }
  return RuleSet::type_name(d);
}
}  // namespace

bool RuleSet::save_tsv(const std::string& path, std::string& err) const {
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    err = "cannot write type rules: " + path;
    return false;
  }
  f << "id\tsrc_dialect\tdst_dialect\tsrc_family\tbits_min\tbits_max"
       "\tunsigned\ttinyint1\tyear\ttz\tdst_family\tdst_bits"
       "\tdst_precision\tdst_scale\tdst_length\tloss\tpriority"
       "\tguard\twitness\tverification\n";
  for (const auto& r : rules_) {
    f << sanitize_tsv(r.id) << '\t' << r.src_dialect << '\t' << r.dst_dialect
      << '\t' << family_name(r.src_family) << '\t' << r.src_bits_min << '\t'
      << r.src_bits_max << '\t' << r.src_unsigned << '\t' << r.src_tinyint1
      << '\t' << r.src_year << '\t' << r.src_with_tz << '\t'
      << family_name(r.dst.family) << '\t' << r.dst.bits << '\t'
      << r.dst.precision << '\t' << r.dst.scale << '\t' << r.dst.length
      << '\t' << loss_name(r.loss) << '\t' << r.priority << '\t'
      << sanitize_tsv(r.guard) << '\t' << sanitize_tsv(r.witness) << '\t'
      << sanitize_tsv(r.verification) << '\n';
  }
  return true;
}

bool RuleSet::load_tsv(const std::string& path, std::string& err) {
  std::ifstream f(path);
  if (!f) {
    err = "cannot read type rules: " + path;
    return false;
  }
  std::vector<TypeRule> loaded;
  std::string line;
  std::getline(f, line);  // header
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    std::vector<std::string> cols;
    std::string cur;
    for (char c : line) {
      if (c == '\t') { cols.push_back(cur); cur.clear(); }
      else cur.push_back(c);
    }
    cols.push_back(cur);
    if (cols.size() < 20) {
      err = "bad type rule line: " + line;
      return false;
    }
    TypeRule r;
    r.id = cols[0];
    r.src_dialect = cols[1];
    r.dst_dialect = cols[2];
    r.src_family = family_from(cols[3]);
    r.src_bits_min = std::atoi(cols[4].c_str());
    r.src_bits_max = std::atoi(cols[5].c_str());
    r.src_unsigned = std::atoi(cols[6].c_str());
    r.src_tinyint1 = std::atoi(cols[7].c_str());
    r.src_year = std::atoi(cols[8].c_str());
    r.src_with_tz = std::atoi(cols[9].c_str());
    r.dst.dialect = r.dst_dialect;
    r.dst.family = family_from(cols[10]);
    r.dst.bits = std::atoi(cols[11].c_str());
    r.dst.precision = std::atoi(cols[12].c_str());
    r.dst.scale = std::atoi(cols[13].c_str());
    r.dst.length = std::atoi(cols[14].c_str());
    r.loss = loss_from(cols[15]);
    r.priority = std::atoi(cols[16].c_str());
    r.guard = cols[17];
    r.witness = cols[18];
    r.verification = cols[19];
    loaded.push_back(std::move(r));
  }
  if (loaded.empty()) {
    err = "type rule file is empty: " + path;
    return false;
  }
  rules_.swap(loaded);
  return true;
}

std::string RuleSet::stats() const {
  std::map<std::string, int> counts;
  for (const auto& r : rules_) {
    ++counts[r.dst_dialect + ":" + loss_name(r.loss)];
  }
  std::ostringstream out;
  out << "rules=" << rules_.size();
  for (const auto& kv : counts) out << " " << kv.first << "=" << kv.second;
  return out.str();
}

bool RuleSet::emit_test_manifest(const std::string& dir, std::string& err) const {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    err = "cannot create " + dir + ": " + ec.message();
    return false;
  }
  const std::string tsv = dir + "/type_rule_cases.tsv";
  std::ofstream f(tsv, std::ios::trunc);
  if (!f) {
    err = "cannot write " + tsv;
    return false;
  }
  f << "rule\tsrc_dialect\tsrc_type\tdst_dialect\tdst_type\twitness"
       "\tloss\tguard\tverification\n";
  for (const auto& r : rules_) {
    f << sanitize_tsv(r.id) << '\t' << r.src_dialect << '\t'
      << sample_src_type(r) << '\t' << r.dst_dialect << '\t'
      << dst_type_name(r) << '\t' << sanitize_tsv(r.witness) << '\t'
      << loss_name(r.loss) << '\t' << sanitize_tsv(r.guard) << '\t'
      << sanitize_tsv(r.verification) << '\n';
  }
  const std::string readme = dir + "/README.md";
  std::ofstream md(readme, std::ios::trunc);
  if (md) {
    md << "# Type rule test cases\n\n"
       << "Generated from the type rule set. Each row is a boundary case\n"
       << "that should be exercised by a migration + verification test.\n\n"
       << "Use the rule id with --explain-types to see the resolved target.\n";
  }
  return true;
}

// ---------------------------------------------------------------- JSON I/O

namespace {

struct JVal {
  enum Kind { kNull, kBool, kNum, kStr, kArr, kObj } kind = kNull;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<JVal> arr;
  std::map<std::string, JVal> obj;
};

class JParser {
 public:
  explicit JParser(const std::string& s) : s_(s) {}

  bool parse(JVal& out, std::string& err) {
    skip();
    if (!value(out, err)) return false;
    skip();
    if (pos_ != s_.size()) {
      err = "trailing data at byte " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  void skip() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' ||
            s_[pos_] == '\n'))
      ++pos_;
  }

  bool value(JVal& out, std::string& err) {
    skip();
    if (pos_ >= s_.size()) {
      err = "unexpected end of JSON";
      return false;
    }
    const char c = s_[pos_];
    if (c == '{') return object(out, err);
    if (c == '[') return array(out, err);
    if (c == '"') {
      out.kind = JVal::kStr;
      return string(out.str, err);
    }
    if (c == 't' || c == 'f' || c == 'n') return literal(out, err);
    return number(out, err);
  }

  bool object(JVal& out, std::string& err) {
    out.kind = JVal::kObj;
    ++pos_;  // {
    skip();
    if (pos_ < s_.size() && s_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      skip();
      std::string key;
      if (pos_ >= s_.size() || s_[pos_] != '"') {
        err = "expected object key at byte " + std::to_string(pos_);
        return false;
      }
      if (!string(key, err)) return false;
      skip();
      if (pos_ >= s_.size() || s_[pos_] != ':') {
        err = "expected ':' at byte " + std::to_string(pos_);
        return false;
      }
      ++pos_;
      JVal v;
      if (!value(v, err)) return false;
      out.obj[key] = std::move(v);
      skip();
      if (pos_ >= s_.size()) {
        err = "unterminated object";
        return false;
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == '}') {
        ++pos_;
        return true;
      }
      err = "expected ',' or '}' at byte " + std::to_string(pos_);
      return false;
    }
  }

  bool array(JVal& out, std::string& err) {
    out.kind = JVal::kArr;
    ++pos_;  // [
    skip();
    if (pos_ < s_.size() && s_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      JVal v;
      if (!value(v, err)) return false;
      out.arr.push_back(std::move(v));
      skip();
      if (pos_ >= s_.size()) {
        err = "unterminated array";
        return false;
      }
      if (s_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (s_[pos_] == ']') {
        ++pos_;
        return true;
      }
      err = "expected ',' or ']' at byte " + std::to_string(pos_);
      return false;
    }
  }

  static void append_utf8(std::string& out, unsigned cp) {
    if (cp <= 0x7f) {
      out.push_back((char)cp);
    } else if (cp <= 0x7ff) {
      out.push_back((char)(0xc0 | (cp >> 6)));
      out.push_back((char)(0x80 | (cp & 0x3f)));
    } else {
      out.push_back((char)(0xe0 | (cp >> 12)));
      out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back((char)(0x80 | (cp & 0x3f)));
    }
  }

  bool string(std::string& out, std::string& err) {
    ++pos_;  // opening quote
    out.clear();
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) break;
      const char e = s_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > s_.size()) {
            err = "bad \\u escape";
            return false;
          }
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
            else {
              err = "bad \\u escape";
              return false;
            }
          }
          append_utf8(out, cp);
          break;
        }
        default:
          err = "bad escape";
          return false;
      }
    }
    err = "unterminated string";
    return false;
  }

  bool number(JVal& out, std::string& err) {
    const size_t start = pos_;
    if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
    while (pos_ < s_.size() &&
           (std::isdigit((unsigned char)s_[pos_]) || s_[pos_] == '.' ||
            s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '-' ||
            s_[pos_] == '+'))
      ++pos_;
    if (start == pos_) {
      err = "expected value at byte " + std::to_string(pos_);
      return false;
    }
    out.kind = JVal::kNum;
    out.num = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
    return true;
  }

  bool literal(JVal& out, std::string& err) {
    if (s_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      out.kind = JVal::kBool;
      out.b = true;
      return true;
    }
    if (s_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      out.kind = JVal::kBool;
      out.b = false;
      return true;
    }
    if (s_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      out.kind = JVal::kNull;
      return true;
    }
    err = "bad literal at byte " + std::to_string(pos_);
    return false;
  }

  const std::string& s_;
  size_t pos_ = 0;
};

std::string jstr(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  out.push_back('"');
  return out;
}

const JVal* get(const JVal& o, const char* key) {
  if (o.kind != JVal::kObj) return nullptr;
  const auto it = o.obj.find(key);
  return it == o.obj.end() ? nullptr : &it->second;
}

std::string str_of(const JVal& o, const char* key, const std::string& def = "") {
  const JVal* v = get(o, key);
  if (!v) return def;
  if (v->kind == JVal::kStr) return v->str;
  if (v->kind == JVal::kNum) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.0f", v->num);
    return buf;
  }
  if (v->kind == JVal::kBool) return v->b ? "true" : "false";
  return def;
}

int int_of(const JVal& o, const char* key, int def) {
  const JVal* v = get(o, key);
  if (!v) return def;
  if (v->kind == JVal::kNum) return (int)v->num;
  if (v->kind == JVal::kBool) return v->b ? 1 : 0;
  if (v->kind == JVal::kStr) return std::atoi(v->str.c_str());
  return def;
}

}  // namespace

bool RuleSet::save_json(const std::string& path, std::string& err) const {
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    err = "cannot write type rules: " + path;
    return false;
  }
  f << "{\n  \"version\": 1,\n  \"rules\": [\n";
  for (size_t i = 0; i < rules_.size(); ++i) {
    const auto& r = rules_[i];
    f << "    {\n";
    f << "      \"id\": " << jstr(r.id) << ",\n";
    f << "      \"src_dialect\": " << jstr(r.src_dialect) << ",\n";
    f << "      \"dst_dialect\": " << jstr(r.dst_dialect) << ",\n";
    f << "      \"src_family\": " << jstr(family_name(r.src_family)) << ",\n";
    f << "      \"src_bits_min\": " << r.src_bits_min << ",\n";
    f << "      \"src_bits_max\": " << r.src_bits_max << ",\n";
    f << "      \"src_unsigned\": " << r.src_unsigned << ",\n";
    f << "      \"src_tinyint1\": " << r.src_tinyint1 << ",\n";
    f << "      \"src_year\": " << r.src_year << ",\n";
    f << "      \"src_with_tz\": " << r.src_with_tz << ",\n";
    f << "      \"dst_family\": " << jstr(family_name(r.dst.family)) << ",\n";
    f << "      \"dst_bits\": " << r.dst.bits << ",\n";
    f << "      \"dst_precision\": " << r.dst.precision << ",\n";
    f << "      \"dst_scale\": " << r.dst.scale << ",\n";
    f << "      \"dst_length\": " << r.dst.length << ",\n";
    f << "      \"loss\": " << jstr(loss_name(r.loss)) << ",\n";
    f << "      \"priority\": " << r.priority << ",\n";
    f << "      \"guard\": " << jstr(r.guard) << ",\n";
    f << "      \"witness\": " << jstr(r.witness) << ",\n";
    f << "      \"verification\": " << jstr(r.verification) << "\n";
    f << "    }" << (i + 1 == rules_.size() ? "" : ",") << "\n";
  }
  f << "  ]\n}\n";
  return true;
}

bool RuleSet::load_json(const std::string& path, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "cannot read type rules: " + path;
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  JVal root;
  JParser parser(text);
  if (!parser.parse(root, err)) {
    err = path + ": " + err;
    return false;
  }
  const JVal* list = nullptr;
  if (root.kind == JVal::kArr) {
    list = &root;
  } else if (root.kind == JVal::kObj) {
    list = get(root, "rules");
  }
  if (!list || list->kind != JVal::kArr) {
    err = path + ": expected an array of rules (or {\"rules\": [...]})";
    return false;
  }
  std::vector<TypeRule> loaded;
  for (size_t i = 0; i < list->arr.size(); ++i) {
    const JVal& o = list->arr[i];
    if (o.kind != JVal::kObj) {
      err = path + ": rule #" + std::to_string(i) + " is not an object";
      return false;
    }
    TypeRule r;
    r.id = str_of(o, "id");
    r.src_dialect = str_of(o, "src_dialect");
    r.dst_dialect = str_of(o, "dst_dialect");
    r.src_family = family_from(str_of(o, "src_family"));
    r.src_bits_min = int_of(o, "src_bits_min", -1);
    r.src_bits_max = int_of(o, "src_bits_max", -1);
    r.src_unsigned = int_of(o, "src_unsigned", -1);
    r.src_tinyint1 = int_of(o, "src_tinyint1", -1);
    r.src_year = int_of(o, "src_year", -1);
    r.src_with_tz = int_of(o, "src_with_tz", -1);
    const JVal* dst = get(o, "dst");
    const JVal& d = (dst && dst->kind == JVal::kObj) ? *dst : o;
    r.dst.dialect = r.dst_dialect;
    r.dst.family = family_from(str_of(d, "family", str_of(d, "dst_family")));
    r.dst.bits = int_of(d, "bits", int_of(d, "dst_bits", 0));
    r.dst.precision = int_of(d, "precision", int_of(d, "dst_precision", -1));
    r.dst.scale = int_of(d, "scale", int_of(d, "dst_scale", -1));
    r.dst.length = int_of(d, "length", int_of(d, "dst_length", -1));
    r.loss = loss_from(str_of(o, "loss"));
    r.priority = int_of(o, "priority", 0);
    r.guard = str_of(o, "guard");
    r.witness = str_of(o, "witness");
    r.verification = str_of(o, "verification");
    if (r.id.empty()) {
      err = path + ": rule #" + std::to_string(i) + " has no id";
      return false;
    }
    loaded.push_back(std::move(r));
  }
  if (loaded.empty()) {
    err = "type rule file is empty: " + path;
    return false;
  }
  rules_.swap(loaded);
  return true;
}

bool RuleSet::load_file(const std::string& path, std::string& err) {
  const bool json = path.size() >= 5 &&
                    lower(path.substr(path.size() - 5)) == ".json";
  return json ? load_json(path, err) : load_tsv(path, err);
}

bool RuleSet::save_file(const std::string& path, std::string& err) const {
  const bool json = path.size() >= 5 &&
                    lower(path.substr(path.size() - 5)) == ".json";
  return json ? save_json(path, err) : save_tsv(path, err);
}

// ---------------------------------------------------------------- proof

namespace {
long double numeric_bound(const TypeRule& r) {
  if (r.src_family != TypeFamily::kInt) return -1;
  const int bits = r.src_bits_max >= 0 ? r.src_bits_max : r.src_bits_min;
  if (bits <= 0) return -1;
  long double v = 1;
  if (r.src_unsigned == 1) {
    for (int i = 0; i < bits; ++i) v *= 2;
    return v - 1;
  }
  if (bits > 62) return 9223372036854775807.0L;
  long double p = 1;
  for (int i = 0; i < bits - 1; ++i) p *= 2;
  return p - 1;
}

bool ranges_overlap(int a_min, int a_max, int b_min, int b_max) {
  const int lo = std::max(a_min < 0 ? 0 : a_min, b_min < 0 ? 0 : b_min);
  const int hi = std::min(a_max < 0 ? 1 << 30 : a_max, b_max < 0 ? 1 << 30 : b_max);
  return lo <= hi;
}

bool same_match(const TypeRule& a, const TypeRule& b) {
  return a.src_dialect == b.src_dialect && a.dst_dialect == b.dst_dialect &&
         a.src_family == b.src_family &&
         ranges_overlap(a.src_bits_min, a.src_bits_max, b.src_bits_min,
                        b.src_bits_max) &&
         (a.src_unsigned < 0 || b.src_unsigned < 0 ||
          a.src_unsigned == b.src_unsigned) &&
         (a.src_tinyint1 < 0 || b.src_tinyint1 < 0 ||
          a.src_tinyint1 == b.src_tinyint1) &&
         (a.src_year < 0 || b.src_year < 0 || a.src_year == b.src_year) &&
         (a.src_with_tz < 0 || b.src_with_tz < 0 ||
          a.src_with_tz == b.src_with_tz);
}
}  // namespace

std::vector<std::string> RuleSet::prove() const {
  std::vector<std::string> out;
  auto pass = [&](const std::string& id, const std::string& msg) {
    out.push_back("PASS " + id + " " + msg);
  };
  auto fail = [&](const std::string& id, const std::string& msg) {
    out.push_back("FAIL " + id + " " + msg);
  };
  auto warn = [&](const std::string& id, const std::string& msg) {
    out.push_back("WARN " + id + " " + msg);
  };

  // P1: ids are present and unique.
  {
    bool ok = true;
    std::map<std::string, int> seen;
    for (const auto& r : rules_) {
      if (r.id.empty()) {
        ok = false;
        fail("P1", "rule with empty id");
      }
      if (++seen[r.id] > 1) {
        ok = false;
        fail("P1", "duplicate id " + r.id);
      }
    }
    if (ok) pass("P1", "unique ids (" + std::to_string(rules_.size()) + " rules)");
  }

  // P2: dialects and families are known.
  {
    bool ok = true;
    for (const auto& r : rules_) {
      if (r.src_dialect != "mysql" && r.src_dialect != "pg") {
        ok = false;
        fail("P2", "unknown source dialect on " + r.id);
      }
      if (r.dst_dialect != "mysql" && r.dst_dialect != "pg") {
        ok = false;
        fail("P2", "unknown target dialect on " + r.id);
      }
      if (r.src_family == TypeFamily::kUnknown) {
        ok = false;
        fail("P2", "unknown source family on " + r.id);
      }
      if (r.dst.family == TypeFamily::kUnknown) {
        ok = false;
        fail("P2", "unknown target family on " + r.id);
      }
    }
    if (ok) pass("P2", "all dialects/families known");
  }

  // P3: every rule carries a witness and a verification method.
  {
    int missing = 0;
    for (const auto& r : rules_) {
      if (r.witness.empty()) {
        ++missing;
        fail("P3", "no witness: " + r.id);
      }
      if (r.verification.empty()) {
        ++missing;
        fail("P3", "no verification: " + r.id);
      }
    }
    if (missing == 0) pass("P3", "witness + verification present on every rule");
  }

  // P4: exact claims must fit the target capacity.
  {
    int bad = 0;
    for (const auto& r : rules_) {
      if (r.loss != LossClass::kExact) continue;
      const auto& s = r.src_family;
      const auto& d = r.dst.family;
      if (s == TypeFamily::kInt && d == TypeFamily::kInt) {
        const int need = r.src_bits_max >= 0 ? r.src_bits_max : r.src_bits_min;
        const int have = r.dst.bits > 0 ? r.dst.bits : 0;
        if (have > 0 && need > have) {
          ++bad;
          fail("P4", r.id + ": exact int " + std::to_string(need) + " bits -> " +
                        std::to_string(have) + " bits");
        }
        if (r.src_unsigned == 1 && have <= need) {
          ++bad;
          fail("P4", r.id + ": unsigned source needs a wider signed target");
        }
      } else if (s == TypeFamily::kInt && d == TypeFamily::kDecimal) {
        if (r.src_unsigned == 1 && r.dst.precision > 0 && r.dst.precision < 20) {
          ++bad;
          fail("P4", r.id + ": uint64 needs numeric(20) or wider");
        }
      } else if (s == TypeFamily::kDecimal && d == TypeFamily::kDecimal) {
        if (r.src_bits_min >= 0 || r.src_bits_max >= 0) {
          // width encoded through bits: ignore
        }
      } else if ((s == TypeFamily::kString || s == TypeFamily::kChar) &&
                 (d == TypeFamily::kString || d == TypeFamily::kChar)) {
        // Length is copied from the source at resolve time; no static check.
      } else if (s != d && !(s == TypeFamily::kBool && d == TypeFamily::kBool)) {
        ++bad;
        fail("P4", r.id + ": exact claim crosses families " + family_name(s) +
                      " -> " + family_name(d));
      }
    }
    if (bad == 0) pass("P4", "exact rules fit target capacity");
  }

  // P5: unsupported rules must document a guard/blocker.
  {
    int bad = 0;
    for (const auto& r : rules_) {
      if (r.loss == LossClass::kUnsupported && r.guard.empty()) {
        ++bad;
        fail("P5", "unsupported without guard: " + r.id);
      }
    }
    if (bad == 0) pass("P5", "unsupported rules are guarded");
  }

  // P6: no two same-priority rules can match the same source column.
  {
    int bad = 0;
    for (size_t i = 0; i < rules_.size(); ++i) {
      for (size_t j = i + 1; j < rules_.size(); ++j) {
        const auto& a = rules_[i];
        const auto& b = rules_[j];
        if (a.priority != b.priority) continue;
        if (!same_match(a, b)) continue;
        if (a.dst.family == b.dst.family && a.dst.bits == b.dst.bits &&
            a.dst.length == b.dst.length && a.dst.precision == b.dst.precision &&
            a.dst.scale == b.dst.scale)
          continue;
        ++bad;
        fail("P6", "ambiguous same-priority match: " + a.id + " vs " + b.id);
      }
    }
    if (bad == 0) pass("P6", "no ambiguous same-priority overlaps");
  }

  // P7: coverage report for source families seen in parse().
  {
    const std::pair<const char*, TypeFamily> wanted[] = {
        {"mysql", TypeFamily::kInt},     {"mysql", TypeFamily::kDecimal},
        {"mysql", TypeFamily::kFloat},   {"mysql", TypeFamily::kString},
        {"mysql", TypeFamily::kChar},    {"mysql", TypeFamily::kBytes},
        {"mysql", TypeFamily::kDate},    {"mysql", TypeFamily::kTime},
        {"mysql", TypeFamily::kTimestamp}, {"mysql", TypeFamily::kJson},
        {"mysql", TypeFamily::kEnum},    {"mysql", TypeFamily::kSet},
        {"mysql", TypeFamily::kBit},     {"mysql", TypeFamily::kGeometry},
        {"pg", TypeFamily::kInt},        {"pg", TypeFamily::kDecimal},
        {"pg", TypeFamily::kFloat},      {"pg", TypeFamily::kBool},
        {"pg", TypeFamily::kString},     {"pg", TypeFamily::kChar},
        {"pg", TypeFamily::kBytes},      {"pg", TypeFamily::kDate},
        {"pg", TypeFamily::kTime},       {"pg", TypeFamily::kTimestamp},
        {"pg", TypeFamily::kTimestampTz}, {"pg", TypeFamily::kJson},
        {"pg", TypeFamily::kUuid},       {"pg", TypeFamily::kBit},
    };
    int gaps = 0;
    for (const auto& w : wanted) {
      bool found = false;
      for (const auto& r : rules_) {
        if (r.src_dialect == w.first && r.src_family == w.second) {
          found = true;
          break;
        }
      }
      if (!found) {
        ++gaps;
        warn("P7", std::string("no rule for ") + w.first + " " +
                      family_name(w.second));
      }
    }
    if (gaps == 0) pass("P7", "source family coverage complete");
  }

  // P8: integer witnesses parse and respect the declared source range.
  {
    int bad = 0;
    for (const auto& r : rules_) {
      if (r.src_family != TypeFamily::kInt) continue;
      char* end = nullptr;
      const long double v = std::strtold(r.witness.c_str(), &end);
      if (r.witness.empty() || end == r.witness.c_str()) {
        ++bad;
        fail("P8", "non-numeric witness: " + r.id);
        continue;
      }
      const long double bound = numeric_bound(r);
      if (bound > 0 && v > bound) {
        ++bad;
        fail("P8", r.id + ": witness " + r.witness + " exceeds source bound");
      }
      if (v < 0 && r.src_unsigned == 1) {
        ++bad;
        fail("P8", r.id + ": negative witness for an unsigned rule");
      }
    }
    if (bad == 0) pass("P8", "integer witnesses are within source bounds");
  }

  return out;
}

}  // namespace mig
