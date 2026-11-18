#include "json_ref.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mmv {
namespace {

struct JNode {
  enum Kind { kNull, kBool, kNumber, kString, kObject, kArray } kind = kNull;
  bool b = false;
  bool number_overflow = false;
  std::string text;  // string value or canonical number
  std::vector<std::pair<std::string, JNode>> obj;
  std::vector<JNode> arr;
};

struct Parser {
  const std::string& s;
  size_t pos = 0;
  std::string err;

  explicit Parser(const std::string& in) : s(in) {}

  void ws() {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
      ++pos;
  }

  bool fail(const std::string& m) {
    if (err.empty()) err = m + " at byte " + std::to_string(pos);
    return false;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
      out += (char)cp;
    } else if (cp <= 0x7FF) {
      out += (char)(0xC0 | (cp >> 6));
      out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
      out += (char)(0xE0 | (cp >> 12));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18));
      out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F));
      out += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(uint32_t& v) {
    if (pos + 4 > s.size()) return fail("short \\u escape");
    v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = s[pos++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
      else return fail("bad \\u hex");
    }
    return true;
  }

  bool parse_string(std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return fail("expected string");
    ++pos;
    out.clear();
    while (pos < s.size()) {
      const unsigned char c = (unsigned char)s[pos++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos >= s.size()) return fail("short escape");
        const char e = s[pos++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            uint32_t cp = 0;
            if (!hex4(cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              if (pos + 2 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u')
                return fail("lone high surrogate");
              pos += 2;
              uint32_t lo = 0;
              if (!hex4(lo)) return false;
              if (lo < 0xDC00 || lo > 0xDFFF) return fail("bad low surrogate");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return fail("lone low surrogate");
            }
            append_utf8(out, cp);
            break;
          }
          default: return fail("bad escape");
        }
      } else if (c < 0x20) {
        return fail("control character in string");
      } else {
        out += (char)c;
      }
    }
    return fail("unterminated string");
  }

  static std::string canon_number(const std::string& tok, bool& overflow) {
    overflow = false;
    size_t i = 0;
    bool neg = false;
    if (i < tok.size() && tok[i] == '-') { neg = true; ++i; }
    std::string ip;
    while (i < tok.size() && std::isdigit((unsigned char)tok[i])) ip += tok[i++];
    std::string fp;
    if (i < tok.size() && tok[i] == '.') {
      ++i;
      while (i < tok.size() && std::isdigit((unsigned char)tok[i])) fp += tok[i++];
    }
    int64_t exp = 0;
    if (i < tok.size() && (tok[i] == 'e' || tok[i] == 'E')) {
      ++i;
      bool eneg = false;
      if (i < tok.size() && (tok[i] == '+' || tok[i] == '-')) { eneg = tok[i] == '-'; ++i; }
      int64_t e = 0;
      while (i < tok.size() && std::isdigit((unsigned char)tok[i])) {
        if (e < 100000) e = e * 10 + (tok[i] - '0');
        ++i;
      }
      exp = eneg ? -e : e;
    }
    const bool has_exp = tok.find('e') != std::string::npos ||
                         tok.find('E') != std::string::npos;
    // Match the MySQL canonicalizer: any exponent, >45 integer digits or >20
    // fractional digits is marked as overflow instead of silently rounding.
    if (has_exp || ip.size() > 45 || fp.size() > 20) {
      overflow = true;
      return "__MMV_JSON_OVF__" + tok;
    }
    if (ip.empty()) ip = "0";
    std::string digits = ip + fp;
    int64_t point = (int64_t)ip.size() + exp;
    size_t nz = digits.find_first_not_of('0');
    if (nz == std::string::npos) return "0";
    if (nz > 0) {
      digits.erase(0, nz);
      point -= (int64_t)nz;
    }
    if (point > 100000 || point < -100000) {
      // Avoid unbounded expansion; keep a deterministic scientific form.
      std::string out = digits;
      if (point != 1) out += "e" + std::to_string(point - 1);
      return neg ? "-" + out : out;
    }
    std::string out;
    if (point <= 0) {
      out = "0." + std::string((size_t)(-point), '0') + digits;
    } else if ((size_t)point >= digits.size()) {
      out = digits + std::string((size_t)point - digits.size(), '0');
    } else {
      out = digits.substr(0, (size_t)point) + "." + digits.substr((size_t)point);
    }
    if (out.find('.') != std::string::npos) {
      while (!out.empty() && out.back() == '0') out.pop_back();
      if (!out.empty() && out.back() == '.') out.pop_back();
    }
    if (out.empty()) out = "0";
    if (out == "0") neg = false;
    return neg ? "-" + out : out;
  }

  bool parse_number(JNode& n) {
    const size_t start = pos;
    if (pos < s.size() && s[pos] == '-') ++pos;
    if (pos >= s.size()) return fail("short number");
    if (s[pos] == '0') {
      ++pos;
    } else if (s[pos] >= '1' && s[pos] <= '9') {
      while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
    } else {
      return fail("bad number");
    }
    if (pos < s.size() && s[pos] == '.') {
      ++pos;
      if (pos >= s.size() || !std::isdigit((unsigned char)s[pos])) return fail("bad fraction");
      while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
    }
    if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
      ++pos;
      if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
      if (pos >= s.size() || !std::isdigit((unsigned char)s[pos])) return fail("bad exponent");
      while (pos < s.size() && std::isdigit((unsigned char)s[pos])) ++pos;
    }
    n.kind = JNode::kNumber;
    n.text = canon_number(s.substr(start, pos - start), n.number_overflow);
    return true;
  }

  bool parse_value(JNode& n, int depth) {
    if (depth > 256) return fail("nesting too deep");
    ws();
    if (pos >= s.size()) return fail("unexpected end");
    const char c = s[pos];
    if (c == '{') {
      ++pos;
      n.kind = JNode::kObject;
      ws();
      if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
      for (;;) {
        ws();
        std::string key;
        if (!parse_string(key)) return false;
        ws();
        if (pos >= s.size() || s[pos] != ':') return fail("expected ':'");
        ++pos;
        JNode child;
        if (!parse_value(child, depth + 1)) return false;
        n.obj.emplace_back(std::move(key), std::move(child));
        ws();
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == '}') { ++pos; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      ++pos;
      n.kind = JNode::kArray;
      ws();
      if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
      for (;;) {
        JNode child;
        if (!parse_value(child, depth + 1)) return false;
        n.arr.push_back(std::move(child));
        ws();
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      n.kind = JNode::kString;
      return parse_string(n.text);
    }
    if (c == 't') {
      if (s.compare(pos, 4, "true") != 0) return fail("bad literal");
      pos += 4; n.kind = JNode::kBool; n.b = true; return true;
    }
    if (c == 'f') {
      if (s.compare(pos, 5, "false") != 0) return fail("bad literal");
      pos += 5; n.kind = JNode::kBool; n.b = false; return true;
    }
    if (c == 'n') {
      if (s.compare(pos, 4, "null") != 0) return fail("bad literal");
      pos += 4; n.kind = JNode::kNull; return true;
    }
    return parse_number(n);
  }

  bool parse(JNode& root) {
    if (!parse_value(root, 0)) return false;
    ws();
    if (pos != s.size()) return fail("trailing characters");
    return true;
  }
};

size_t utf8_len(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xC0) != 0x80) ++n;
  return n;
}

std::string json_quote(const std::string& s) {
  std::string o = "\"";
  static const char* h = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\b': o += "\\b"; break;
      case '\f': o += "\\f"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          o += "\\u00";
          o += h[(c >> 4) & 0xF];
          o += h[c & 0xF];
        } else {
          o += (char)c;
        }
    }
  }
  return o + "\"";
}

struct Entry {
  std::string path;
  std::string typ;
  std::string val;
};

void walk(const JNode& n, const std::string& path, std::vector<Entry>& out) {
  Entry e;
  e.path = path;
  switch (n.kind) {
    case JNode::kNull: e.typ = "null"; break;
    case JNode::kBool: e.typ = "bool"; e.val = n.b ? "true" : "false"; break;
    case JNode::kNumber: e.typ = "number"; e.val = n.text; break;
    case JNode::kString: e.typ = "string"; e.val = n.text; break;
    case JNode::kObject: e.typ = "object"; break;
    case JNode::kArray: e.typ = "array"; break;
  }
  out.push_back(std::move(e));
  if (n.kind == JNode::kObject) {
    for (const auto& kv : n.obj)
      walk(kv.second, path + "[" + json_quote(kv.first) + "]", out);
  } else if (n.kind == JNode::kArray) {
    for (size_t i = 0; i < n.arr.size(); ++i)
      walk(n.arr[i], path + "[" + std::to_string(i + 1) + "]", out);
  }
}

}  // namespace

bool json_canonical_ref(const std::string& input, std::string& out, std::string& err,
                        bool* overflow) {
  out.clear();
  err.clear();
  if (overflow) *overflow = false;
  Parser p(input);
  JNode root;
  if (!p.parse(root)) {
    err = p.err.empty() ? "invalid JSON" : p.err;
    return false;
  }
  if (overflow) {
    std::function<void(const JNode&)> scan = [&](const JNode& n) {
      if (n.kind == JNode::kNumber && n.number_overflow) *overflow = true;
      for (const auto& kv : n.obj) scan(kv.second);
      for (const auto& c : n.arr) scan(c);
    };
    scan(root);
  }
  std::vector<Entry> entries;
  walk(root, "$", entries);
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.path < b.path; });
  for (const auto& e : entries) {
    if (!out.empty()) out += '|';
    out += std::to_string(utf8_len(e.path)) + ":" + e.path + "=" + e.typ + ":" +
           std::to_string(utf8_len(e.val)) + ":" + e.val;
  }
  return true;
}

}  // namespace mmv
