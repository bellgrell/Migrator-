#include "local_file_loader.h"

#include <filesystem>
#include <iomanip>
#include <sstream>

namespace mig {

std::string LocalFileLoader::escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '\t': o += "\\t"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case 0: o += "\\0"; break;
      default: o.push_back((char)c);
    }
  }
  return o;
}

std::string LocalFileLoader::field(const Value& v) {
  switch (v.type) {
    case ValType::kNull: return "\\N";
    case ValType::kInt: return std::to_string(v.i);
    case ValType::kDouble: {
      std::ostringstream o;
      o << std::setprecision(17) << v.d;
      return o.str();
    }
    case ValType::kBool: return v.b ? "1" : "0";
    case ValType::kBytes:
    case ValType::kString:
    default: return escape(v.s);
  }
}

std::string LocalFileLoader::hex_field(const Value& v) {
  if (v.type == ValType::kNull) return "\\N";
  static const char* d = "0123456789abcdef";
  std::string o;
  o.reserve(v.s.size() * 2);
  for (unsigned char c : v.s) {
    o.push_back(d[c >> 4]);
    o.push_back(d[c & 0xF]);
  }
  return o;
}

void LocalFileLoader::write_row(const Row& row,
                                const std::vector<bool>* hex_cols) {
  for (size_t j = 0; j < row.size(); ++j) {
    if (j) out_.put('\t');
    const bool hex = hex_cols && j < hex_cols->size() && (*hex_cols)[j];
    out_ << (hex ? hex_field(row[j]) : field(row[j]));
  }
  out_.put('\n');
  ++pending_;
}

void LocalFileLoader::close_file() {
  if (out_.is_open()) out_.close();
}

void LocalFileLoader::remove_file() {
  close_file();
  std::error_code ec;
  std::filesystem::remove(path_, ec);
}

void LocalFileLoader::reset() {
  remove_file();
  pending_ = 0;
}

}  // namespace mig
