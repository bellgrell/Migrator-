#include "normalize.h"

#ifdef HAVE_ICU
#include "unicode.h"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace mmv {
namespace {
bool g_opengauss_mode = false;
}

void set_opengauss_mode(bool on) { g_opengauss_mode = on; }

namespace {

// Common Latin-1 / Latin Extended-A accents. Kept intentionally small and
// deterministic so the C++ reference and the SQL REPLACE chain agree.
const std::pair<const char*, const char*> kAccentMap[] = {
    {"á","a"},{"à","a"},{"â","a"},{"ä","a"},{"ã","a"},{"å","a"},{"ā","a"},{"ă","a"},{"ą","a"},
    {"Á","A"},{"À","A"},{"Â","A"},{"Ä","A"},{"Ã","A"},{"Å","A"},{"Ā","A"},{"Ă","A"},{"Ą","A"},
    {"ç","c"},{"ć","c"},{"č","c"},
    {"Ç","C"},{"Ć","C"},{"Č","C"},
    {"ď","d"},{"đ","d"},{"Ď","D"},{"Đ","D"},
    {"é","e"},{"è","e"},{"ê","e"},{"ë","e"},{"ē","e"},{"ĕ","e"},{"ė","e"},{"ę","e"},{"ě","e"},
    {"É","E"},{"È","E"},{"Ê","E"},{"Ë","E"},{"Ē","E"},{"Ĕ","E"},{"Ė","E"},{"Ę","E"},{"Ě","E"},
    {"ğ","g"},{"Ğ","G"},
    {"í","i"},{"ì","i"},{"î","i"},{"ï","i"},{"ī","i"},{"į","i"},{"ı","i"},
    {"Í","I"},{"Ì","I"},{"Î","I"},{"Ï","I"},{"Ī","I"},{"Į","I"},{"İ","I"},
    {"ł","l"},{"Ł","L"},
    {"ñ","n"},{"ń","n"},{"ň","n"},{"Ñ","N"},{"Ń","N"},{"Ň","N"},
    {"ó","o"},{"ò","o"},{"ô","o"},{"ö","o"},{"õ","o"},{"ō","o"},{"ő","o"},{"ø","o"},
    {"Ó","O"},{"Ò","O"},{"Ô","O"},{"Ö","O"},{"Õ","O"},{"Ō","O"},{"Ő","O"},{"Ø","O"},
    {"ŕ","r"},{"ř","r"},{"Ŕ","R"},{"Ř","R"},
    {"ś","s"},{"ş","s"},{"š","s"},{"ș","s"},
    {"Ś","S"},{"Ş","S"},{"Š","S"},{"Ș","S"},
    {"ť","t"},{"ţ","t"},{"ț","t"},{"Ť","T"},{"Ţ","T"},{"Ț","T"},
    {"ú","u"},{"ù","u"},{"û","u"},{"ü","u"},{"ū","u"},{"ű","u"},{"ų","u"},{"ů","u"},
    {"Ú","U"},{"Ù","U"},{"Û","U"},{"Ü","U"},{"Ū","U"},{"Ű","U"},{"Ų","U"},{"Ů","U"},
    {"ŵ","w"},{"Ŵ","W"},
    {"ý","y"},{"ÿ","y"},{"ŷ","y"},{"Ý","Y"},{"Ÿ","Y"},{"Ŷ","Y"},
    {"ź","z"},{"ż","z"},{"ž","z"},{"Ź","Z"},{"Ż","Z"},{"Ž","Z"},
    {"ß","ss"},{"æ","ae"},{"Æ","AE"},{"œ","oe"},{"Œ","OE"},
};

std::string sql_accent_fold(const std::string& x, Dialect d) {
  std::string out = x;
  for (const auto& p : kAccentMap)
    out = "REPLACE(" + out + ", " + quote_lit(p.first, d) + ", " +
          quote_lit(p.second, d) + ")";
  return out;
}

std::string escape_field(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\\') { o += '\\'; o += '\\'; }
    else if (c == '|') { o += '\\'; o += '|'; }
    else o += c;
  }
  return o;
}

std::string trim_copy(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) ++b;
  return s.substr(b);
}

std::string canon_decimal(std::string s) {
  s = trim_copy(std::move(s));
  if (s.empty()) return "0";
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; s.erase(s.begin()); }
  size_t dot = s.find('.');
  std::string ip = dot == std::string::npos ? s : s.substr(0, dot);
  std::string fp = dot == std::string::npos ? "" : s.substr(dot + 1);
  size_t nz = ip.find_first_not_of('0');
  ip = nz == std::string::npos ? "0" : ip.substr(nz);
  while (!fp.empty() && fp.back() == '0') fp.pop_back();
  std::string out = ip + (fp.empty() ? "" : "." + fp);
  if (out == "0") neg = false;
  return neg ? "-" + out : out;
}

std::string pad_fraction(std::string s, size_t digits) {
  size_t dot = s.find('.');
  if (dot == std::string::npos) return s + "." + std::string(digits, '0');
  std::string frac = s.substr(dot + 1);
  if (frac.size() >= digits) frac = frac.substr(0, digits);
  else frac.append(digits - frac.size(), '0');
  return s.substr(0, dot) + "." + frac;
}

std::string canon_timestamp(std::string s) {
  s = trim_copy(std::move(s));
  if (s.size() > 10 && s[10] == ' ') s[10] = 'T';
  size_t tpos = s.find('T');
  if (tpos == std::string::npos) return s;  // date-only or unknown shape
  size_t zone = s.find_first_of("+Z", tpos);
  if (zone == std::string::npos) {
    size_t minus = s.find('-', tpos);
    if (minus != std::string::npos && minus > tpos + 1) zone = minus;
  }
  std::string core = zone == std::string::npos ? s : s.substr(0, zone);
  if (core.size() == tpos + 3) core += ":00";
  return pad_fraction(core, 6);
}

std::string canon_time(std::string s) {
  s = trim_copy(std::move(s));
  if (s.size() == 5) s += ":00";
  return pad_fraction(s, 6);
}

std::string canon_double(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-Inf" : "Inf";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.12g", v);
  return buf;
}

// Must match ROUND(x, 9) + decimal trimming on the SQL side.
std::string canon_float(double v, int digits) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-Inf" : "Inf";
  char buf[96];
  std::snprintf(buf, sizeof buf, "%.*f", digits, v);
  return canon_decimal(buf);
}

std::string hex_lower(const std::string& raw) {
  static const char* h = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) { out += h[c >> 4]; out += h[c & 0xF]; }
  return out;
}

// SQL literal pieces that are dialect-safe regardless of backslash settings.
std::string null_marker(Dialect d) {
  return d == Dialect::kPg ? "(chr(92)||'N')" : "CONCAT(CHAR(92),'N')";
}

std::string sql_escape(const std::string& x, Dialect d) {
  if (d == Dialect::kPg)
    return "REPLACE(REPLACE(" + x + ", chr(92), chr(92)||chr(92)), '|', chr(92)||'|')";
  return "REPLACE(REPLACE(" + x + ", CHAR(92), CONCAT(CHAR(92),CHAR(92))), '|', CONCAT(CHAR(92),'|'))";
}

std::string sql_trim_decimal(const std::string& x, Dialect d) {
  if (d == Dialect::kPg) {
    return "CASE WHEN position('.' in " + x + ") > 0 THEN "
           "trim(trailing '.' from trim(trailing '0' from " + x + ")) ELSE " + x + " END";
  }
  return "CASE WHEN LOCATE('.', " + x + ") > 0 THEN "
         "TRIM(TRAILING '.' FROM TRIM(TRAILING '0' FROM " + x + ")) ELSE " + x + " END";
}

std::string sql_text(const std::string& col, Dialect d) {
  return d == Dialect::kPg ? "(" + col + ")::text" : "CAST(" + col + " AS CHAR)";
}

}  // namespace

const char* logical_name(LogicalType t) {
  switch (t) {
    case LogicalType::kInt: return "int";
    case LogicalType::kDecimal: return "decimal";
    case LogicalType::kFloat: return "float";
    case LogicalType::kBool: return "bool";
    case LogicalType::kString: return "string";
    case LogicalType::kChar: return "char";
    case LogicalType::kBytes: return "bytes";
    case LogicalType::kDate: return "date";
    case LogicalType::kTime: return "time";
    case LogicalType::kTimestamp: return "timestamp";
    case LogicalType::kTimestampTz: return "timestamptz";
    case LogicalType::kJson: return "json";
    case LogicalType::kUuid: return "uuid";
    case LogicalType::kUnknown: return "unknown";
  }
  return "unknown";
}

int epsilon_digits(double eps) {
  if (!(eps > 0.0) || eps >= 1.0) return 0;
  const double d = -std::log10(eps);
  int n = (int)std::ceil(d - 1e-12);
  if (n < 0) n = 0;
  if (n > 15) n = 15;
  return n;
}

std::string fold_accent(const std::string& s) {
#ifdef HAVE_ICU
  if (unicode_available()) {
    std::string out = unicode_fold(s, false);
    // ICU NFD leaves compatibility ligatures intact; fold them explicitly.
    static const std::pair<const char*, const char*> ligatures[] = {
        {"ß", "ss"}, {"ẞ", "SS"}, {"æ", "ae"}, {"Æ", "AE"}, {"œ", "oe"},
        {"Œ", "OE"}, {"ﬀ", "ff"}, {"ﬁ", "fi"}, {"ﬂ", "fl"}, {"ﬃ", "ffi"},
        {"ﬄ", "ffl"}, {"ﬅ", "st"},
        // Letters with strokes/bar that NFD does not decompose.
        {"Ł", "L"}, {"ł", "l"}, {"Đ", "D"}, {"đ", "d"}, {"Ø", "O"}, {"ø", "o"},
        {"Ð", "D"}, {"ð", "d"}, {"Þ", "TH"}, {"þ", "th"}, {"Ħ", "H"}, {"ħ", "h"},
        {"Ŧ", "T"}, {"ŧ", "t"}, {"Ŋ", "N"}, {"ŋ", "n"}, {"Ə", "E"}, {"ə", "e"}};
    for (const auto& p : ligatures) {
      const std::string from = p.first, to = p.second;
      size_t pos = 0;
      while ((pos = out.find(from, pos)) != std::string::npos) {
        out.replace(pos, from.size(), to);
        pos += to.size();
      }
    }
    return out;
  }
#endif
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    bool matched = false;
    for (const auto& p : kAccentMap) {
      const size_t n = std::strlen(p.first);
      if (i + n <= s.size() && s.compare(i, n, p.first) == 0) {
        out += p.second;
        i += n;
        matched = true;
        break;
      }
    }
    if (matched) continue;
    // Combining marks U+0300..U+036F (UTF-8 0xCC/0xCD lead bytes).
    const unsigned char c = (unsigned char)s[i];
    if (c == 0xCC && i + 1 < s.size() &&
        (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0xBF) {
      i += 2;
      continue;
    }
    if (c == 0xCD && i + 1 < s.size() &&
        (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0xAF) {
      i += 2;
      continue;
    }
    out += s[i++];
  }
  return out;
}

std::string canonical(const Value& v, const NormSpec& spec) {
  if (v.is_null()) return "\\N";
  std::string out;
  switch (spec.type) {
    case LogicalType::kInt:
      out = v.type == ValType::kInt ? std::to_string(v.i) : v.s;
      break;
    case LogicalType::kDecimal:
      out = canon_decimal(v.type == ValType::kDouble ? canon_double(v.d) : v.s);
      break;
    case LogicalType::kFloat:
      out = v.type == ValType::kDouble ? canon_float(v.d, epsilon_digits(spec.epsilon))
                                        : canon_decimal(v.s);
      break;
    case LogicalType::kBool:
      out = ((v.type == ValType::kBool && v.b) || (v.type == ValType::kInt && v.i != 0) ||
             (v.type == ValType::kString && (v.s == "t" || v.s == "true" || v.s == "1")))
                ? "1" : "0";
      break;
    case LogicalType::kChar: {
      std::string s = v.s;
      if (spec.accent_insensitive) s = fold_accent(s);
      while (!s.empty() && s.back() == ' ') s.pop_back();
      if (spec.case_insensitive) {
#ifdef HAVE_ICU
        if (unicode_available()) s = unicode_fold(s, true);
        else
#endif
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
      }
      out = s;
      break;
    }
    case LogicalType::kBytes:
      out = v.type == ValType::kBytes ? hex_lower(v.s) : hex_lower(v.s);
      break;
    case LogicalType::kDate: {
      std::string s = trim_copy(v.s);
      out = s.size() >= 10 ? s.substr(0, 10) : s;
      break;
    }
    case LogicalType::kTime:
      out = canon_time(v.s);
      break;
    case LogicalType::kTimestamp:
    case LogicalType::kTimestampTz:
      out = canon_timestamp(v.s);
      break;
    case LogicalType::kUuid: {
      std::string s = trim_copy(v.s);
      for (char& c : s) c = (char)std::tolower((unsigned char)c);
      out = s;
      break;
    }
    case LogicalType::kJson:
    case LogicalType::kString: {
      std::string s = v.type == ValType::kInt ? std::to_string(v.i)
                    : v.type == ValType::kDouble ? canon_double(v.d)
                    : v.s;
      if (spec.accent_insensitive) s = fold_accent(s);
      if (spec.trim_space)
        while (!s.empty() && s.back() == ' ') s.pop_back();
      if (spec.case_insensitive) {
#ifdef HAVE_ICU
        if (unicode_available()) s = unicode_fold(s, true);
        else
#endif
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
      }
      out = s;
      break;
    }
    case LogicalType::kUnknown:
    default:
      out = v.type == ValType::kInt ? std::to_string(v.i)
            : v.type == ValType::kDouble ? canon_double(v.d)
            : v.s;
      break;
  }
  return escape_field(out);
}

std::string quote_ident(const std::string& name, Dialect d) {
  if (d == Dialect::kMySQL) {
    std::string o = "`";
    for (char c : name) { if (c == '`') o += '`'; o += c; }
    return o + "`";
  }
  std::string o = "\"";
  for (char c : name) { if (c == '"') o += '"'; o += c; }
  return o + "\"";
}

std::string quote_lit(const std::string& s, Dialect d) {
  std::string o = "'";
  for (char c : s) {
    if (c == '\'') { o += '\''; o += '\''; }
    else if (d == Dialect::kMySQL && c == '\\') { o += '\\'; o += '\\'; }
    else o += c;
  }
  return o + "'";
}

std::string sql_json_canonical(const std::string& col, Dialect d) {
  if (g_opengauss_mode) {
    // openGauss 8.0 has no to_jsonb(), LATERAL or WITH ORDINALITY.  Compare a
    // whitespace-normalized text form instead; jsonb already gives a stable
    // key order on the target side.
    if (d == Dialect::kPg) {
      return "LOWER(MD5(REPLACE(REPLACE(REPLACE((" + col +
             ")::text, ' ', ''), chr(10), ''), chr(9), '')))";
    }
    return "LOWER(MD5(REPLACE(REPLACE(REPLACE(CAST(" + col +
           " AS CHAR), ' ', ''), CHAR(10), ''), CHAR(9), '')))";
  }
  if (d == Dialect::kPg) {
    return "(WITH RECURSIVE j(path,node) AS ("
           "SELECT '$'::text, (" + col + ")::jsonb "
           "UNION ALL "
           "SELECT j.path || e.seg, e.v FROM j CROSS JOIN LATERAL ("
           "SELECT '[' || to_jsonb(oe.key)::text || ']' AS seg, oe.value AS v "
           "FROM jsonb_each(CASE WHEN jsonb_typeof(j.node)='object' THEN j.node ELSE '{}'::jsonb END) oe "
           "UNION ALL "
           "SELECT '[' || aa.ord || ']', aa.value "
           "FROM jsonb_array_elements(CASE WHEN jsonb_typeof(j.node)='array' THEN j.node ELSE '[]'::jsonb END) "
           "WITH ORDINALITY aa(value,ord)) e) "
           "SELECT COALESCE(string_agg("
           "char_length(path)::text || ':' || path || '=' || typ || ':' || "
           "char_length(val)::text || ':' || val, '|' ORDER BY path COLLATE \"C\"), '') "
           "FROM (SELECT path, "
           "CASE jsonb_typeof(node) WHEN 'boolean' THEN 'bool' ELSE jsonb_typeof(node) END AS typ, "
           "CASE WHEN jsonb_typeof(node) IN ('object','array','null') THEN '' "
           "WHEN jsonb_typeof(node)='number' THEN "
           "CASE WHEN position('.' in (node #>> '{}')) > 0 THEN "
           "CASE WHEN trim(trailing '.' from trim(trailing '0' from (node #>> '{}'))) IN ('','-0') THEN '0' "
           "ELSE trim(trailing '.' from trim(trailing '0' from (node #>> '{}'))) END "
           "ELSE CASE WHEN (node #>> '{}')='-0' THEN '0' ELSE (node #>> '{}') END END "
           "ELSE node #>> '{}' END AS val FROM j) q)";
  }
  return "(WITH RECURSIVE j(path,node,is_null) AS ("
         "SELECT CAST('$' AS CHAR(4000)), CAST(" + col + " AS JSON), 0 "
         "UNION ALL "
         "SELECT CONCAT(j.path,'[',JSON_QUOTE(kt.k),']'), "
         "JSON_EXTRACT(j.node, CONCAT('$.',JSON_QUOTE(kt.k))), "
         "CASE WHEN JSON_EXTRACT(j.node, CONCAT('$.',JSON_QUOTE(kt.k))) IS NULL THEN 1 ELSE 0 END "
         "FROM j JOIN JSON_TABLE(JSON_KEYS(j.node),'$[*]' COLUMNS (k VARCHAR(255) PATH '$')) kt "
         "WHERE JSON_TYPE(j.node)='OBJECT' "
         "UNION ALL "
         "SELECT CONCAT(j.path,'[',kt.i,']'), kt.val, "
         "CASE WHEN kt.val IS NULL THEN 1 ELSE 0 END "
         "FROM j JOIN JSON_TABLE(j.node,'$[*]' COLUMNS (i FOR ORDINALITY, val JSON PATH '$')) kt "
         "WHERE JSON_TYPE(j.node)='ARRAY') "
         "SELECT COALESCE(GROUP_CONCAT("
         "CONCAT(CHAR_LENGTH(path),':',path,'=',typ,':',CHAR_LENGTH(val),':',val) "
         "ORDER BY BINARY path SEPARATOR '|'), '') "
         "FROM (SELECT path, "
         "CASE WHEN is_null=1 THEN 'null' ELSE CASE JSON_TYPE(node) WHEN 'OBJECT' THEN 'object' "
         "WHEN 'ARRAY' THEN 'array' WHEN 'STRING' THEN 'string' WHEN 'BOOLEAN' THEN 'bool' "
         "WHEN 'NULL' THEN 'null' WHEN 'INTEGER' THEN 'number' WHEN 'DOUBLE' THEN 'number' "
         "WHEN 'DECIMAL' THEN 'number' ELSE 'string' END END AS typ, "
         "CASE WHEN is_null=1 THEN '' "
         "WHEN JSON_TYPE(node) IN ('OBJECT','ARRAY','NULL') THEN '' "
         "WHEN JSON_TYPE(node) IN ('INTEGER','DOUBLE','DECIMAL') THEN "
         "CASE WHEN LENGTH(SUBSTRING_INDEX(JSON_UNQUOTE(node), '.', 1)) > 45 "
         "OR (LOCATE('.', JSON_UNQUOTE(node)) > 0 AND "
         "LENGTH(SUBSTRING_INDEX(JSON_UNQUOTE(node), '.', -1)) > 20) "
         "OR LOCATE('e', LOWER(JSON_UNQUOTE(node))) > 0 "
         "THEN CONCAT('__MMV_JSON_OVF__', JSON_UNQUOTE(node)) "
         "ELSE CASE WHEN LOCATE('.', CAST(CAST(JSON_UNQUOTE(node) AS DECIMAL(65,20)) AS CHAR)) > 0 THEN "
         "CASE WHEN TRIM(TRAILING '.' FROM TRIM(TRAILING '0' FROM "
         "CAST(CAST(JSON_UNQUOTE(node) AS DECIMAL(65,20)) AS CHAR))) IN ('','-0') THEN '0' "
         "ELSE TRIM(TRAILING '.' FROM TRIM(TRAILING '0' FROM "
         "CAST(CAST(JSON_UNQUOTE(node) AS DECIMAL(65,20)) AS CHAR))) END "
         "ELSE CASE WHEN CAST(JSON_UNQUOTE(node) AS CHAR)='-0' THEN '0' "
         "ELSE CAST(JSON_UNQUOTE(node) AS CHAR) END END END "
         "ELSE JSON_UNQUOTE(node) END AS val FROM j) q)";
}

std::string sql_canonical(const std::string& col, const NormSpec& spec, Dialect d) {
  switch (spec.type) {
    case LogicalType::kInt:
      return sql_text(col, d);
    case LogicalType::kDecimal:
      return sql_trim_decimal(sql_text(col, d), d);
    case LogicalType::kFloat: {
      const std::string digits = std::to_string(epsilon_digits(spec.epsilon));
      std::string rounded = d == Dialect::kPg
          // openGauss real::numeric truncates to 6 digits; widen via float8 first.
          ? "round((" + col + ")::double precision::numeric, " + digits + ")::text"
          : "CAST(ROUND(" + col + ", " + digits + ") AS CHAR)";
      return sql_trim_decimal(rounded, d);
    }
    case LogicalType::kBool:
      return d == Dialect::kPg ? "CASE WHEN " + col + " THEN '1' ELSE '0' END"
                               : "IF(" + col + ", '1', '0')";
    case LogicalType::kString: {
      std::string s = d == Dialect::kPg ? "(" + col + ")::text"
                                        : "CONVERT(" + col + " USING utf8mb4)";
      if (spec.accent_insensitive) s = sql_accent_fold(s, d);
      if (spec.trim_space) s = d == Dialect::kPg ? "rtrim(" + s + ", ' ')" : "RTRIM(" + s + ")";
      if (spec.case_insensitive) s = d == Dialect::kPg ? "lower(" + s + ")" : "LOWER(" + s + ")";
      return s;
    }
    case LogicalType::kChar: {
      std::string s = d == Dialect::kPg ? "rtrim((" + col + ")::text, ' ')"
                                        : "RTRIM(CONVERT(" + col + " USING utf8mb4))";
      if (spec.accent_insensitive) s = sql_accent_fold(s, d);
      if (spec.case_insensitive) s = d == Dialect::kPg ? "lower(" + s + ")" : "LOWER(" + s + ")";
      return s;
    }
    case LogicalType::kBytes:
      // Digest mode hashes raw bytes so large BLOBs stay bounded; the
      // canonical/report form remains lower-case hex.
      if (spec.digest_bytes) return sql_blob_digest(col, d);
      return d == Dialect::kPg ? "encode(" + col + ", 'hex')"
                               : "LOWER(HEX(" + col + "))";
    case LogicalType::kDate:
      return d == Dialect::kPg ? "to_char(" + col + ", 'YYYY-MM-DD')"
                               : "DATE_FORMAT(" + col + ", '%Y-%m-%d')";
    case LogicalType::kTime:
      return d == Dialect::kPg ? "to_char(" + col + ", 'HH24:MI:SS.US')"
                               : "DATE_FORMAT(" + col + ", '%H:%i:%s.%f')";
    case LogicalType::kTimestamp:
      return d == Dialect::kPg
          ? "to_char(" + col + ", 'YYYY-MM-DD\"T\"HH24:MI:SS.US')"
          : "DATE_FORMAT(" + col + ", '%Y-%m-%dT%H:%i:%s.%f')";
    case LogicalType::kTimestampTz:
      if (d == Dialect::kPg)
        return "to_char((" + col + ") AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US')";
      // The MySQL backend pins the session to UTC, so TIMESTAMP text is UTC.
      return "DATE_FORMAT(" + col + ", '%Y-%m-%dT%H:%i:%s.%f')";
    case LogicalType::kUuid:
      return d == Dialect::kPg ? "lower((" + col + ")::text)"
                               : "LOWER(CAST(" + col + " AS CHAR))";
    case LogicalType::kJson:
      return sql_json_canonical(col, d);
    case LogicalType::kUnknown:
    default:
      return sql_text(col, d);
  }
}

std::string sql_key_text(const std::string& quoted_col, const NormSpec& spec, Dialect d) {
  if (spec.type == LogicalType::kBytes)
    return d == Dialect::kPg ? "encode(" + quoted_col + ", 'hex')"
                             : "LOWER(HEX(" + quoted_col + "))";
  if (spec.type == LogicalType::kInt) return sql_text(quoted_col, d);
  std::string t = d == Dialect::kPg ? "(" + quoted_col + ")::text"
                                   : "CONVERT(" + quoted_col + " USING utf8mb4)";
  if (spec.accent_insensitive) t = sql_accent_fold(t, d);
  if (spec.case_insensitive)
    t = d == Dialect::kPg ? "lower(" + t + ")" : "LOWER(" + t + ")";
  return t;
}

std::string sql_blob_digest(const std::string& quoted_col, Dialect d) {
  // SHA-256 is available on both sides and hashes the raw bytes, so the hex
  // digests are directly comparable across dialects.
  return d == Dialect::kPg ? "encode(sha256(" + quoted_col + "), 'hex')"
                           : "SHA2(" + quoted_col + ", 256)";
}

std::string sql_field(const std::string& quoted_col, const NormSpec& spec, Dialect d) {
  const std::string c = sql_canonical(quoted_col, spec, d);
  return "CASE WHEN " + quoted_col + " IS NULL THEN " + null_marker(d) +
         " ELSE " + sql_escape(c, d) + " END";
}

std::string sql_row_text(const std::vector<std::pair<std::string, NormSpec>>& cols,
                         Dialect d) {
  std::string out;
  for (size_t i = 0; i < cols.size(); ++i) {
    std::string expr = cols[i].first;
    // MySQL text protocol truncates FLOAT to 6 significant digits.  Widen
    // both sides to DOUBLE before formatting so the digest sees the exact
    // binary32 value instead of a rounded decimal string.
    if (d == Dialect::kMySQL && cols[i].second.type == LogicalType::kFloat)
      expr = "CAST((" + expr + ") AS DOUBLE)";
    const std::string one = sql_field(expr, cols[i].second, d);
    if (i) out += d == Dialect::kPg ? " || '|' || " : ", '|', ";
    out += one;
  }
  if (d == Dialect::kMySQL) out = "CONCAT(" + out + ")";
  return out;
}

std::string sql_digest(const std::string& quoted_table, const std::string& where,
                       const std::vector<std::pair<std::string, NormSpec>>& cols,
                       Dialect d, bool check_json_overflow, bool use_md5) {
  std::vector<std::pair<std::string, NormSpec>> digest_cols = cols;
  for (auto& c : digest_cols)
    if (c.second.type == LogicalType::kBytes) c.second.digest_bytes = true;
  const std::string row = sql_row_text(digest_cols, d);
  const std::string seed = quote_lit("mmv3|", d);
  std::string w = where.empty() ? "TRUE" : where;
  const std::string pg_hash = use_md5
      ? "md5(" + seed + "||rt)"
      : "encode(sha256(convert_to(" + seed + "||rt, 'UTF8')), 'hex')";
  const std::string mysql_hash = use_md5
      ? "MD5(CONCAT(" + seed + ",rt))"
      : "SHA2(CONCAT(" + seed + ",rt),256)";

  // v3: one SHA-256 per row. The 32-byte digest is split into two 16-byte
  // halves, each interpreted as a signed 64-bit integer, and each half is
  // summed across rows. Summing (rather than xor) keeps duplicate rows from
  // cancelling; two independent 64-bit sums give a 128-bit checksum with a
  // ~2^64 birthday bound, at half the hashing cost of the old two-seed form.
  if (d == Dialect::kPg) {
    const std::string h1 = "((('x'||substr(d,1,16))::bit(64))::bigint)";
    const std::string h2 = "((('x'||substr(d,17,16))::bit(64))::bigint)";
    return "SELECT count(*)::bigint AS n, COALESCE(sum(h1),0)::text AS h1, "
           "COALESCE(sum(h2),0)::text AS h2, COALESCE(max(rl),0)::bigint AS max_len "
           "FROM (SELECT " + h1 + " AS h1, " + h2 + " AS h2, rl FROM (SELECT " +
           pg_hash + " AS d, " +
           "octet_length(rt) AS rl FROM (SELECT " + row + " AS rt FROM " +
           quoted_table + " WHERE " + w + ") z) x) y";
  }

  const std::string h1 = "CAST(CONV(SUBSTR(d,1,16),16,10) AS SIGNED)";
  const std::string h2 = "CAST(CONV(SUBSTR(d,17,16),16,10) AS SIGNED)";
  std::string ovf;
  if (check_json_overflow) {
    for (const auto& c : digest_cols) {
      if (c.second.type != LogicalType::kJson) continue;
      const std::string expr = sql_canonical(c.first, c.second, d);
      if (!ovf.empty()) ovf += " + ";
      ovf += "CASE WHEN (" + expr + ") LIKE '%__MMV_JSON_OVF__%' THEN 1 ELSE 0 END";
    }
  }
  if (ovf.empty()) {
    return "SELECT COUNT(*) AS n, CAST(COALESCE(SUM(h1),0) AS CHAR) AS h1, "
           "CAST(COALESCE(SUM(h2),0) AS CHAR) AS h2, COALESCE(MAX(rl),0) AS max_len "
           "FROM (SELECT " + h1 + " AS h1, " + h2 + " AS h2, rl FROM (SELECT " +
           mysql_hash + " AS d, OCTET_LENGTH(rt) AS rl FROM "
           "(SELECT " + row + " AS rt FROM " + quoted_table + " WHERE " + w + ") z) x) y";
  }
  return "SELECT COUNT(*) AS n, CAST(COALESCE(SUM(h1),0) AS CHAR) AS h1, "
         "CAST(COALESCE(SUM(h2),0) AS CHAR) AS h2, COALESCE(MAX(rl),0) AS max_len, "
         "COALESCE(SUM(ovf),0) AS ovf "
         "FROM (SELECT " + h1 + " AS h1, " + h2 + " AS h2, rl, ovf FROM (SELECT " +
         mysql_hash + " AS d, OCTET_LENGTH(rt) AS rl, ovf FROM "
         "(SELECT " + row + " AS rt, (" + ovf + ") AS ovf FROM " + quoted_table +
         " WHERE " + w + ") z) x) y";
}

}  // namespace mmv
