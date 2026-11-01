// Differential of JSC::canonicalizeLocaleIDWithoutNullTerminator() against icu::Locale::createCanonical().
#include "config.h"

#include "InitializeThreading.h"
#include "IntlObject.h"
#include <cstdio>
#include <iostream>
#include <string>
#include <unicode/uloc.h>
#include <wtf/Vector.h>

extern "C" int aliasReference(const char* localeID, char* out, int capacity);

static bool toID(const std::string& tag, std::string& id)
{
    char stack[512];
    UErrorCode status = U_ZERO_ERROR;
    int32_t parsed = 0;
    int32_t length = uloc_forLanguageTag(tag.c_str(), stack, sizeof(stack), &parsed, &status);
    if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING || parsed != static_cast<int32_t>(tag.size()))
        return false;
    id.assign(stack, static_cast<size_t>(length));
    return true;
}

static bool engine(const std::string& id, std::string& out)
{
    auto result = JSC::canonicalizeLocaleIDWithoutNullTerminator(id.c_str());
    if (!result)
        return false;
    out.assign(result->span().data(), result->size());
    return true;
}

int main(int argc, char** argv)
{
    WTF::initialize();
    JSC::initialize();
    std::string mode = argc > 1 ? argv[1] : "diff";
    unsigned long maxShown = argc > 2 ? strtoul(argv[2], nullptr, 10) : 40;
    bool inputIsIDs = mode == "ids";
    std::string line;
    unsigned long total = 0, notATag = 0, bothFail = 0, changed = 0, mismatches = 0, notIdempotent = 0, shown = 0;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        ++total;
        std::string id;
        if (inputIsIDs)
            id = line;
        else if (!toID(line, id)) {
            ++notATag;
            continue;
        }
        std::string ours;
        bool oursOK = engine(id, ours);
        char buffer[1024];
        int referenceLength = aliasReference(id.c_str(), buffer, sizeof(buffer));
        bool referenceOK = referenceLength >= 0;
        std::string reference = referenceOK ? std::string(buffer, static_cast<size_t>(referenceLength)) : std::string();
        if (mode == "show") {
            printf("%s\t%s\t%s\t%s\n", line.c_str(), id.c_str(), oursOK ? ours.c_str() : "<fail>", referenceOK ? reference.c_str() : "<bogus>");
            continue;
        }
        if (!oursOK && !referenceOK) {
            ++bothFail;
            continue;
        }
        char plain[1024];
        UErrorCode status = U_ZERO_ERROR;
        int32_t plainLength = uloc_canonicalize(id.c_str(), plain, sizeof(plain), &status);
        if (referenceOK && U_SUCCESS(status) && reference != std::string(plain, static_cast<size_t>(plainLength)))
            ++changed;
        if (oursOK != referenceOK || ours != reference) {
            ++mismatches;
            if (shown++ < maxShown)
                printf("MISMATCH %s (id %s): engine %s | createCanonical %s\n", line.c_str(), id.c_str(), oursOK ? ours.c_str() : "<fail>", referenceOK ? reference.c_str() : "<bogus>");
            continue;
        }
        std::string again;
        if (!engine(ours, again) || again != ours) {
            ++notIdempotent;
            if (shown++ < maxShown)
                printf("NOT IDEMPOTENT %s: %s -> %s\n", line.c_str(), ours.c_str(), again.c_str());
        }
    }
    printf("mode=%s total=%lu not-a-tag=%lu both-fail=%lu changed-by-alias-pass=%lu mismatches=%lu not-idempotent=%lu\n", mode.c_str(), total, notATag, bothFail, changed, mismatches, notIdempotent);
    return mismatches || notIdempotent ? 1 : 0;
}
