// Build-time check for rbbi-clone-allocation-failure.patch. ./Dockerfile links it
// against the libicuuc.a / libicudata.a it just built and runs it; the patched
// source is identical in every other ICU build, so it runs in that one image only.
//
// It does what JSC::IntlSegmenter::segment() does (ubrk_clone a prototype
// iterator that has no text, ubrk_setText, iterate) for each granularity
// Intl.Segmenter exposes, once per ICU allocation that ubrk_clone makes, with
// that one allocation failing. Every such clone has to either be reported as a
// failure (nullptr + U_MEMORY_ALLOCATION_ERROR) or work exactly like an
// unaffected iterator. Without the patch the process segfaults on the first
// injected failure (see the patch header).
//
// With arguments (<character|word|sentence> <failAt>) it runs a single case
// instead, which is how the per-allocation results quoted in the patch header
// were produced against an unpatched build: a crashing case kills the process,
// so the full sweep of an unpatched library needs one process per case.
#include <unicode/ubrk.h>
#include <unicode/uclean.h>
#include <unicode/utypes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static long g_failAt = -1;
static long g_allocations = 0;

static bool failThisOne() {
    long ordinal = g_allocations++;
    return g_failAt >= 0 && ordinal == g_failAt;
}

static void* U_CALLCONV countingAlloc(const void*, size_t size) {
    return failThisOne() ? nullptr : malloc(size);
}

static void* U_CALLCONV countingRealloc(const void*, void* p, size_t size) {
    return failThisOne() ? nullptr : realloc(p, size);
}

static void U_CALLCONV countingFree(const void*, void* p) {
    free(p);
}

// Two flags in a row: the grapheme rules' one look-ahead rule (regional
// indicator pairs) fires here, so a clone that silently lost its
// fLookAheadMatches buffer crashes while iterating instead of passing.
static const UChar kText[] = u"Hello, world! This is a test. Flags \U0001F1FA\U0001F1F8\U0001F1EF\U0001F1F5 and \U0001F44D\U0001F3FD done.";
static const int32_t kTextLength = sizeof(kText) / sizeof(kText[0]) - 1;

static const int kMaxBoundaries = 256;

static int collectBoundaries(UBreakIterator* bi, int32_t* out) {
    int n = 0;
    for (int32_t b = ubrk_first(bi); b != UBRK_DONE; b = ubrk_next(bi)) {
        if (n == kMaxBoundaries)
            return -1;
        out[n++] = b;
    }
    return n;
}

struct Granularity {
    const char* name;
    UBreakIteratorType type;
};

static const Granularity kGranularities[] = {
    { "character", UBRK_CHARACTER },
    { "word", UBRK_WORD },
    { "sentence", UBRK_SENTENCE },
};

// Returns true if the clone made with allocation `failAt` failing (or with no
// failure injected, for failAt < 0) behaved acceptably. *allocations receives
// the number of ICU allocations ubrk_clone attempted.
static bool runCase(const Granularity& g, UBreakIterator* prototype, const int32_t* expected, int expectedCount, long failAt, long* allocations) {
    g_allocations = 0;
    g_failAt = failAt;
    UErrorCode status = U_ZERO_ERROR;
    UBreakIterator* clone = ubrk_clone(prototype, &status);
    g_failAt = -1;
    *allocations = g_allocations;

    if (U_FAILURE(status) || clone == nullptr) {
        bool clean = clone == nullptr && status == U_MEMORY_ALLOCATION_ERROR;
        printf("%-9s failAt=%2ld: ubrk_clone -> %s, %s%s (%ld allocations attempted)\n", g.name, failAt, u_errorName(status), clone ? "non-null iterator" : "nullptr", clean ? "" : "  <-- unexpected", *allocations);
        if (clone)
            ubrk_close(clone);
        return clean;
    }

    ubrk_setText(clone, kText, kTextLength, &status);
    int32_t actual[kMaxBoundaries];
    int actualCount = U_SUCCESS(status) ? collectBoundaries(clone, actual) : -1;
    ubrk_close(clone);

    bool same = actualCount == expectedCount && memcmp(actual, expected, sizeof(int32_t) * expectedCount) == 0;
    printf("%-9s failAt=%2ld: ubrk_clone -> ok, %d boundaries%s (%ld allocations attempted)\n", g.name, failAt, actualCount, same ? "" : "  <-- differ from an unaffected iterator", *allocations);
    return same;
}

int main(int argc, char** argv) {
    const Granularity* only = nullptr;
    long onlyFailAt = -1;
    if (argc == 3) {
        for (const Granularity& g : kGranularities) {
            if (strcmp(argv[1], g.name) == 0)
                only = &g;
        }
        onlyFailAt = strtol(argv[2], nullptr, 10);
    }
    if (argc != 1 && !only) {
        fprintf(stderr, "usage: %s [character|word|sentence <failAt>]\n", argv[0]);
        return 2;
    }

    // A crashing case must not take the lines before it along (stdout is a pipe
    // in a docker build).
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // No u_init(): it wants cnvalias.icu, which the data filter in ./Dockerfile
    // removes, and like JSC we do not need it; ubrk_open loads what it uses.
    UErrorCode status = U_ZERO_ERROR;
    u_setMemoryFunctions(nullptr, countingAlloc, countingRealloc, countingFree, &status);
    if (U_FAILURE(status)) {
        fprintf(stderr, "u_setMemoryFunctions failed: %s\n", u_errorName(status));
        return 2;
    }

    bool ok = true;
    for (const Granularity& g : kGranularities) {
        if (only && only != &g)
            continue;

        // What an iterator that was never cloned produces for kText.
        UBreakIterator* reference = ubrk_open(g.type, "en", kText, kTextLength, &status);
        // What IntlSegmenter keeps: opened without text, cloned per segment() call.
        UBreakIterator* prototype = ubrk_open(g.type, "en", nullptr, 0, &status);
        if (U_FAILURE(status)) {
            fprintf(stderr, "%s: ubrk_open failed: %s\n", g.name, u_errorName(status));
            return 2;
        }
        int32_t expected[kMaxBoundaries];
        int expectedCount = collectBoundaries(reference, expected);
        ubrk_close(reference);
        if (expectedCount <= 1) {
            fprintf(stderr, "%s: reference iterator found no boundaries\n", g.name);
            return 2;
        }

        long allocations = 0;
        if (only) {
            ok = runCase(g, prototype, expected, expectedCount, onlyFailAt, &allocations) && ok;
        } else {
            // An unaffected clone first; it also tells us how many allocations
            // there are to fail.
            ok = runCase(g, prototype, expected, expectedCount, -1, &allocations) && ok;
            if (allocations == 0) {
                fprintf(stderr, "%s: ubrk_clone made no allocations; this check no longer exercises anything\n", g.name);
                return 2;
            }
            long total = allocations;
            for (long failAt = 0; failAt < total; failAt++)
                ok = runCase(g, prototype, expected, expectedCount, failAt, &allocations) && ok;
        }
        ubrk_close(prototype);
    }

    if (!ok) {
        fprintf(stderr, "rbbi-clone-allocation-failure-check: FAILED\n");
        return 1;
    }
    printf("rbbi-clone-allocation-failure-check: ok\n");
    return 0;
}
