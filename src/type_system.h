#pragma once

#include <string>
#include <vector>

namespace mig {

enum class TypeFamily {
  kInt, kDecimal, kFloat, kBool, kString, kChar, kBytes,
  kDate, kTime, kTimestamp, kTimestampTz, kJson, kUuid,
  kEnum, kSet, kBit, kGeometry, kUnknown
};

enum class LossClass {
  kExact, kRangeLossless, kPrecisionLoss, kRepresentationOnly, kUnsupported
};

struct TypeDesc {
  std::string dialect;
  TypeFamily family = TypeFamily::kUnknown;
  int bits = 0;         // integer width: 8/16/24/32/64
  int precision = -1;   // decimal precision
  int scale = -1;       // decimal scale
  int length = -1;      // string/bit length
  bool unsigned_ = false;
  bool with_tz = false;
  bool tinyint1 = false;
  bool year = false;
  bool is_array = false;
  std::string raw;
};

struct TypeResolution {
  bool ok = false;
  TypeDesc src;
  TypeDesc dst;
  std::string rule_id;
  LossClass loss = LossClass::kUnsupported;
  std::string guard;
  std::string witness;
  std::string note;
  std::string verification;
};

struct TypeRule {
  std::string id;
  std::string src_dialect;
  std::string dst_dialect;
  TypeFamily src_family = TypeFamily::kUnknown;
  int src_bits_min = -1;
  int src_bits_max = -1;
  int src_unsigned = -1;   // -1 any, 0 false, 1 true
  int src_tinyint1 = -1;
  int src_year = -1;
  int src_with_tz = -1;
  TypeDesc dst;
  LossClass loss = LossClass::kUnsupported;
  std::string guard;
  std::string witness;
  std::string verification;
  int priority = 0;
};

class RuleSet {
 public:
  RuleSet();

  TypeResolution resolve(const std::string& src_dialect,
                         const std::string& src_raw,
                         const std::string& dst_dialect) const;

  static TypeDesc parse(const std::string& dialect, const std::string& raw);
  static std::string type_name(const TypeDesc& t);
  static const char* loss_name(LossClass l);
  static const char* family_name(TypeFamily f);
  static TypeFamily family_from(const std::string& s);
  static LossClass loss_from(const std::string& s);
  std::vector<std::string> validate() const;
  std::vector<std::string> dump_tests() const;
  bool save_tsv(const std::string& path, std::string& err) const;
  bool load_tsv(const std::string& path, std::string& err);
  bool save_json(const std::string& path, std::string& err) const;
  bool load_json(const std::string& path, std::string& err);
  // Extension-based dispatch: .json -> JSON, everything else -> TSV.
  bool load_file(const std::string& path, std::string& err);
  bool save_file(const std::string& path, std::string& err) const;
  std::string stats() const;
  bool emit_test_manifest(const std::string& dir, std::string& err) const;
  // Machine-checkable invariants (P1..P8). Each line starts with PASS/FAIL/WARN.
  std::vector<std::string> prove() const;

 private:
  std::vector<TypeRule> rules_;
  void add(const TypeRule& r);
};

}  // namespace mig
