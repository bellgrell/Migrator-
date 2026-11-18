#include "unicode.h"

#ifdef HAVE_ICU
#include <unicode/uchar.h>
#include <unicode/ucol.h>
#include <unicode/unorm2.h>
#include <unicode/ustring.h>
#include <unicode/utf16.h>

#include <vector>

namespace mmv {

bool unicode_available() { return true; }


std::string unicode_collation_key(const std::string& s, const std::string& locale,
                                  int strength) {
  UErrorCode st = U_ZERO_ERROR;
  UCollator* coll = ucol_open(locale.c_str(), &st);
  if (U_FAILURE(st)) return "";
  const UCollationStrength cs = strength <= 0   ? UCOL_PRIMARY
                                : strength == 1 ? UCOL_SECONDARY
                                : strength == 2 ? UCOL_TERTIARY
                                                : UCOL_QUATERNARY;
  ucol_setStrength(coll, cs);

  int32_t len = 0;
  u_strFromUTF8(nullptr, 0, &len, s.data(), (int32_t)s.size(), &st);
  if (U_FAILURE(st) && st != U_BUFFER_OVERFLOW_ERROR) { ucol_close(coll); return ""; }
  st = U_ZERO_ERROR;
  std::vector<UChar> buf((size_t)len + 1);
  u_strFromUTF8(buf.data(), (int32_t)buf.size(), &len, s.data(), (int32_t)s.size(), &st);
  if (U_FAILURE(st)) { ucol_close(coll); return ""; }

  int32_t klen = ucol_getSortKey(coll, buf.data(), len, nullptr, 0);
  if (klen <= 0) { ucol_close(coll); return ""; }
  std::vector<uint8_t> key((size_t)klen);
  const int32_t wrote = ucol_getSortKey(coll, buf.data(), len, key.data(), klen);
  ucol_close(coll);
  if (wrote <= 0) return "";
  static const char* h = "0123456789abcdef";
  std::string out;
  out.reserve((size_t)wrote * 2);
  for (int32_t i = 0; i < wrote; ++i) {
    out += h[key[(size_t)i] >> 4];
    out += h[key[(size_t)i] & 0xF];
  }
  return out;
}

std::string unicode_fold(const std::string& s, bool casefold) {
  UErrorCode st = U_ZERO_ERROR;
  int32_t len = 0;
  u_strFromUTF8(nullptr, 0, &len, s.data(), (int32_t)s.size(), &st);
  if (U_FAILURE(st) && st != U_BUFFER_OVERFLOW_ERROR) return s;
  st = U_ZERO_ERROR;
  std::vector<UChar> buf((size_t)len + 1);
  u_strFromUTF8(buf.data(), (int32_t)buf.size(), &len, s.data(), (int32_t)s.size(), &st);
  if (U_FAILURE(st)) return s;

  const UNormalizer2* nfd = unorm2_getNFDInstance(&st);
  if (U_FAILURE(st)) return s;
  int32_t nlen = unorm2_normalize(nfd, buf.data(), len, nullptr, 0, &st);
  if (U_FAILURE(st) && st != U_BUFFER_OVERFLOW_ERROR) return s;
  st = U_ZERO_ERROR;
  std::vector<UChar> norm((size_t)nlen + 1);
  unorm2_normalize(nfd, buf.data(), len, norm.data(), (int32_t)norm.size(), &st);
  if (U_FAILURE(st)) return s;

  std::vector<UChar> stripped;
  stripped.reserve((size_t)nlen);
  int32_t i = 0;
  while (i < nlen) {
    UChar32 cp = 0;
    U16_NEXT(norm.data(), i, nlen, cp);
    const int8_t type = u_charType(cp);
    if (type == U_NON_SPACING_MARK || type == U_COMBINING_SPACING_MARK ||
        type == U_ENCLOSING_MARK)
      continue;
    UChar tmp[2];
    int32_t n = 0;
    U16_APPEND_UNSAFE(tmp, n, cp);
    stripped.insert(stripped.end(), tmp, tmp + n);
  }

  if (casefold) {
    UErrorCode cs = U_ZERO_ERROR;
    int32_t flen = u_strFoldCase(nullptr, 0, stripped.data(), (int32_t)stripped.size(),
                                 U_FOLD_CASE_DEFAULT, &cs);
    if (U_FAILURE(cs) && cs != U_BUFFER_OVERFLOW_ERROR) return s;
    cs = U_ZERO_ERROR;
    std::vector<UChar> folded((size_t)flen + 1);
    u_strFoldCase(folded.data(), (int32_t)folded.size(), stripped.data(),
                  (int32_t)stripped.size(), U_FOLD_CASE_DEFAULT, &cs);
    if (U_FAILURE(cs)) return s;
    stripped.swap(folded);
    stripped.resize((size_t)flen);
  }

  UErrorCode us = U_ZERO_ERROR;
  int32_t ulen = 0;
  u_strToUTF8(nullptr, 0, &ulen, stripped.data(), (int32_t)stripped.size(), &us);
  if (U_FAILURE(us) && us != U_BUFFER_OVERFLOW_ERROR) return s;
  us = U_ZERO_ERROR;
  std::string out((size_t)ulen, '\0');
  u_strToUTF8(&out[0], ulen, &ulen, stripped.data(), (int32_t)stripped.size(), &us);
  if (U_FAILURE(us)) return s;
  out.resize((size_t)ulen);
  return out;
}

}  // namespace mmv
#else
namespace mmv {
bool unicode_available() { return false; }
std::string unicode_fold(const std::string& s, bool) { return s; }
std::string unicode_collation_key(const std::string&, const std::string&, int) {
  return std::string();
}
}  // namespace mmv
#endif
