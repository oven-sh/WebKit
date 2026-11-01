#include "config.h"
#include "IntlLocaleAliases.h"

#if USE(BUN_JSC_ADDITIONS)

#include <limits>
#include <mutex>
#include <unicode/uenum.h>
#include <unicode/uloc.h>
#include <unicode/ures.h>
#include <wtf/ASCIICType.h>
#include <wtf/BitSet.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/StringCommon.h>
#include <wtf/unicode/icu/ICUHelpers.h>

namespace JSC {

// ICU replaces aliases in icu::Locale::canonicalize() (AliasReplacer in common/locid.cpp), which its C API does
// not reach. This is that algorithm over the C API. It reads the alias tables of the ICU that is linked
// (metadata.res), so the data is ICU's own on every platform. Keep it in step with AliasReplacer.

using LocaleSubtag = std::span<const char>;

class LocaleAliasTables {
    WTF_MAKE_NONCOPYABLE(LocaleAliasTables);
public:
    enum class Kind : uint8_t { Language, Script, Territory, Variant, Subdivision };

    LocaleAliasTables() = default;

    // Null when the data of the linked ICU has no alias tables.
    static const LocaleAliasTables* singleton();

    // Empty when `key` has no alias.
    LocaleSubtag find(Kind, LocaleSubtag key) const;

    // True only for an ID that the replacer leaves as it is. Decides the common shape,
    // language[_Script][_REGION][@keywords], from the bit sets below and the script table; false for every
    // other shape.
    bool isFreeOfAliases(std::span<const char> localeID) const;

private:
    // The characters of the replacement come after those of the key.
    struct Entry {
        unsigned key;
        uint16_t keyLength;
        uint16_t replacementLength;
    };

    static constexpr size_t numberOfKinds = 5;
    static constexpr size_t numberOfLanguageIndexes = 26 * 26 * 27;
    static constexpr size_t numberOfRegionIndexes = 26 * 26 + 1000;

    // Two or three lower-case letters.
    static std::optional<unsigned> indexOfLanguage(LocaleSubtag);
    // Two upper-case letters or three digits.
    static std::optional<unsigned> indexOfRegion(LocaleSubtag);
    static bool hasKeywordWithAliases(std::span<const char> keywords);

    bool load();
    LocaleSubtag keyOf(const Entry& entry) const { return m_characters.subspan(entry.key, entry.keyLength); }
    LocaleSubtag replacementOf(const Entry& entry) const { return m_characters.subspan(entry.key + entry.keyLength, entry.replacementLength); }

    Vector<char> m_characters;
    std::array<Vector<Entry>, numberOfKinds> m_entries;
    // What isFreeOfAliases() asks in place of a search. The keys of the language table that can match an ID
    // without variants are a language, or a language and a region.
    WTF::BitSet<numberOfLanguageIndexes> m_languageKeys;
    WTF::BitSet<numberOfLanguageIndexes> m_languagesOfLanguageAndRegionKeys;
    WTF::BitSet<numberOfRegionIndexes> m_territoryKeys;
};

const LocaleAliasTables* LocaleAliasTables::singleton()
{
    static LazyNeverDestroyed<LocaleAliasTables> tables;
    static bool loaded;
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [] {
        tables.construct();
        loaded = tables->load();
    });
    return loaded ? &tables.get() : nullptr;
}

std::optional<unsigned> LocaleAliasTables::indexOfLanguage(LocaleSubtag subtag)
{
    if (subtag.size() < 2 || subtag.size() > 3 || !std::ranges::all_of(subtag, isASCIILower<char>))
        return std::nullopt;
    unsigned index = (subtag[0] - 'a') * 26 + (subtag[1] - 'a');
    return index * 27 + (subtag.size() == 3 ? subtag[2] - 'a' + 1 : 0);
}

std::optional<unsigned> LocaleAliasTables::indexOfRegion(LocaleSubtag subtag)
{
    if (subtag.size() == 2 && isASCIIUpper(subtag[0]) && isASCIIUpper(subtag[1]))
        return (subtag[0] - 'A') * 26 + (subtag[1] - 'A');
    if (subtag.size() == 3 && std::ranges::all_of(subtag, isASCIIDigit<char>))
        return 26 * 26 + (subtag[0] - '0') * 100 + (subtag[1] - '0') * 10 + (subtag[2] - '0');
    return std::nullopt;
}

bool LocaleAliasTables::load()
{
    static constexpr std::array<ASCIILiteral, numberOfKinds> tableNames { "language"_s, "script"_s, "territory"_s, "variant"_s, "subdivision"_s };

    UErrorCode status = U_ZERO_ERROR;
    auto metadata = std::unique_ptr<UResourceBundle, ICUDeleter<ures_close>>(ures_openDirect(nullptr, "metadata", &status));
    if (U_FAILURE(status))
        return false;
    auto aliases = std::unique_ptr<UResourceBundle, ICUDeleter<ures_close>>(ures_getByKey(metadata.get(), "alias", nullptr, &status));
    if (U_FAILURE(status))
        return false;

    m_characters.reserveInitialCapacity(16 * KB);
    for (size_t kind = 0; kind < numberOfKinds; ++kind) {
        auto table = std::unique_ptr<UResourceBundle, ICUDeleter<ures_close>>(ures_getByKey(aliases.get(), tableNames[kind].characters(), nullptr, &status));
        if (U_FAILURE(status))
            return false;
        auto& entries = m_entries[kind];
        entries.reserveInitialCapacity(ures_getSize(table.get()));
        std::unique_ptr<UResourceBundle, ICUDeleter<ures_close>> item;
        while (ures_hasNext(table.get())) {
            // ICU fills the bundle it is given again, so the loop allocates one.
            item.reset(ures_getNextResource(table.get(), item.release(), &status));
            if (U_FAILURE(status))
                return false;
            const char* key = ures_getKey(item.get());
            int32_t replacementLength = 0;
            const UChar* replacement = ures_getStringByKey(item.get(), "replacement", &replacementLength, &status);
            if (U_FAILURE(status) || !key)
                return false;
            auto keyCharacters = unsafeSpan(key);
            auto replacementCharacters = unsafeMakeSpan(replacement, replacementLength);
            constexpr size_t maxLength = std::numeric_limits<uint16_t>::max();
            if (keyCharacters.size() > maxLength || replacementCharacters.size() > maxLength || !std::ranges::all_of(replacementCharacters, isASCII<UChar>))
                return false;
            entries.append(Entry {
                static_cast<unsigned>(m_characters.size()),
                static_cast<uint16_t>(keyCharacters.size()),
                static_cast<uint16_t>(replacementCharacters.size()),
            });
            m_characters.append(keyCharacters);
            m_characters.appendUsingFunctor(replacementCharacters.size(), [&](size_t index) {
                return static_cast<char>(replacementCharacters[index]);
            });
        }
    }
    m_characters.shrinkToFit();

    // find() searches by key. ICU's tables are in that order already.
    auto byKey = [&](const Entry& a, const Entry& b) {
        return is_lt(compareSpans(keyOf(a), keyOf(b)));
    };
    for (auto& entries : m_entries) {
        if (!std::ranges::is_sorted(entries, byKey))
            std::ranges::sort(entries, byKey);
    }

    for (auto& entry : m_entries[static_cast<size_t>(Kind::Language)]) {
        auto key = keyOf(entry);
        size_t separator = WTF::find(key, '_');
        auto language = indexOfLanguage(key.first(std::min(separator, key.size())));
        if (!language)
            continue;
        if (separator == notFound)
            m_languageKeys.set(*language);
        else if (indexOfRegion(key.subspan(separator + 1)))
            m_languagesOfLanguageAndRegionKeys.set(*language);
    }
    for (auto& entry : m_entries[static_cast<size_t>(Kind::Territory)]) {
        if (auto region = indexOfRegion(keyOf(entry)))
            m_territoryKeys.set(*region);
    }
    return true;
}

LocaleSubtag LocaleAliasTables::find(Kind kind, LocaleSubtag key) const
{
    auto& entries = m_entries[static_cast<size_t>(kind)];
    auto entry = std::ranges::lower_bound(entries, key, [](LocaleSubtag a, LocaleSubtag b) {
        return is_lt(compareSpans(a, b));
    }, [&](const Entry& entry) {
        return keyOf(entry);
    });
    if (entry == entries.end() || !equalSpans(keyOf(*entry), key))
        return { };
    return replacementOf(*entry);
}

// The keywords of an ICU locale ID are "key=value;key=value". The replacer changes the values of three.
bool LocaleAliasTables::hasKeywordWithAliases(std::span<const char> keywords)
{
    while (true) {
        auto key = keywords.first(std::min(WTF::find(keywords, '='), keywords.size()));
        if (equalSpans(key, "rg"_s.span()) || equalSpans(key, "sd"_s.span()) || equalSpans(key, "t"_s.span()))
            return true;
        size_t separator = WTF::find(keywords, ';');
        if (separator == notFound)
            return false;
        keywords = keywords.subspan(separator + 1);
    }
}

bool LocaleAliasTables::isFreeOfAliases(std::span<const char> localeID) const
{
    size_t position = 0;
    auto isAtEndOfSubtag = [&] {
        return position == localeID.size() || localeID[position] == '_' || localeID[position] == '@';
    };

    while (position < localeID.size() && isASCIILower(localeID[position]))
        ++position;
    auto language = indexOfLanguage(localeID.first(position));
    if (!language || !isAtEndOfSubtag())
        return false;

    LocaleSubtag script;
    if (localeID.size() - position >= 5 && localeID[position] == '_' && isASCIIUpper(localeID[position + 1]) && std::ranges::all_of(localeID.subspan(position + 2, 3), isASCIILower<char>)) {
        script = localeID.subspan(position + 1, 4);
        position += 5;
        if (!isAtEndOfSubtag())
            return false;
    }

    std::optional<unsigned> region;
    if (position < localeID.size() && localeID[position] == '_') {
        size_t start = ++position;
        while (!isAtEndOfSubtag())
            ++position;
        region = indexOfRegion(localeID.subspan(start, position - start));
        if (!region)
            return false;
    }

    // What is left is a variant, or the keywords.
    if (position < localeID.size() && (localeID[position] != '@' || hasKeywordWithAliases(localeID.subspan(position + 1))))
        return false;

    if (m_languageKeys.get(*language))
        return false;
    if (region && (m_languagesOfLanguageAndRegionKeys.get(*language) || m_territoryKeys.get(*region)))
        return false;
    return script.empty() || find(Kind::Script, script).empty();
}

class LocaleAliasReplacer {
public:
    explicit LocaleAliasReplacer(const LocaleAliasTables& tables)
        : m_tables(tables)
    {
    }

    bool replace(Vector<char, 32>& localeID, bool isLanguageOfTransformedExtension = false);

private:
    using Kind = LocaleAliasTables::Kind;

    bool parse(const char* localeID);
    bool replaceLanguage(bool checkLanguage, bool checkRegion, bool checkVariants);
    bool replaceTerritory();
    bool replaceScript();
    bool replaceVariant();
    bool replaceSubdivision(LocaleSubtag, Vector<char, 32>& output) const;
    bool replaceTransformedExtensions(std::span<const char> value, Vector<char, 32>& output) const;
    bool replaceKeywords(const char* localeID, Vector<char, 64>& output, bool isLanguageOfTransformedExtension) const;
    void appendSubtags(Vector<char, 64>& output);

    static void sortSubtags(Vector<LocaleSubtag, 4>&);
    static size_t findTransformKey(std::span<const char>, size_t start);

    const LocaleAliasTables& m_tables;
    Vector<char, 16> m_languageCharacters;
    Vector<char, 8> m_scriptCharacters;
    Vector<char, 8> m_regionCharacters;
    Vector<char, 32> m_variantCharacters;
    // An empty subtag is one that the ID does not have.
    LocaleSubtag m_language;
    LocaleSubtag m_script;
    LocaleSubtag m_region;
    Vector<LocaleSubtag, 4> m_variants;
    bool m_failed { false };
};

void LocaleAliasReplacer::sortSubtags(Vector<LocaleSubtag, 4>& subtags)
{
    std::ranges::sort(subtags, [](LocaleSubtag a, LocaleSubtag b) {
        return is_lt(compareSpans(a, b));
    });
}

bool LocaleAliasReplacer::parse(const char* localeID)
{
    if (U_FAILURE(callBufferProducingFunction(uloc_getLanguage, localeID, m_languageCharacters))
        || U_FAILURE(callBufferProducingFunction(uloc_getScript, localeID, m_scriptCharacters))
        || U_FAILURE(callBufferProducingFunction(uloc_getCountry, localeID, m_regionCharacters))
        || U_FAILURE(callBufferProducingFunction(uloc_getVariant, localeID, m_variantCharacters)))
        return false;
    m_language = m_languageCharacters.span();
    m_script = m_scriptCharacters.span();
    m_region = m_regionCharacters.span();

    for (auto& character : m_variantCharacters)
        character = toASCIILower(character);
    std::span<const char> variants = m_variantCharacters.span();
    while (!variants.empty()) {
        auto variant = variants.first(std::min(WTF::find(variants, '_'), variants.size()));
        if (!variant.empty() && !m_variants.containsIf([&](LocaleSubtag other) { return equalSpans(other, variant); }))
            m_variants.constructAndAppend(variant);
        variants = variants.subspan(std::min(variant.size() + 1, variants.size()));
    }
    sortSubtags(m_variants);
    return true;
}

// The keys of the language table are language, language_REGION, language_variant, language_REGION_variant
// and und_variant. A replacement is language[_Script][_REGION][_variant], and four legacy ones go on with a
// private use extension, which uloc_forLanguageTag() has already dealt with.
bool LocaleAliasReplacer::replaceLanguage(bool checkLanguage, bool checkRegion, bool checkVariants)
{
    if ((checkRegion && m_region.empty()) || (checkVariants && m_variants.isEmpty()))
        return false;

    size_t numberOfVariants = checkVariants ? m_variants.size() : 1;
    for (size_t index = 0; index < numberOfVariants; ++index) {
        LocaleSubtag searchVariant;
        if (checkVariants && m_variants[index].size() >= 4)
            searchVariant = m_variants[index];

        Vector<char, 32> key;
        key.append(checkLanguage ? m_language : "und"_s.span());
        if (checkRegion) {
            key.append('_');
            key.append(m_region);
        }
        if (!searchVariant.empty()) {
            key.append('_');
            key.append(searchVariant);
        }
        auto replacement = m_tables.find(Kind::Language, key.span());
        if (replacement.empty())
            continue;

        LocaleSubtag replacedScript;
        LocaleSubtag replacedRegion;
        LocaleSubtag replacedVariant;
        bool hasExtensions = false;
        auto nextSubtag = [&] {
            auto subtag = replacement.first(std::min(WTF::find(replacement, '_'), replacement.size()));
            replacement = replacement.subspan(std::min(subtag.size() + 1, replacement.size()));
            return subtag;
        };
        auto replacedLanguage = nextSubtag();
        if (!replacement.empty()) {
            auto subtag = nextSubtag();
            if (subtag.size() == 4 && isASCIIAlpha(subtag[0])) {
                replacedScript = subtag;
                subtag = nextSubtag();
            }
            if (subtag.size() >= 2 && subtag.size() <= 3) {
                replacedRegion = subtag;
                subtag = nextSubtag();
            }
            if (subtag.size() >= 4) {
                replacedVariant = subtag;
                subtag = nextSubtag();
            }
            hasExtensions = !subtag.empty();
        }

        if (equalSpans(replacedLanguage, "und"_s.span()))
            replacedLanguage = m_language;
        // A subtag of the ID stays where the replacement has one too. A subtag that the key matched goes
        // where the replacement has none.
        auto deleteOrReplace = [](LocaleSubtag current, bool isPartOfKey, LocaleSubtag replaced) {
            if (!replaced.empty())
                return current.empty() ? replaced : current;
            return isPartOfKey ? LocaleSubtag { } : current;
        };
        replacedScript = deleteOrReplace(m_script, false, replacedScript);
        replacedRegion = deleteOrReplace(m_region, checkRegion, replacedRegion);
        replacedVariant = deleteOrReplace(searchVariant, !searchVariant.empty(), replacedVariant);

        if (equalSpans(m_language, replacedLanguage) && equalSpans(m_script, replacedScript) && equalSpans(m_region, replacedRegion) && equalSpans(searchVariant, replacedVariant) && !hasExtensions)
            continue;

        m_language = replacedLanguage;
        m_script = replacedScript;
        m_region = replacedRegion;
        if (!searchVariant.empty()) {
            if (replacedVariant.empty())
                m_variants.removeAt(index);
            else
                m_variants[index] = replacedVariant;
        }
        return true;
    }
    return false;
}

bool LocaleAliasReplacer::replaceTerritory()
{
    if (m_region.empty())
        return false;
    auto replacement = m_tables.find(Kind::Territory, m_region);
    if (replacement.empty())
        return false;

    size_t firstSpace = WTF::find(replacement, ' ');
    if (firstSpace != notFound) {
        // Several regions replace this one: the one that the language is most likely in, or the first.
        Vector<char, 32> likelyInput;
        likelyInput.append(m_language.empty() ? "und"_s.span() : m_language);
        if (!m_script.empty()) {
            likelyInput.append('_');
            likelyInput.append(m_script);
        }
        likelyInput.append('\0');
        Vector<char, 32> maximized;
        if (U_FAILURE(callBufferProducingFunction(uloc_addLikelySubtags, likelyInput.span().data(), maximized))) {
            m_failed = true;
            return false;
        }
        maximized.append('\0');
        Vector<char, 8> likelyRegion;
        if (U_FAILURE(callBufferProducingFunction(uloc_getCountry, maximized.span().data(), likelyRegion)))
            likelyRegion.clear();

        size_t likely = likelyRegion.isEmpty() ? notFound : WTF::find(replacement, likelyRegion.span());
        replacement = likely == notFound ? replacement.first(firstSpace) : replacement.subspan(likely, likelyRegion.size());
    }
    m_region = replacement;
    return true;
}

bool LocaleAliasReplacer::replaceScript()
{
    if (m_script.empty())
        return false;
    auto replacement = m_tables.find(Kind::Script, m_script);
    if (replacement.empty())
        return false;
    m_script = replacement;
    return true;
}

bool LocaleAliasReplacer::replaceVariant()
{
    for (auto& variant : m_variants) {
        auto replacement = m_tables.find(Kind::Variant, variant);
        if (replacement.empty() || equalSpans(variant, replacement))
            continue;
        bool wasHeploc = equalSpans(variant, "heploc"_s.span());
        variant = replacement;
        // ICU's own exception: "hepburn-heploc" becomes "alalc97".
        if (wasHeploc) {
            m_variants.removeFirstMatching([](LocaleSubtag other) {
                return equalSpans(other, "hepburn"_s.span());
            });
        }
        return true;
    }
    return false;
}

bool LocaleAliasReplacer::replaceSubdivision(LocaleSubtag subdivision, Vector<char, 32>& output) const
{
    auto replacement = m_tables.find(Kind::Subdivision, subdivision);
    if (replacement.empty())
        return false;
    replacement = replacement.first(std::min(WTF::find(replacement, ' '), replacement.size()));
    if (replacement.size() >= 2 && replacement.size() <= 8) {
        output.append(replacement);
        // A region in the place of a subdivision is the region's unknown subdivision.
        if (replacement.size() == 2)
            output.append("zzzz"_s.span());
    }
    return true;
}

// ultag_getTKeyStart(): a tkey is a letter and a digit.
size_t LocaleAliasReplacer::findTransformKey(std::span<const char> value, size_t start)
{
    while (start <= value.size()) {
        size_t end = std::min(WTF::find(value, '-', start), value.size());
        if (end - start == 2 && isASCIIAlpha(value[start]) && isASCIIDigit(value[start + 1]))
            return start;
        start = end + 1;
    }
    return notFound;
}

// The value of "t" is a language tag and then fields: "hi-latn-h0-hybrid". The language tag is canonicalized as
// a locale of its own and the fields are sorted.
bool LocaleAliasReplacer::replaceTransformedExtensions(std::span<const char> value, Vector<char, 32>& output) const
{
    size_t key = findTransformKey(value, 0);
    size_t languageTagLength = key == notFound ? value.size() : (key ? key - 1 : 0);
    if (languageTagLength) {
        Vector<char, 32> languageTag;
        languageTag.append(value.first(languageTagLength));
        languageTag.append('\0');
        Vector<char, 32> localeID;
        int32_t parsedLength = 0;
        if (U_FAILURE(callBufferProducingFunction(uloc_forLanguageTag, languageTag.span().data(), localeID, &parsedLength)) || static_cast<size_t>(parsedLength) != languageTagLength)
            return false;
        localeID.append('\0');
        Vector<char, 32> canonical;
        if (U_FAILURE(callBufferProducingFunction(uloc_canonicalize, localeID.span().data(), canonical)))
            return false;
        if (!LocaleAliasReplacer(m_tables).replace(canonical, true))
            return false;
        canonical.append('\0');
        if (U_FAILURE(callBufferProducingFunction(uloc_toLanguageTag, canonical.span().data(), output, false)))
            return false;
        for (auto& character : output)
            character = toASCIILower(character);
    }

    if (key == notFound)
        return true;
    Vector<std::span<const char>, 4> fields;
    while (key != notFound) {
        size_t fieldValue = WTF::find(value, '-', key);
        if (fieldValue == notFound)
            return false;
        size_t nextKey = findTransformKey(value, fieldValue + 1);
        fields.constructAndAppend(nextKey == notFound ? value.subspan(key) : value.subspan(key, nextKey - 1 - key));
        key = nextKey;
    }
    sortSubtags(fields);
    for (auto field : fields) {
        if (!output.isEmpty())
            output.append('-');
        Vector<char, 8> fieldKey;
        fieldKey.append(field.first(2));
        fieldKey.append('\0');
        Vector<char, 32> fieldValue;
        fieldValue.append(field.subspan(3));
        fieldValue.append('\0');
        output.append(field.first(3));
        const char* type = uloc_toUnicodeLocaleType(fieldKey.span().data(), fieldValue.span().data());
        output.append(type ? unsafeSpan(type) : field.subspan(3));
    }
    return true;
}

// Appends the keywords of `localeID`, "@key=value;…", with the values of "rg", "sd" and "t" replaced.
bool LocaleAliasReplacer::replaceKeywords(const char* localeID, Vector<char, 64>& output, bool isLanguageOfTransformedExtension) const
{
    Vector<char, 64> replaced;
    replaced.append(unsafeSpanIncludingNullTerminator(localeID));
    bool changed = false;

    UErrorCode status = U_ZERO_ERROR;
    auto keywords = std::unique_ptr<UEnumeration, ICUDeleter<uenum_close>>(uloc_openKeywords(localeID, &status));
    if (U_FAILURE(status))
        return false;
    while (keywords) {
        const char* keyword = uenum_next(keywords.get(), nullptr, &status);
        if (U_FAILURE(status))
            return false;
        if (!keyword)
            break;
        auto key = unsafeSpan(keyword);
        bool isSubdivision = equalSpans(key, "rg"_s.span()) || equalSpans(key, "sd"_s.span());
        if (!isSubdivision && !equalSpans(key, "t"_s.span()))
            continue;
        Vector<char, 32> value;
        if (U_FAILURE(callBufferProducingFunction(uloc_getKeywordValue, localeID, keyword, value)))
            continue;
        Vector<char, 32> replacement;
        if (isSubdivision) {
            if (!replaceSubdivision(value.span(), replacement))
                continue;
        } else if (isLanguageOfTransformedExtension || !replaceTransformedExtensions(value.span(), replacement))
            return false;
        replacement.append('\0');

        // An empty value takes the keyword out.
        replaced.grow(replaced.size() + key.size() + replacement.size() + 1);
        status = U_ZERO_ERROR;
        auto length = uloc_setKeywordValue(keyword, replacement.span().data(), replaced.mutableSpan().data(), replaced.size(), &status);
        if (U_FAILURE(status))
            return false;
        replaced.shrink(length + 1);
        changed = true;
    }

    auto source = changed ? replaced.span().first(replaced.size() - 1) : unsafeSpan(localeID);
    size_t keywordsStart = WTF::find(source, '@');
    if (keywordsStart != notFound)
        output.append(source.subspan(keywordsStart));
    return true;
}

void LocaleAliasReplacer::appendSubtags(Vector<char, 64>& output)
{
    output.append(m_language);
    if (!m_script.empty()) {
        output.append('_');
        output.append(m_script);
    }
    if (!m_region.empty()) {
        output.append('_');
        output.append(m_region);
    }
    if (m_variants.isEmpty())
        return;
    // A variant is the fourth subtag of an ICU locale ID, after an empty region if there is none.
    if (m_script.empty() && m_region.empty())
        output.append('_');
    sortSubtags(m_variants);
    for (auto variant : m_variants) {
        output.append('_');
        output.appendUsingFunctor(variant.size(), [&](size_t index) {
            return toASCIIUpper(variant[index]);
        });
    }
}

bool LocaleAliasReplacer::replace(Vector<char, 32>& localeID, bool isLanguageOfTransformedExtension)
{
    Vector<char, 64> original;
    original.append(localeID.span());
    original.append('\0');
    if (!parse(original.span().data()))
        return false;

    // A replacement can make another rule match, so the rules start over after each. No chain in CLDR's data
    // is longer than three.
    unsigned numberOfReplacements = 0;
    while (replaceLanguage(true, true, true)
        || replaceLanguage(true, true, false)
        || replaceLanguage(true, false, true)
        || replaceLanguage(true, false, false)
        || replaceLanguage(false, false, true)
        || replaceTerritory()
        || replaceScript()
        || replaceVariant()) {
        if (++numberOfReplacements > 10)
            return false;
    }
    if (m_failed)
        return false;

    bool hasKeywords = WTF::find(localeID.span(), '@') != notFound;
    if (!numberOfReplacements && m_variants.size() <= 1 && !hasKeywords)
        return true;

    Vector<char, 64> replaced;
    appendSubtags(replaced);
    if (hasKeywords && !replaceKeywords(original.span().data(), replaced, isLanguageOfTransformedExtension))
        return false;
    if (equalSpans(replaced.span(), localeID.span()))
        return true;

    // ICU parses what it put together once more, which takes out what is left of a removed subtag.
    replaced.append('\0');
    localeID.clear();
    return U_SUCCESS(callBufferProducingFunction(uloc_getName, replaced.span().data(), localeID));
}

bool replaceLocaleAliases(Vector<char, 32>& localeID)
{
    // ICU makes the same two exceptions, so that the default locale reads no table.
    if (localeID.isEmpty() || equalSpans(localeID.span(), "en_US"_s.span()) || equalSpans(localeID.span(), "en"_s.span()))
        return true;

    auto* tables = LocaleAliasTables::singleton();
    if (!tables)
        return false;
    if (tables->isFreeOfAliases(localeID.span())) {
#if ASSERT_ENABLED
        auto replaced = localeID;
        ASSERT(LocaleAliasReplacer(*tables).replace(replaced) && replaced == localeID);
#endif
        return true;
    }
    return LocaleAliasReplacer(*tables).replace(localeID);
}

} // namespace JSC

#endif // USE(BUN_JSC_ADDITIONS)
