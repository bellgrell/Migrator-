#include "cdc.h"

#include <mysql/mysql.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "backends.h"
#include "ddl_dialect.h"

namespace mig {
namespace {

uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le24(const unsigned char* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
uint32_t le32(const unsigned char* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
uint64_t le64(const unsigned char* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
uint64_t be(const unsigned char* p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
  return v;
}

bool read_lenenc(const unsigned char* p, size_t len, size_t& off, uint64_t& out) {
  if (off >= len) return false;
  const unsigned char b = p[off];
  if (b < 0xFB) { out = b; off += 1; return true; }
  if (b == 0xFC) { if (off + 2 >= len) return false; out = le16(p + off + 1); off += 3; return true; }
  if (b == 0xFD) { if (off + 3 >= len) return false; out = le24(p + off + 1); off += 4; return true; }
  if (b == 0xFE) { if (off + 8 >= len) return false; out = le64(p + off + 1); off += 9; return true; }
  return false;
}

enum {
  T_TINY = 1, T_SHORT = 2, T_LONG = 3, T_FLOAT = 4, T_DOUBLE = 5,
  T_TIMESTAMP = 7, T_LONGLONG = 8, T_INT24 = 9, T_DATE = 10,
  T_DATETIME = 12, T_YEAR = 13, T_VARCHAR = 15, T_BIT = 16,
  T_TIMESTAMP2 = 17, T_DATETIME2 = 18,
  T_JSON = 245, T_NEWDECIMAL = 246, T_ENUM = 247, T_SET = 248,
  T_TINY_BLOB = 249, T_MEDIUM_BLOB = 250, T_LONG_BLOB = 251, T_BLOB = 252,
  T_VAR_STRING = 253, T_STRING = 254, T_GEOMETRY = 255
};

int meta_size(uint8_t type) {
  switch (type) {
    case T_FLOAT: case T_DOUBLE:
    case T_BLOB: case T_TINY_BLOB: case T_MEDIUM_BLOB: case T_LONG_BLOB:
    case T_GEOMETRY: case T_JSON:
      return 1;
    case T_VARCHAR: case T_VAR_STRING: case T_STRING:
    case T_BIT: case T_NEWDECIMAL: case T_ENUM: case T_SET:
      return 2;
    case T_TIMESTAMP2: case T_DATETIME2:
      return 1;
    default:
      return 0;
  }
}

std::string fmt_dt(int y, int mo, int d, int h, int mi, int s) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
  return std::string(buf);
}

int decimal_size(int precision, int scale) {
  static const int dig2bytes[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  const int intg = precision - scale;
  const int intg0 = intg / 9;
  const int frac0 = scale / 9;
  const int intg0x = intg % 9;
  const int frac0x = scale % 9;
  return intg0 * 4 + dig2bytes[intg0x] + frac0 * 4 + dig2bytes[frac0x];
}

// MySQL NEWDECIMAL 的 packed-BCD 格式
std::string decode_newdecimal(const unsigned char* p, int precision, int scale,
                              size_t& used) {
  static const int dig2bytes[10] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  const int intg = precision - scale;
  const int intg0 = intg / 9;
  const int frac0 = scale / 9;
  const int intg0x = intg % 9;
  const int frac0x = scale % 9;
  const int size = decimal_size(precision, scale);
  used = (size_t)size;
  if (size <= 0) return "0";

  std::vector<unsigned char> buf(p, p + size);
  const bool negative = (buf[0] & 0x80) == 0;
  if (negative) {
    for (auto& b : buf) b = (unsigned char)~b;
  }
  // 无论正负，首字节的 0x80 是符号位，必须掩掉
  buf[0] ^= 0x80;

  size_t pos = 0;
  auto read_group = [&](int n) -> uint32_t {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | buf[pos++];
    return v;
  };

  std::string digits;
  for (int i = 0; i < intg0; ++i) {
    char tmp[16];
    std::snprintf(tmp, sizeof(tmp), "%09u", read_group(4));
    digits += tmp;
  }
  if (intg0x > 0) {
    char tmp[16];
    std::snprintf(tmp, sizeof(tmp), "%0*u", intg0x, read_group(dig2bytes[intg0x]));
    digits += tmp;
  }

  const size_t nz = digits.find_first_not_of('0');
  std::string intpart = (nz == std::string::npos) ? "0" : digits.substr(nz);

  std::string frac;
  for (int i = 0; i < frac0; ++i) {
    char tmp[16];
    std::snprintf(tmp, sizeof(tmp), "%09u", read_group(4));
    frac += tmp;
  }
  if (frac0x > 0) {
    char tmp[16];
    std::snprintf(tmp, sizeof(tmp), "%0*u", frac0x, read_group(dig2bytes[frac0x]));
    frac += tmp;
  }

  std::string out;
  if (negative) out += '-';
  out += intpart;
  if (scale > 0) {
    out += '.';
    out += frac;
  }
  return out;
}

std::string trim_printable(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    if (c < 0x20 || c > 0x7E) break;
    out.push_back((char)c);
  }
  return out;
}

bool decode_value(uint8_t type, int m1, int m2, const unsigned char* p, size_t len,
                  size_t& off, Value& out) {
  switch (type) {
    case T_TINY:
      if (off + 1 > len) return false;
      out = Value::Int((int8_t)p[off]); off += 1; return true;
    case T_SHORT:
      if (off + 2 > len) return false;
      out = Value::Int((int16_t)le16(p + off)); off += 2; return true;
    case T_LONG: case T_INT24:
      if (off + 4 > len) return false;
      out = Value::Int((int32_t)le32(p + off)); off += 4; return true;
    case T_LONGLONG:
      if (off + 8 > len) return false;
      out = Value::Int((int64_t)le64(p + off)); off += 8; return true;
    case T_YEAR:
      if (off + 1 > len) return false;
      out = Value::Int(1900 + p[off]); off += 1; return true;
    case T_FLOAT: {
      if (off + 4 > len) return false;
      float f; std::memcpy(&f, p + off, 4);
      out = Value::Dbl((double)f); off += 4; return true;
    }
    case T_DOUBLE: {
      if (off + 8 > len) return false;
      double d; std::memcpy(&d, p + off, 8);
      out = Value::Dbl(d); off += 8; return true;
    }
    case T_VARCHAR: case T_VAR_STRING: case T_STRING: {
      const int maxlen = m1 | (m2 << 8);
      const int lb = (maxlen > 255) ? 2 : 1;
      if (off + (size_t)lb > len) return false;
      const size_t n = (lb == 2) ? le16(p + off) : p[off];
      off += (size_t)lb;
      if (off + n > len) return false;
      out = Value::Str(std::string((const char*)p + off, n));
      off += n; return true;
    }
    case T_BLOB: case T_TINY_BLOB: case T_MEDIUM_BLOB: case T_LONG_BLOB:
    case T_GEOMETRY: case T_JSON: {
      const int lb = m1;
      if (lb < 1 || lb > 4 || off + (size_t)lb > len) return false;
      size_t n = 0;
      for (int i = 0; i < lb; ++i) n |= ((size_t)p[off + i]) << (8 * i);
      off += (size_t)lb;
      if (off + n > len) return false;
      out = Value::Str(std::string((const char*)p + off, n));
      off += n; return true;
    }
    case T_NEWDECIMAL: {
      const int sz = decimal_size(m1, m2);
      if (sz <= 0 || off + (size_t)sz > len) return false;
      size_t used = 0;
      const std::string txt = decode_newdecimal(p + off, m1, m2, used);
      off += used;
      out = Value::Str(txt);
      return true;
    }
    case T_DATE: {
      if (off + 3 > len) return false;
      const uint32_t v = le24(p + off);
      off += 3;
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", v >> 9, (v >> 5) & 15, v & 31);
      out = Value::Str(buf);
      return true;
    }
    case T_DATETIME2: {
      if (off + 5 > len) return false;
      uint64_t v = be(p + off, 5);
      off += 5;
      v -= 0x8000000000ULL;
      const uint32_t ymd = (uint32_t)(v >> 17);
      const uint32_t ym = ymd >> 5;
      const uint32_t hms = (uint32_t)(v & ((1u << 17) - 1));
      out = Value::Str(fmt_dt((int)(ym / 13), (int)(ym % 13), (int)(ymd & 31),
                              (int)(hms >> 12), (int)((hms >> 6) & 63), (int)(hms & 63)));
      return true;
    }
    case T_TIMESTAMP2: {
      if (off + 4 > len) return false;
      const uint32_t secs = (uint32_t)be(p + off, 4);
      off += 4;
      const std::time_t t = (std::time_t)secs;
      std::tm tm{};
      localtime_r(&t, &tm);
      out = Value::Str(fmt_dt(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min, tm.tm_sec));
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

struct BinlogReader::Impl {
  MYSQL* conn = nullptr;
  std::string src_db;
  std::unique_ptr<IDdlDialect> ddl;
  MYSQL_RPL rpl{};
  bool opened = false;
  size_t checksum_len = 0;  // CRC32 时事件尾部多 4 字节

  struct TableMap {
    std::string db;
    std::string table;
    std::vector<uint8_t> types;
    std::vector<int> m1;
    std::vector<int> m2;
  };
  std::unordered_map<uint64_t, TableMap> maps;
};

BinlogReader::BinlogReader() : impl_(new Impl()) {}
BinlogReader::~BinlogReader() { close(); }

void BinlogReader::close() {
  if (impl_->opened && impl_->conn) {
    mysql_binlog_close(impl_->conn, &impl_->rpl);
    impl_->opened = false;
  }
  if (impl_->conn) { mysql_close(impl_->conn); impl_->conn = nullptr; }
}

Status BinlogReader::open(const Options& o, const std::string& file, uint64_t pos) {
  close();
  impl_->conn = mysql_init(nullptr);
  if (!impl_->conn) return Status::Err(Code::kDb, "binlog mysql_init failed");

  std::string host = "127.0.0.1", port = "3306", user = "root", db = "";
  std::istringstream ss(o.source);
  std::string item;
  while (std::getline(ss, item, ';')) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = item.substr(0, eq);
    const std::string v = item.substr(eq + 1);
    if (k == "host") host = v;
    else if (k == "port") port = v;
    else if (k == "user") user = v;
    else if (k == "db") db = v;
  }

  if (!mysql_real_connect(impl_->conn, host.c_str(), user.c_str(), "",
                          db.empty() ? nullptr : db.c_str(),
                          (unsigned)std::atoi(port.c_str()), nullptr, 0)) {
    return Status::Err(Code::kDb, std::string("binlog connect: ") + mysql_error(impl_->conn));
  }

  impl_->src_db = db;
  impl_->ddl = make_ddl_dialect(o.target_kind);
  std::memset(&impl_->rpl, 0, sizeof(impl_->rpl));
  impl_->rpl.file_name = file.c_str();
  impl_->rpl.file_name_length = file.size();
  impl_->rpl.start_position = pos;
  impl_->rpl.server_id = (unsigned)o.cdc_server_id;

  // 复制客户端必须声明 checksum 处理方式，否则服务端拒绝发送事件
  mysql_query(impl_->conn, "SET @master_binlog_checksum='NONE'");
  mysql_query(impl_->conn, "SET @source_binlog_checksum='NONE'");

  // 事件仍带 CRC32 尾巴，QUERY 事件解析要扣掉，否则 DDL 末尾多出垃圾字符
  if (mysql_query(impl_->conn, "SHOW VARIABLES LIKE 'binlog_checksum'") == 0) {
    MYSQL_RES* cr = mysql_store_result(impl_->conn);
    if (cr) {
      MYSQL_ROW row = mysql_fetch_row(cr);
      if (row && row[1] && std::string(row[1]) == "CRC32") impl_->checksum_len = 4;
      mysql_free_result(cr);
    }
  }

  if (mysql_binlog_open(impl_->conn, &impl_->rpl)) {
    return Status::Err(Code::kDb, std::string("mysql_binlog_open: ") + mysql_error(impl_->conn));
  }
  impl_->opened = true;
  file_ = file;
  pos_ = pos;
  return Status::OK();
}

Status BinlogReader::next(bool& has_event, std::vector<CdcEvent>& events) {
  has_event = false;
  events.clear();
  if (!impl_->opened) return Status::Err(Code::kDb, "binlog not open");

  const int frc = mysql_binlog_fetch(impl_->conn, &impl_->rpl);
  if (std::getenv("MIG_CDC_DEBUG")) {
    std::fprintf(stderr, "[cdc-dbg] fetch rc=%d size=%lu\n", frc,
                 (unsigned long)impl_->rpl.size);
  }
  if (frc) {
    return Status::Err(Code::kDb, std::string("binlog fetch: ") + mysql_error(impl_->conn));
  }

  const unsigned char* d = impl_->rpl.buffer;
  size_t size = impl_->rpl.size;
  if (std::getenv("MIG_CDC_DEBUG")) {
    std::fprintf(stderr, "[cdc-dbg] raw size=%lu hexdump:", (unsigned long)size);
    for (size_t i = 0; i < size && i < 24; ++i) std::fprintf(stderr, " %02x", d[i]);
    std::fprintf(stderr, "\n");
  }
  if (!d || size < 20) return Status::OK();

  // libmysqlclient 在事件前多返回 1 字节状态位，用 event_size 自检修正
  {
    size_t off = 0;
    if (le32(d + 9) != (uint32_t)size) {
      if (size >= 21 && le32(d + 10) == (uint32_t)(size - 1)) off = 1;
    }
    d += off;
    size -= off;
  }
  if (size < 19) return Status::OK();

  const uint8_t type = d[4];
  const uint32_t ev_size = le32(d + 9);
  const uint32_t log_pos = le32(d + 13);
  if (ev_size < 19 || ev_size > size) return Status::OK();
  pos_ = log_pos;

  if (std::getenv("MIG_CDC_DEBUG")) {
    std::fprintf(stderr, "[cdc-dbg] type=%u ev_size=%u log_pos=%u\n", (unsigned)type,
                 (unsigned)ev_size, (unsigned)log_pos);
  }

  const unsigned char* b = d + 19;
  const size_t blen = ev_size - 19;

  // ---------- ROTATE：binlog 轮转 ----------
  if (type == 4 && blen > 8) {
    const uint64_t new_pos = le64(b);
    std::string name;
    for (size_t i = 8; i < blen; ++i) {
      const unsigned char ch = b[i];
      // 只接受文件名合法字符，截断 NUL / CRC / 引号等尾部垃圾
      if (!(std::isalnum(ch) || ch == '.' || ch == '/' || ch == '_' || ch == '-')) break;
      name.push_back((char)ch);
    }
    while (!name.empty() && name.back() == '\n') name.pop_back();
    if (!name.empty()) {
      file_ = name;
      impl_->rpl.file_name = file_.c_str();
      impl_->rpl.file_name_length = file_.size();
      pos_ = new_pos;
      if (std::getenv("MIG_CDC_DEBUG")) {
        std::fprintf(stderr, "[cdc-dbg] rotate -> %s:%llu\n", file_.c_str(),
                     (unsigned long long)new_pos);
      }
    }
    return Status::OK();
  }

  // ---------- QUERY：DDL ----------
  if (type == 2 && blen > 13) {
    const size_t plen =
        blen > impl_->checksum_len ? blen - impl_->checksum_len : blen;
    size_t p = 8;
    const uint8_t dblen = b[p++];
    p += 2;
    const uint16_t svlen = le16(b + p);
    p += 2 + svlen;
    if (p + dblen + 1 > plen) return Status::OK();
    std::string dbname((const char*)b + p, dblen);
    p += dblen + 1;
    if (p >= plen) return Status::OK();

    std::string query((const char*)b + p, plen - p);
    if (std::getenv("MIG_CDC_DEBUG")) {
      std::fprintf(stderr, "[cdc-dbg] query db=%s sql=%.100s\n", dbname.c_str(),
                   query.c_str());
    }
    std::string low = query;
    for (char& c : low) c = (char)std::tolower((unsigned char)c);

    const bool ddl = low.rfind("create ", 0) == 0 || low.rfind("alter ", 0) == 0 ||
                     low.rfind("drop ", 0) == 0 || low.rfind("truncate ", 0) == 0 ||
                     low.rfind("rename ", 0) == 0;
    if (ddl) {
      CdcEvent ev;
      ev.is_ddl = true;
      ev.table = dbname;
      const std::string raw_ddl = trim_printable(query);
      const std::string src_db = dbname.empty() ? impl_->src_db : dbname;
      ev.ddl = impl_->ddl ? impl_->ddl->convert(raw_ddl, src_db) : raw_ddl;
      events.push_back(std::move(ev));
    }
    has_event = !events.empty();
    return Status::OK();
  }

  // ---------- TABLE_MAP ----------
  if (type == 19 && blen > 10) {
    const uint64_t tid = le64(b) & 0xFFFFFFFFFFFFULL;
    size_t p = 8;
    const uint8_t dblen = b[p++];
    std::string dbname((const char*)b + p, dblen); p += dblen;
    p++;  // 0x00
    const uint8_t tblen = b[p++];
    std::string tbl((const char*)b + p, tblen); p += tblen;
    p++;  // 0x00

    uint64_t ncols = 0;
    if (!read_lenenc(b, blen, p, ncols)) return Status::OK();

    Impl::TableMap tm;
    tm.db = dbname;
    tm.table = tbl;
    tm.types.assign(b + p, b + p + ncols);
    p += ncols;

    uint64_t metalen = 0;
    if (!read_lenenc(b, blen, p, metalen)) return Status::OK();
    for (uint64_t i = 0; i < ncols; ++i) {
      const int msz = meta_size(tm.types[i]);
      int v1 = 0, v2 = 0;
      if (msz >= 1 && p < blen) v1 = b[p++];
      if (msz >= 2 && p < blen) v2 = b[p++];
      tm.m1.push_back(v1);
      tm.m2.push_back(v2);
    }
    impl_->maps[tid] = tm;
    return Status::OK();
  }

  // ---------- ROWS ----------
  const bool is_write = (type == 23 || type == 30);
  const bool is_update = (type == 24 || type == 31);
  const bool is_delete = (type == 25 || type == 32);
  if (!is_write && !is_update && !is_delete) return Status::OK();

  if (blen < 10) return Status::OK();
  const uint64_t tid = le64(b) & 0xFFFFFFFFFFFFULL;
  auto it = impl_->maps.find(tid);
  if (it == impl_->maps.end()) return Status::OK();
  const Impl::TableMap& tm = it->second;

  size_t p = 8;
  if (type == 30 || type == 31 || type == 32) {
    // extra_data_length 包含它自身的 2 字节
    const uint16_t extra = le16(b + 8);
    p = 8 + extra;
  }

  uint64_t ncols = 0;
  if (!read_lenenc(b, blen, p, ncols)) return Status::OK();

  const size_t bm_len = (size_t)((ncols + 7) / 8);
  if (p + bm_len > blen) return Status::OK();
  const unsigned char* bm1 = b + p;
  p += bm_len;

  const unsigned char* bm2 = nullptr;
  if (is_update) {
    if (p + bm_len > blen) return Status::OK();
    bm2 = b + p;
    p += bm_len;
  }

  auto decode_image = [&](const unsigned char* bm, std::vector<Value>& out) -> bool {
    int present = 0;
    for (size_t i = 0; i < bm_len; ++i) {
      for (int k = 0; k < 8; ++k) if (bm[i] & (1 << k)) ++present;
    }
    const size_t null_len = (size_t)((present + 7) / 8);
    if (p + null_len > blen) return false;
    const unsigned char* nulls = b + p;
    p += null_len;

    out.assign(tm.types.size(), Value::Null());
    int idx = 0;
    for (size_t col = 0; col < tm.types.size(); ++col) {
      if (!(bm[col / 8] & (1 << (col % 8)))) continue;
      const bool is_null = (nulls[idx / 8] >> (idx % 8)) & 1;
      ++idx;
      if (is_null) continue;
      Value v;
      if (!decode_value(tm.types[col], tm.m1[col], tm.m2[col], b, blen, p, v)) {
        return false;
      }
      out[col] = v;
    }
    return true;
  };

  while (p < blen) {
    CdcEvent ev;
    ev.table = tm.table;

    if (is_write) {
      ev.op = 0;
      if (!decode_image(bm1, ev.after)) break;
    } else if (is_delete) {
      ev.op = 2;
      if (!decode_image(bm1, ev.before)) break;
    } else {
      ev.op = 1;
      if (!decode_image(bm1, ev.before)) break;
      if (!decode_image(bm2, ev.after)) break;
    }
    events.push_back(std::move(ev));
  }

  has_event = !events.empty();
  return Status::OK();
}

namespace {

bool current_binlog_pos(const Options& o, std::string& file, uint64_t& pos) {
  MYSQL* c = mysql_init(nullptr);
  if (!c) return false;

  std::string host = "127.0.0.1", port = "3306", user = "root", db = "";
  std::istringstream ss(o.source);
  std::string item;
  while (std::getline(ss, item, ';')) {
    const size_t eq = item.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = item.substr(0, eq);
    const std::string v = item.substr(eq + 1);
    if (k == "host") host = v;
    else if (k == "port") port = v;
    else if (k == "user") user = v;
    else if (k == "db") db = v;
  }
  if (!mysql_real_connect(c, host.c_str(), user.c_str(), "",
                          db.empty() ? nullptr : db.c_str(),
                          (unsigned)std::atoi(port.c_str()), nullptr, 0)) {
    mysql_close(c);
    return false;
  }

  bool ok = false;
  const char* queries[] = {"SHOW BINARY LOG STATUS", "SHOW MASTER STATUS"};
  for (const char* q : queries) {
    if (mysql_query(c, q) != 0) continue;
    MYSQL_RES* r = mysql_store_result(c);
    if (!r) continue;
    MYSQL_ROW row = mysql_fetch_row(r);
    if (row && row[0] && row[1]) {
      file = row[0];
      pos = std::strtoull(row[1], nullptr, 10);
      ok = true;
    }
    mysql_free_result(r);
    if (ok) break;
  }
  mysql_close(c);
  return ok;
}

int pk_index(const TableSchema& s) {
  if (s.primary_key.empty()) return -1;
  for (size_t i = 0; i < s.columns.size(); ++i) {
    if (s.columns[i].name == s.primary_key[0]) return (int)i;
  }
  return -1;
}

Status apply_event(ITarget& tgt, const TableSchema& s, const CdcEvent& ev) {
  const int pk = pk_index(s);
  if (pk < 0) return Status::Err(Code::kInvalid, "table has no primary key");

  const Row& img = (ev.op == 2) ? ev.before : ev.after;
  if ((size_t)pk >= img.size()) return Status::Err(Code::kInvalid, "row image too short");
  if (img.size() != s.columns.size())
    return Status::Err(Code::kInvalid,
                       "schema/row image column mismatch: schema=" +
                           std::to_string(s.columns.size()) + " image=" +
                           std::to_string(img.size()) + " (DDL 后需重读 schema)");

  const std::string sql = (ev.op == 2) ? tgt.delete_row_sql(s, img)
                                      : tgt.upsert_sql(s, img);
  if (sql.empty())
    return Status::Err(Code::kInvalid,
                       "target cannot build row change SQL for " + s.name);
  return tgt.exec_sql(sql);
}

}  // namespace

Status apply_cdc_event(ITarget& tgt, const TableSchema& s, const CdcEvent& ev) {
  return apply_event(tgt, s, ev);
}

Status run_cdc(const Options& o, Metrics& m, int max_events) {
  std::string file;
  uint64_t pos = 0;

  if (!o.cdc_start.empty()) {
    const size_t c = o.cdc_start.find(':');
    if (c != std::string::npos) {
      file = o.cdc_start.substr(0, c);
      pos = std::strtoull(o.cdc_start.substr(c + 1).c_str(), nullptr, 10);
    }
  }
  if (file.empty()) {
    std::ifstream in(o.cdc_position_path);
    if (in) { std::getline(in, file, ':'); in >> pos; }
  }
  if (file.empty()) {
    if (!current_binlog_pos(o, file, pos)) {
      return Status::Err(Code::kDb, "cannot determine binlog position");
    }
  }

  m.log("cdc start: " + file + ":" + std::to_string(pos));

  BinlogReader reader;
  Status st = reader.open(o, file, pos);
  if (!st.ok()) return st;

  Status s1;
  auto src = make_source(o, s1);
  if (!src || !src->connect().ok()) {
    return Status::Err(Code::kDb, "cdc source connect failed");
  }
  Status s2;
  auto tgt = make_target(o, s2);
  if (!tgt || !tgt->connect().ok()) {
    return Status::Err(Code::kDb, "cdc target connect failed");
  }

  std::unordered_map<std::string, TableSchema> schemas;
  int applied = 0;
  std::string last_file = file;
  uint64_t last_pos = pos;
  int reconnects = 0;

  while (true) {
    bool has = false;
    std::vector<CdcEvent> events;
    st = reader.next(has, events);
    if (!st.ok()) {
      const int max_retry = o.cdc_retries > 0 ? o.cdc_retries : 0;
      if (reconnects >= max_retry) {
        m.log("[error] cdc: " + st.str());
        return st;
      }
      ++reconnects;
      m.log("[warn] cdc disconnected: " + st.str() + ", reconnect from " +
            last_file + ":" + std::to_string(last_pos));
      reader.close();
      Status rs;
      for (int i = 0; i <= (o.connect_retries > 0 ? o.connect_retries : 0); ++i) {
        if (i) std::this_thread::sleep_for(std::chrono::milliseconds(
            (o.connect_backoff_ms > 0 ? o.connect_backoff_ms : 500) * i));
        rs = reader.open(o, last_file, last_pos);
        if (rs.ok()) break;
      }
      if (!rs.ok()) {
        m.log("[error] cdc reopen: " + rs.str());
        return rs;
      }
      src = make_source(o, s1);
      if (!src || !src->connect().ok()) {
        m.log("[error] cdc source reconnect failed");
        return Status::Err(Code::kDb, "cdc source reconnect failed");
      }
      tgt->close();
      if (!tgt->connect().ok()) {
        m.log("[error] cdc target reconnect failed");
        return Status::Err(Code::kDb, "cdc target reconnect failed");
      }
      schemas.clear();
      continue;
    }
    if (!has) continue;

    for (const auto& ev : events) {
      if (std::getenv("MIG_CDC_DEBUG")) {
        m.log(std::string("evt is_ddl=") + (ev.is_ddl ? "1" : "0") +
              " table=" + ev.table + " after=" + std::to_string(ev.after.size()) +
              " before=" + std::to_string(ev.before.size()));
      }

      if (ev.is_ddl) {
        if (o.cdc_ddl) {
          Status ds = tgt->exec_sql(ev.ddl);
          if (ds.ok()) {
            m.log("ddl ok: " + ev.ddl.substr(0, 120));
            // DDL 可能新增/删除列，缓存 schema 全部失效，下个事件重新读
            schemas.clear();
          } else {
            m.errors += 1;
            m.log("[error] ddl: " + ds.str() + " | " + ev.ddl.substr(0, 120));
            if (std::getenv("MIG_CDC_DEBUG"))
              std::fprintf(stderr, "[cdc-err] ddl: %s | %s\n", ds.str().c_str(),
                           ev.ddl.substr(0, 200).c_str());
          }
        } else {
          m.log("[ddl] skipped (use --cdc-ddl): " + ev.ddl.substr(0, 100));
        }
        ++applied;
        continue;
      }

      auto it = schemas.find(ev.table);
      if (it == schemas.end()) {
        TableSchema s;
        if (src->read_schema(ev.table, s).ok()) {
          schemas[ev.table] = s;
          it = schemas.find(ev.table);
        }
      }
      if (it == schemas.end()) continue;

      Status as = apply_event(*tgt, it->second, ev);
      if (!as.ok()) {
        m.errors += 1;
        m.log("[error] cdc apply " + ev.table + ": " + as.str());
        if (std::getenv("MIG_CDC_DEBUG"))
          std::fprintf(stderr, "[cdc-err] apply %s: %s\n", ev.table.c_str(),
                       as.str().c_str());
      } else {
        m.rows_done += 1;
        ++applied;
      }
    }

    // 变更必须提交，否则进程退出时全部回滚
    Status cs = tgt->commit();
    if (!cs.ok()) {
      m.errors += 1;
      m.log("[error] cdc commit: " + cs.str());
    }

    std::ofstream out(o.cdc_position_path, std::ios::trunc);
    out << reader.file() << ":" << reader.pos();
    last_file = reader.file();
    last_pos = reader.pos();
    reconnects = 0;
    m.log("cdc applied " + std::to_string(applied) + " events, pos=" +
          std::to_string(reader.pos()));

    if (max_events > 0 && applied >= max_events) break;
  }

  return Status::OK();
}

#ifndef MIG_ENABLE_PG_CDC
Status run_pg_cdc(const Options&, Metrics&, int) {
  return Status::Err(Code::kUnsupported, "PG CDC module not built");
}
#endif

}  // namespace mig
