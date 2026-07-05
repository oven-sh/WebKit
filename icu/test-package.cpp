// Build-time gate for the repacked ICU data (run by the Dockerfiles after
// icu/compress-data.ts): links the patched libicuuc/libicui18n and the
// produced libicudata.a, loads the package through the real readers
// (compact TOC + per-item zstd hook), and exercises every ICU data family a
// JavaScript engine reaches. Any failure exits nonzero and fails the image.
//
//   c++ test-package.cpp -DLINKED_DATA -I<icu>/common -I<icu>/i18n \
//       libicui18n.a libicuuc.a libicudata.a libzstd.a -lpthread -ldl -lm
//
// With -DLINKED_DATA the package comes from the linked libicudata.a exactly
// as it does in Bun (ICU's static-data entry point); the trained dictionary
// comes from the archive's bun_icu_zstd_dict symbols. Also runnable against
// loose files: test-package <icudt.dat> [zstd-dict].
//
// The bun_icu_maybe_decompress definition mirrors Bun's
// src/jsc/bindings/bun_icu_decompress.cpp (the weak hook installed by
// icu/udata-decompress-hook.patch): per-item zstd frames, one shared trained
// dictionary, decoded once and cached for the process lifetime. One deliberate
// difference: Bun returns the raw pointer on any zstd error (graceful
// degradation at runtime); this gate aborts instead, because a frame the
// shipped decoder cannot decode must fail the image build.
#include <unicode/putil.h>
#include <unicode/ubrk.h>
#include <unicode/ucol.h>
#include <unicode/udat.h>
#include <unicode/udata.h>
#include <unicode/uldnames.h>
#include <unicode/uloc.h>
#include <unicode/unorm2.h>
#include <unicode/unum.h>
#include <unicode/ures.h>
#include <unicode/ustring.h>
#include <zstd.h>
#include <mutex>
#include <unordered_map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LINKED_DATA
extern "C" const unsigned char bun_icu_zstd_dict[];
extern "C" const unsigned int bun_icu_zstd_dict_size;
#endif

static ZSTD_DDict* gDict;
static ZSTD_DCtx* gCtx;
static std::unordered_map<const void*, void*>* gCache;
static std::mutex gLock;

extern "C" const void* bun_icu_maybe_decompress(const void* p, int32_t* length) {
  if (!p) return p;
  uint32_t magic;
  memcpy(&magic, p, 4);
  if (magic != ZSTD_MAGICNUMBER) return p;
  std::lock_guard<std::mutex> lock(gLock);
  size_t bound = *length > 0 ? (size_t)*length : (1u << 20);
  unsigned long long dlen = ZSTD_getFrameContentSize(p, bound);
  auto it = gCache->find(p);
  if (it != gCache->end()) { *length = (int32_t)dlen; return it->second; }
  size_t clen = ZSTD_findFrameCompressedSize(p, bound);
  if (ZSTD_isError(clen) || dlen == ZSTD_CONTENTSIZE_ERROR || dlen == ZSTD_CONTENTSIZE_UNKNOWN) {
    fprintf(stderr, "test-package: bad zstd frame (length %d)\n", *length);
    abort();
  }
  void* buf = malloc(dlen);
  size_t r = gDict ? ZSTD_decompress_usingDDict(gCtx, buf, dlen, p, clen, gDict)
                   : ZSTD_decompressDCtx(gCtx, buf, dlen, p, clen);
  if (ZSTD_isError(r)) { fprintf(stderr, "test-package: zstd: %s\n", ZSTD_getErrorName(r)); abort(); }
  (*gCache)[p] = buf;
  *length = (int32_t)dlen;
  return buf;
}

static void chk(UErrorCode st, const char* what) {
  if (U_FAILURE(st)) { printf("FAIL %s: %s\n", what, u_errorName(st)); exit(1); }
}
static void pu(const char* label, const UChar* s, int32_t len) {
  char b[1024];
  UErrorCode e = U_ZERO_ERROR;
  int32_t n = 0;
  u_strToUTF8(b, sizeof b, &n, s, len, &e);
  printf("%s :: %.*s\n", label, U_FAILURE(e) ? 0 : n, b);
}
static void* readAll(const char* path, long* outLen) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  void* d = malloc(n);
  if (fread(d, 1, n, f) != (size_t)n) exit(2);
  fclose(f);
  if (outLen) *outLen = n;
  return d;
}

int main(int argc, char** argv) {
  gCache = new std::unordered_map<const void*, void*>();
  gCtx = ZSTD_createDCtx();
  UErrorCode st = U_ZERO_ERROR;
  if (argc > 1) {
    if (argc > 2) { long n = 0; void* d = readAll(argv[2], &n); gDict = ZSTD_createDDict(d, n); }
    udata_setCommonData(readAll(argv[1], nullptr), &st);
    chk(st, "udata_setCommonData");
  } else {
#ifdef LINKED_DATA
    // The common data is the linked icudt<NN>_dat from libicudata.a (ICU's
    // static-data entry point), exactly as Bun loads it.
    if (bun_icu_zstd_dict_size) gDict = ZSTD_createDDict(bun_icu_zstd_dict, bun_icu_zstd_dict_size);
#else
    fprintf(stderr, "usage: %s <icudt.dat> [dict] (or build with -DLINKED_DATA)\n", argv[0]);
    return 2;
#endif
  }
  uloc_setDefault("en_US", &st);

  printf("uloc_countAvailable>400 :: %d\n", uloc_countAvailable() > 400);
  {
    UErrorCode e = U_ZERO_ERROR;
    ULocaleDisplayNames* dn = uldn_open("de", ULDN_STANDARD_NAMES, &e);
    chk(e, "uldn_open");
    UChar b[256];
    int32_t n = uldn_localeDisplayName(dn, "zh_Hant_TW", b, 256, &e);
    chk(e, "uldn_localeDisplayName");
    pu("uldn de(zh_Hant_TW)", b, n);
    uldn_close(dn);
  }
  {
    UErrorCode e = U_ZERO_ERROR;
    UNumberFormat* nf = unum_open(UNUM_CURRENCY, nullptr, 0, "ja", nullptr, &e);
    chk(e, "unum_open");
    UChar cur[] = { 0x45, 0x55, 0x52, 0 };
    unum_setTextAttribute(nf, UNUM_CURRENCY_CODE, cur, 3, &e);
    UChar b[128];
    int32_t n = unum_formatDouble(nf, 1234.5, b, 128, nullptr, &e);
    chk(e, "unum_formatDouble");
    pu("unum ja EUR", b, n);
    unum_close(nf);
  }
  {
    UErrorCode e = U_ZERO_ERROR;
    UChar tz[64];
    u_uastrcpy(tz, "America/New_York");
    UDateFormat* df = udat_open(UDAT_FULL, UDAT_FULL, "de", tz, -1, nullptr, 0, &e);
    chk(e, "udat_open");
    UChar b[256];
    int32_t n = udat_format(df, 1720107045000.0, b, 256, nullptr, &e);
    chk(e, "udat_format");
    pu("udat de full NY", b, n);
    udat_close(df);
  }
  {
    static const char* const locs[] = { "zh", "zh@collation=stroke", "cs", "sv", "root" };
    for (const char* L : locs) {
      UErrorCode e = U_ZERO_ERROR;
      UCollator* c = ucol_open(L, &e);
      chk(e, "ucol_open");
      UChar a[8], b[8], xs[16], ys[16];
      u_uastrcpy(a, "ch");
      u_uastrcpy(b, "h");
      int32_t xn = u_unescape("\\u4e2d\\u6587", xs, 16), yn = u_unescape("\\u4e2d\\u56fd", ys, 16);
      printf("ucol %s :: %d %d %d\n", L, (int)ucol_strcoll(c, a, -1, b, -1), (int)ucol_strcoll(c, xs, xn, ys, yn), (int)ucol_getStrength(c));
      // The tailoring rule source is stubbed to one code unit, never emptied:
      // JSC keys its ASCII collation fast path on ucol_getRules() being
      // non-empty (IntlCollator.cpp), so an empty stub would silently change
      // Intl.Collator ordering for every tailored locale. Enforce it.
      int32_t rl = 0;
      ucol_getRules(c, &rl);
      printf("ucol %s rules-nonempty :: %d\n", L, rl > 0);
      if (rl <= 0 && 0 != strcmp(L, "root")) { printf("FAIL %s: tailoring rules are empty\n", L); return 1; }
      ucol_close(c);
    }
  }
  {
    static const char* const cases[][2] = {
      { "th", "\\u0e20\\u0e32\\u0e29\\u0e32\\u0e44\\u0e17\\u0e22\\u0e07\\u0e48\\u0e32\\u0e22" },
      { "ja", "\\u543e\\u8f29\\u306f\\u732b\\u3067\\u3042\\u308b" },
      { "zh", "\\u6211\\u4eec\\u90fd\\u7231\\u5199\\u4ee3\\u7801" },
      { "en", "The quick brown fox can't jump 32.3 feet." },
    };
    for (auto& c : cases) {
      UErrorCode e = U_ZERO_ERROR;
      UChar txt[128];
      int32_t n = u_unescape(c[1], txt, 128);
      UBreakIterator* bi = ubrk_open(UBRK_WORD, c[0], txt, n, &e);
      chk(e, "ubrk_open");
      int32_t count = 0;
      for (int32_t p2 = ubrk_first(bi); p2 != UBRK_DONE; p2 = ubrk_next(bi)) count++;
      printf("ubrk %s :: %d\n", c[0], count);
      ubrk_close(bi);
    }
  }
  {
    UErrorCode e = U_ZERO_ERROR;
    UChar txt[128];
    int32_t n = u_unescape("Hi! Dr. Smith went to Washington. He arrived.", txt, 128);
    UBreakIterator* bi = ubrk_open(UBRK_SENTENCE, "en", txt, n, &e);
    chk(e, "ubrk_open sentence");
    int32_t count = 0;
    for (int32_t p2 = ubrk_first(bi); p2 != UBRK_DONE; p2 = ubrk_next(bi)) count++;
    printf("ubrk sentence :: %d\n", count);
    ubrk_close(bi);
  }
  {
    UErrorCode e = U_ZERO_ERROR;
    const UNormalizer2* n2 = unorm2_getNFKCInstance(&e);
    chk(e, "unorm2_getNFKCInstance");
    UChar src[8], dst[32];
    int32_t sn = u_unescape("\\ufb01\\u00bd\\u3300", src, 8);
    int32_t dn = unorm2_normalize(n2, src, sn, dst, 32, &e);
    chk(e, "unorm2_normalize");
    pu("nfkc", dst, dn);
  }
  {
    UChar b[128];
    UErrorCode e = U_ZERO_ERROR;
    int32_t n = uloc_getDisplayName("fr_CA", "de", b, 128, &e);
    chk(e, "uloc_getDisplayName");
    pu("dispname fr_CA/de", b, n);
  }
  {
    // Removed items must be cleanly missing; their kept neighbors must load.
    UErrorCode e = U_ZERO_ERROR;
    UResourceBundle* r = ures_openDirect(nullptr, "genderList", &e);
    printf("genderList missing :: %d\n", e == U_MISSING_RESOURCE_ERROR);
    if (e != U_MISSING_RESOURCE_ERROR) { printf("FAIL genderList should be removed\n"); return 1; }
    if (U_SUCCESS(e)) ures_close(r);
    for (const char* name : { "supplementalData", "zoneinfo64", "res_index", "metadata" }) {
      e = U_ZERO_ERROR;
      r = ures_openDirect(nullptr, name, &e);
      chk(e, name);
      ures_close(r);
    }
    // The whole item classes the Dockerfiles filter out (converters,
    // transliteration, rbnf, character names) and the individually-removed
    // normalizers must be absent; the kept normalizer must not be.
    struct { const char* type; const char* name; UBool present; } probes[] = {
      { "nrm", "nfkc", true },   { "nrm", "nfkc_cf", false },
      { "icu", "cnvalias", false }, { "cnv", "ibm-437_P100-1995", false },
      { "res", "translit/root", false }, { "icu", "unames", false },
    };
    for (auto& pr : probes) {
      e = U_ZERO_ERROR;
      UDataMemory* d = udata_open(nullptr, pr.type, pr.name, &e);
      if (!!U_SUCCESS(e) != !!pr.present) {
        printf("FAIL %s.%s: expected %s, got %s\n", pr.name, pr.type, pr.present ? "present" : "absent", u_errorName(e));
        return 1;
      }
      printf("item %s.%s %s :: ok\n", pr.name, pr.type, pr.present ? "present" : "absent");
      if (d) udata_close(d);
    }
  }
  printf("ICU_PACKAGE_TEST_OK\n");
  return 0;
}
