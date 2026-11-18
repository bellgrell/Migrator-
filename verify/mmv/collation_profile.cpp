#include "collation_profile.h"

#include "unicode.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace mmv {

bool parse_collation_profile(const std::string& name, CollationProfile& out,
                             std::string& err) {
  out = CollationProfile{};
  out.name = name;
  if (name == "binary") {
    return true;
  }
  if (name == "icu-79-ai-ci") {
    out.icu_locale = "und-u-ks-level1";
    out.strength = 0;
    out.requires_key_column = true;
    return true;
  }
  if (name == "mysql-0900-ai-ci") {
    out.icu_locale = "und-u-ks-level1";
    out.strength = 0;
    out.mysql_collation = "utf8mb4_0900_ai_ci";
    out.mysql_sql_key = true;
    out.requires_key_column = true;
    return true;
  }
  err = "unknown collation profile: " + name +
        " (known: binary, icu-79-ai-ci, mysql-0900-ai-ci)";
  return false;
}

std::string sql_collation_key(const CollationProfile& p, const std::string& quoted_col,
                              Dialect d) {
  if (p.name == "binary" || p.mysql_collation.empty()) return "";
  if (d == Dialect::kMySQL)
    return "HEX(WEIGHT_STRING(" + quoted_col + " COLLATE " + p.mysql_collation + "))";
  return "";
}

std::string client_collation_key(const CollationProfile& p, const std::string& value) {
  if (p.name == "binary") return value;
  return unicode_collation_key(value, p.icu_locale, p.strength);
}

namespace {

Status ensure_pg_collation(SqlBackend& db, std::string& err) {
  ResultSet rs;
  Status st = db.query(
      "SELECT 1 FROM pg_collation WHERE collname = 'mmv_ai_ci'", rs);
  if (!st.ok()) { err = st.msg; return st; }
  if (!rs.rows.empty()) return Status::OK();
  st = db.query(
      "CREATE COLLATION mmv_ai_ci (provider = icu, locale = 'und-u-ks-level1', "
      "deterministic = false)",
      rs);
  if (!st.ok()) {
    err = "cannot create mmv_ai_ci: " + st.msg;
    return st;
  }
  return Status::OK();
}

Status equality(SqlBackend& db, const std::string& a, const std::string& b,
                const CollationProfile& p, bool& out) {
  std::string expr;
  if (db.dialect() == Dialect::kMySQL) {
    const std::string c = p.mysql_collation.empty() ? "utf8mb4_0900_ai_ci"
                                                    : p.mysql_collation;
    expr = "(" + quote_lit(a, db.dialect()) + " COLLATE " + c + ") = (" +
           quote_lit(b, db.dialect()) + " COLLATE " + c + ")";
  } else {
    expr = "(" + quote_lit(a, db.dialect()) + " COLLATE mmv_ai_ci) = (" +
           quote_lit(b, db.dialect()) + " COLLATE mmv_ai_ci)";
  }
  ResultSet rs;
  Status st = db.query("SELECT " + expr, rs);
  if (!st.ok()) return st;
  out = !rs.rows.empty() && !rs.rows[0].empty() && !rs.rows[0][0].is_null &&
        (rs.rows[0][0].text == "t" || rs.rows[0][0].text == "1");
  return Status::OK();
}

}  // namespace

Status check_collation_profile(SqlBackend& a, SqlBackend& b, const CollationProfile& p,
                               std::string& err) {
  if (p.name == "binary" || p.name == "icu-79-ai-ci") return Status::OK();
  if (a.dialect() == Dialect::kPg) {
    Status st = ensure_pg_collation(a, err);
    if (!st.ok()) return st;
  }
  if (b.dialect() == Dialect::kPg) {
    Status st = ensure_pg_collation(b, err);
    if (!st.ok()) return st;
  }
  std::vector<std::pair<std::string, std::string>> corpus = {
      {"a", "A"},       {"café", "cafe"}, {"ß", "ss"},   {"ø", "o"},
      {"İ", "i"},       {"æ", "ae"},      {"a ", "a"},   {"e\u0301", "é"}};
  std::ifstream corpus_file("tests/collation_corpus.tsv");
  std::string cline;
  while (std::getline(corpus_file, cline)) {
    if (cline.empty() || cline[0] == '#') continue;
    const size_t tab = cline.find('\t');
    if (tab == std::string::npos) continue;
    corpus.emplace_back(cline.substr(0, tab), cline.substr(tab + 1));
  }

  for (const auto& c : corpus) {
    bool ea = false, eb = false;
    Status sa = equality(a, c.first, c.second, p, ea);
    if (!sa.ok()) { err = sa.msg; return sa; }
    Status sb = equality(b, c.first, c.second, p, eb);
    if (!sb.ok()) { err = sb.msg; return sb; }
    if (ea != eb) {
      err = "collation profile mismatch for \"" + c.first + "\" vs \"" + c.second + "\"";
      return Status::Err(Code::kInvalid, err);
    }
  }
  return Status::OK();
}

}  // namespace mmv
