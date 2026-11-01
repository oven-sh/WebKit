// Reference: ICU's own alias-aware canonicalization, reachable only through the C++ API.
#include <unicode/locid.h>
#include <cstring>
extern "C" int aliasReference(const char* localeID, char* out, int capacity)
{
    icu::Locale locale = icu::Locale::createCanonical(localeID);
    if (locale.isBogus())
        return -1;
    int length = static_cast<int>(strlen(locale.getName()));
    if (length >= capacity)
        return -2;
    memcpy(out, locale.getName(), static_cast<size_t>(length) + 1);
    return length;
}
