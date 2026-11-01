#pragma once

#if USE(BUN_JSC_ADDITIONS)

#include <wtf/Vector.h>

namespace JSC {

// Replaces the deprecated subtags of an ICU locale ID that uloc_canonicalize() produced, as UTS #35 Annex C
// asks: "fre" becomes "fr", "en_UK" becomes "en_GB", "sh" becomes "sr_Latn". Returns false when the ID has no
// canonical form.
bool replaceLocaleAliases(Vector<char, 32>& localeID);

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
