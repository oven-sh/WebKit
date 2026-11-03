/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyUnicode.h"
#include "PyUnicodeDataNew.h"
#include "PyUnicodeDataTables.h"
#include <algorithm>
#include <unicode/uchar.h>
#include <unicode/unorm2.h>
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module unicodedata, on the data of ICU, brought up to the version of Unicode that this version of Python has (the standard
// library insists on it: stringprep, and so the codec idna) by PyUnicodeDataNew.h, which has what ICU does not know yet.
//
// ucd_3_2_0 is the same for Unicode 3.2.0, which IDNA is defined in terms of. ICU says in which version a character was added;
// what has changed since about the characters there were is in PyUnicodeDataTables.h.

enum class UnicodeVersion : bool { Current, Old };

// The value for the character in a table of ranges that is in order.
template<typename Entry, size_t size>
static auto unicodeDataFind(const Entry (&table)[size], char32_t c) -> std::optional<decltype(Entry::value)>
{
    auto* entry = std::ranges::lower_bound(table, c, { }, &Entry::last);
    if (entry != std::end(table) && entry->first <= c)
        return entry->value;
    return std::nullopt;
}

// Whether the character has been added since Unicode 3.2.0.
static bool unicodeDataIsNew(char32_t c)
{
    UVersionInfo age;
    u_charAge(static_cast<UChar32>(c), age);
    // One that ICU has no age for is not assigned, or so lately that ICU has not heard.
    if (!age[0])
        return !!unicodeDataFind(unicodeNewCategories, c);
    return age[0] > 3 || (age[0] == 3 && age[1] > 2);
}

// ---- What ICU says, brought up to date ----

static int unicodeDataCurrentCategory(char32_t c) { return unicodeDataFind(unicodeNewCategories, c).value_or(u_charType(static_cast<UChar32>(c))); }
static int unicodeDataCurrentCombiningClass(char32_t c) { return unicodeDataFind(unicodeNewCombiningClasses, c).value_or(u_getCombiningClass(static_cast<UChar32>(c))); }
static int unicodeDataCurrentGraphemeBreak(char32_t c)
{
    return unicodeDataFind(unicodeNewGraphemeBreaks, c).value_or(u_getIntPropertyValue(static_cast<UChar32>(c), UCHAR_GRAPHEME_CLUSTER_BREAK));
}
static bool unicodeDataIsCurrentlyPictographic(char32_t c)
{
    return unicodeDataFind(unicodeNewPictographic, c).value_or(u_hasBinaryProperty(static_cast<UChar32>(c), UCHAR_EXTENDED_PICTOGRAPHIC));
}
static const UnicodeNewDecomposition* unicodeDataNewDecomposition(char32_t c)
{
    auto* entry = std::ranges::lower_bound(unicodeNewDecompositions, c, { }, &UnicodeNewDecomposition::character);
    return entry != std::end(unicodeNewDecompositions) && entry->character == c ? entry : nullptr;
}

template<size_t size>
static ASCIILiteral unicodeDataNameOf(const ASCIILiteral (&names)[size], int value)
{
    return static_cast<size_t>(value) < size ? names[value] : ""_s;
}

// ---- Arguments ----

// What "must be x, not y" calls the type of an argument.
static String unicodeDataTypeName(JSGlobalObject* globalObject, JSValue value)
{
    return isNone(value) ? "None"_s : typeName(globalObject, value);
}

// `argument` is how messages refer to it: "argument", or "argument 1" if the function takes more than one.
static bool unicodeDataCharacter(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral argument, JSValue value, char32_t& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, function, "() "_s, argument, " must be a unicode character, not "_s, unicodeDataTypeName(globalObject, value));
        return false;
    }
    StrView view(globalObject, strValue(value));
    RETURN_IF_EXCEPTION(scope, false);
    if (view.length() != 1) {
        raiseTypeError(globalObject, scope, function, "(): "_s, argument, " must be a unicode character, not a string of length "_s, view.length());
        return false;
    }
    result = view.at(0);
    return true;
}

static bool unicodeDataString(JSGlobalObject* globalObject, ASCIILiteral function, unsigned position, JSValue value, String& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, function, "() argument "_s, position, " must be str, not "_s, unicodeDataTypeName(globalObject, value));
        return false;
    }
    result = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

// ---- Numbers ----

template<UnicodeVersion version>
static JSValue unicodeDataDecimal(JSGlobalObject* globalObject, JSValue character, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "decimal"_s, "argument 1"_s, character, c)))
        return { };
    int result = u_charDigitValue(static_cast<UChar32>(c));
    if constexpr (version == UnicodeVersion::Old) {
        if (unicodeDataIsNew(c))
            result = -1;
        else if (auto old = unicodeDataFind(unicodeOldDecimals, c))
            result = *old;
    } else
        result = unicodeDataFind(unicodeNewDecimals, c).value_or(result);
    if (result >= 0)
        return jsNumber(result);
    if (defaultValue)
        return defaultValue;
    return raiseValueError(globalObject, scope, "not a decimal"_s);
}

// As in CPython, this one answers the same for both versions.
template<UnicodeVersion>
static JSValue unicodeDataDigit(JSGlobalObject* globalObject, JSValue character, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "digit"_s, "argument 1"_s, character, c)))
        return { };
    int type = u_getIntPropertyValue(static_cast<UChar32>(c), UCHAR_NUMERIC_TYPE);
    if (auto digit = unicodeDataFind(unicodeNewDigits, c)) {
        if (*digit >= 0)
            return jsNumber(*digit);
    } else if (type == U_NT_DECIMAL || type == U_NT_DIGIT)
        return jsNumber(static_cast<int>(u_getNumericValue(static_cast<UChar32>(c))));
    if (defaultValue)
        return defaultValue;
    return raiseValueError(globalObject, scope, "not a digit"_s);
}

template<UnicodeVersion version>
static JSValue unicodeDataNumeric(JSGlobalObject* globalObject, JSValue character, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "numeric"_s, "argument 1"_s, character, c)))
        return { };
    double result = u_getNumericValue(static_cast<UChar32>(c));
    bool isNumeric = result != U_NO_NUMERIC_VALUE;
    if constexpr (version == UnicodeVersion::Old) {
        if (unicodeDataIsNew(c))
            isNumeric = false;
        else if (auto old = unicodeDataFind(unicodeOldNumerics, c)) {
            isNumeric = *old >= 0;
            result = *old;
        }
    } else if (auto current = unicodeDataFind(unicodeNewNumerics, c)) {
        isNumeric = *current >= 0;
        result = *current;
    }
    if (isNumeric)
        RELEASE_AND_RETURN(scope, makeFloat(globalObject, result));
    if (defaultValue)
        return defaultValue;
    return raiseValueError(globalObject, scope, "not a numeric character"_s);
}

// ---- Properties ----

template<UnicodeVersion version>
static int unicodeDataCategoryOf(char32_t c)
{
    if constexpr (version == UnicodeVersion::Old) {
        if (unicodeDataIsNew(c))
            return U_UNASSIGNED;
        if (auto old = unicodeDataFind(unicodeOldCategories, c))
            return *old;
        return u_charType(static_cast<UChar32>(c));
    }
    return unicodeDataCurrentCategory(c);
}

template<UnicodeVersion version>
static JSValue unicodeDataCategory(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // In the order of UCharCategory.
    static constexpr ASCIILiteral names[] = {
        "Cn"_s, "Lu"_s, "Ll"_s, "Lt"_s, "Lm"_s, "Lo"_s, "Mn"_s, "Me"_s, "Mc"_s, "Nd"_s, "Nl"_s, "No"_s, "Zs"_s, "Zl"_s, "Zp"_s,
        "Cc"_s, "Cf"_s, "Co"_s, "Cs"_s, "Pd"_s, "Ps"_s, "Pe"_s, "Pc"_s, "Po"_s, "Sm"_s, "Sc"_s, "Sk"_s, "So"_s, "Pi"_s, "Pf"_s,
    };
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "category"_s, "argument"_s, character, c)))
        return { };
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), unicodeDataNameOf(names, unicodeDataCategoryOf<version>(c))));
}

template<UnicodeVersion version>
static JSValue unicodeDataBidirectional(JSGlobalObject* globalObject, JSValue character)
{
    // In the order of UCharDirection.
    static constexpr ASCIILiteral names[] = {
        "L"_s, "R"_s, "EN"_s, "ES"_s, "ET"_s, "AN"_s, "CS"_s, "B"_s, "S"_s, "WS"_s, "ON"_s, "LRE"_s, "LRO"_s, "AL"_s, "RLE"_s, "RLO"_s,
        "PDF"_s, "NSM"_s, "BN"_s, "FSI"_s, "LRI"_s, "RLI"_s, "PDI"_s,
    };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "bidirectional"_s, "argument"_s, character, c)))
        return { };
    int direction = u_charDirection(static_cast<UChar32>(c));
    if constexpr (version == UnicodeVersion::Old) {
        // The classes that unassigned code points now have by default are more recent.
        if (unicodeDataCategoryOf<version>(c) == U_UNASSIGNED)
            return jsEmptyString(vm);
        if (auto old = unicodeDataFind(unicodeOldDirections, c))
            direction = *old;
    } else
        direction = unicodeDataFind(unicodeNewDirections, c).value_or(direction);
    return makeStr(vm, unicodeDataNameOf(names, direction));
}

template<UnicodeVersion version>
static JSValue unicodeDataCombining(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "combining"_s, "argument"_s, character, c)))
        return { };
    if (version == UnicodeVersion::Old)
        return jsNumber(unicodeDataIsNew(c) ? 0 : u_getCombiningClass(static_cast<UChar32>(c)));
    return jsNumber(unicodeDataCurrentCombiningClass(c));
}

template<UnicodeVersion version>
static JSValue unicodeDataMirrored(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "mirrored"_s, "argument"_s, character, c)))
        return { };
    if constexpr (version == UnicodeVersion::Old) {
        if (unicodeDataIsNew(c))
            return jsNumber(0);
        if (auto old = unicodeDataFind(unicodeOldMirrored, c))
            return jsNumber(*old);
    } else if (auto current = unicodeDataFind(unicodeNewMirrored, c))
        return jsNumber(*current);
    return jsNumber(u_isMirrored(static_cast<UChar32>(c)) ? 1 : 0);
}

template<UnicodeVersion version>
static JSValue unicodeDataEastAsianWidth(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // In the order of UEastAsianWidth.
    static constexpr ASCIILiteral names[] = { "N"_s, "A"_s, "H"_s, "F"_s, "Na"_s, "W"_s };
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "east_asian_width"_s, "argument"_s, character, c)))
        return { };
    int width = u_getIntPropertyValue(static_cast<UChar32>(c), UCHAR_EAST_ASIAN_WIDTH);
    if constexpr (version == UnicodeVersion::Old) {
        if (unicodeDataIsNew(c))
            width = U_EA_NEUTRAL;
        else if (auto old = unicodeDataFind(unicodeOldWidths, c))
            width = *old;
    } else
        width = unicodeDataFind(unicodeNewWidths, c).value_or(width);
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), unicodeDataNameOf(names, width)));
}

// ---- Normalization ----

enum class UnicodeForm : uint8_t { NFC, NFD, NFKC, NFKD };

static const UNormalizer2* unicodeDataNormalizer(UnicodeForm form)
{
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* result = nullptr;
    switch (form) {
    case UnicodeForm::NFC:
        result = unorm2_getNFCInstance(&status);
        break;
    case UnicodeForm::NFD:
        result = unorm2_getNFDInstance(&status);
        break;
    case UnicodeForm::NFKC:
        result = unorm2_getNFKCInstance(&status);
        break;
    case UnicodeForm::NFKD:
        result = unorm2_getNFKDInstance(&status);
        break;
    }
    return U_SUCCESS(status) ? result : nullptr;
}

// Five ideographs whose decompositions were wrong in Unicode 3.2.0, and were corrected later. (NormalizationCorrections.txt)
static char32_t unicodeDataOldDecomposition(char32_t c)
{
    switch (c) {
    case 0x2F868:
        return 0x2136A;
    case 0x2F874:
        return 0x5F33;
    case 0x2F91F:
        return 0x43AB;
    case 0x2F95F:
        return 0x7AAE;
    case 0x2F9BF:
        return 0x4D57;
    default:
        return 0;
    }
}

static void unicodeDataAppendNewDecomposition(Vector<char32_t, 64>& characters, char32_t c, bool isCompatibility, const UNormalizer2* decomposer, bool& failed)
{
    if (auto* entry = unicodeDataNewDecomposition(c)) {
        if (!entry->length || (entry->type != U_DT_CANONICAL && !isCompatibility)) {
            characters.append(c);
            return;
        }
        for (unsigned i = 0; i < entry->length; ++i)
            unicodeDataAppendNewDecomposition(characters, entry->parts[i], isCompatibility, decomposer, failed);
        return;
    }
    char16_t buffer[32];
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = unorm2_getDecomposition(decomposer, static_cast<UChar32>(c), buffer, std::size(buffer), &status);
    failed |= U_FAILURE(status);
    if (length < 0 || U_FAILURE(status)) {
        characters.append(c);
        return;
    }
    for (char32_t part : StringView(std::span<const char16_t>(buffer, length)).codePoints())
        characters.append(part);
}

// The algorithm of UAX #15, character by character, with what ICU knows about each. A null string: ICU failed.
//
// For Unicode 3.2.0, as CPython has it: characters that have been added since do not decompose, but are put in order and composed
// like any others. For the current version: for strings with characters that ICU does not know.
template<UnicodeVersion version>
static String unicodeDataNormalizedSlowly(const String& string, UnicodeForm form)
{
    bool isCompatibility = form == UnicodeForm::NFKC || form == UnicodeForm::NFKD;
    const UNormalizer2* decomposer = unicodeDataNormalizer(isCompatibility ? UnicodeForm::NFKD : UnicodeForm::NFD);
    const UNormalizer2* composer = unicodeDataNormalizer(UnicodeForm::NFC);
    if (!decomposer || !composer)
        return { };

    Vector<char32_t, 64> characters;
    for (char32_t c : StringView(string).codePoints()) {
        if constexpr (version == UnicodeVersion::Current) {
            bool failed = false;
            unicodeDataAppendNewDecomposition(characters, c, isCompatibility, decomposer, failed);
            if (failed)
                return { };
            continue;
        }
        if (char32_t old = unicodeDataOldDecomposition(c))
            c = old;
        char16_t buffer[32];
        UErrorCode status = U_ZERO_ERROR;
        int32_t length = unicodeDataIsNew(c) ? -1 : unorm2_getDecomposition(decomposer, static_cast<UChar32>(c), buffer, std::size(buffer), &status);
        if (U_FAILURE(status))
            return { };
        if (length < 0) {
            characters.append(c);
            continue;
        }
        for (char32_t part : StringView(std::span<const char16_t>(buffer, length)).codePoints())
            characters.append(part);
    }

    auto combiningClass = [](char32_t c) -> int {
        return version == UnicodeVersion::Current ? unicodeDataCurrentCombiningClass(c) : u_getCombiningClass(static_cast<UChar32>(c));
    };
    auto composePair = [&](char32_t first, char32_t second) -> UChar32 {
        if constexpr (version == UnicodeVersion::Current) {
            for (auto& entry : unicodeNewDecompositions) {
                if (entry.composes && entry.parts[0] == first && entry.parts[1] == second)
                    return static_cast<UChar32>(entry.character);
            }
        }
        return unorm2_composePair(composer, static_cast<UChar32>(first), static_cast<UChar32>(second));
    };
    for (size_t i = 1; i < characters.size(); ++i) {
        char32_t c = characters[i];
        int current = combiningClass(c);
        if (!current)
            continue;
        size_t j = i;
        for (; j && combiningClass(characters[j - 1]) > current; --j)
            characters[j] = characters[j - 1];
        characters[j] = c;
    }

    if (form == UnicodeForm::NFC || form == UnicodeForm::NFKC) {
        size_t length = 0;
        std::optional<size_t> starter;
        int lastClass = -1; // Of what follows the starter. A mark is blocked from it by one of the same class, or by another starter.
        for (char32_t c : characters) {
            int current = combiningClass(c);
            if (starter && lastClass < current) {
                UChar32 composite = composePair(characters[*starter], c);
                if (composite >= 0) {
                    characters[*starter] = static_cast<char32_t>(composite);
                    continue;
                }
            }
            if (!current) {
                starter = length;
                lastClass = -1;
            } else
                lastClass = current;
            characters[length++] = c;
        }
        characters.shrink(length);
    }

    StrBuilder builder;
    for (char32_t c : characters)
        appendCodePoint(builder, c);
    return builder.toStringOrNull();
}

// A null string: ICU failed.
static String unicodeDataNormalized(const String& string, UnicodeForm form, UnicodeVersion version)
{
    if (string.containsOnlyASCII())
        return string;
    if (version == UnicodeVersion::Old) {
        String result = unicodeDataNormalizedSlowly<UnicodeVersion::Old>(string, form);
        return result == string ? string : result;
    }
    // Whatever decomposes, composes or combines without ICU knowing is a character that ICU takes to be unassigned.
    if (!string.is8Bit()) {
        for (char32_t c : StringView(string).codePoints()) {
            if (!unicodeDataFind(unicodeNewCategories, c))
                continue;
            String result = unicodeDataNormalizedSlowly<UnicodeVersion::Current>(string, form);
            return result == string ? string : result;
        }
    }
    const UNormalizer2* normalizer = unicodeDataNormalizer(form);
    if (!normalizer)
        return { };

    auto characters = StringView(string).upconvertedCharacters();
    auto source = characters.span();
    UErrorCode status = U_ZERO_ERROR;
    if (unorm2_isNormalized(normalizer, source.data(), source.size(), &status) && U_SUCCESS(status))
        return string;
    status = U_ZERO_ERROR;
    Vector<char16_t, 128> buffer(128);
    int32_t length = unorm2_normalize(normalizer, source.data(), source.size(), buffer.mutableSpan().data(), buffer.size(), &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        buffer.grow(length);
        status = U_ZERO_ERROR;
        length = unorm2_normalize(normalizer, source.data(), source.size(), buffer.mutableSpan().data(), buffer.size(), &status);
    }
    if (U_FAILURE(status))
        return { };
    return String(buffer.span().first(length));
}

static std::optional<UnicodeForm> unicodeDataForm(const String& name)
{
    if (name == "NFC"_s)
        return UnicodeForm::NFC;
    if (name == "NFD"_s)
        return UnicodeForm::NFD;
    if (name == "NFKC"_s)
        return UnicodeForm::NFKC;
    if (name == "NFKD"_s)
        return UnicodeForm::NFKD;
    return std::nullopt;
}

template<UnicodeVersion version>
static JSValue unicodeDataNormalize(JSGlobalObject* globalObject, JSValue formValue, JSValue stringValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String formName;
    String string;
    if (failed(scope, unicodeDataString(globalObject, "normalize"_s, 1, formValue, formName)) || failed(scope, unicodeDataString(globalObject, "normalize"_s, 2, stringValue, string)))
        return { };
    if (string.isEmpty())
        return strValue(stringValue);
    auto form = unicodeDataForm(formName);
    if (!form)
        return raiseValueError(globalObject, scope, "invalid normalization form"_s);
    String result = unicodeDataNormalized(string, *form, version);
    if (result.isNull())
        return raiseSystemError(globalObject, scope, "normalization failed"_s);
    if (result.impl() == string.impl())
        return strValue(stringValue);
    return makeStr(vm, result);
}

template<UnicodeVersion version>
static JSValue unicodeDataIsNormalized(JSGlobalObject* globalObject, JSValue formValue, JSValue stringValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    String formName;
    String string;
    if (failed(scope, unicodeDataString(globalObject, "is_normalized"_s, 1, formValue, formName)) || failed(scope, unicodeDataString(globalObject, "is_normalized"_s, 2, stringValue, string)))
        return { };
    if (string.isEmpty())
        return jsBoolean(true);
    auto form = unicodeDataForm(formName);
    if (!form)
        return raiseValueError(globalObject, scope, "invalid normalization form"_s);
    String result = unicodeDataNormalized(string, *form, version);
    if (result.isNull())
        return raiseSystemError(globalObject, scope, "normalization failed"_s);
    return jsBoolean(result == string);
}

template<UnicodeVersion version>
static JSValue unicodeDataDecomposition(JSGlobalObject* globalObject, JSValue character)
{
    // In the order of UDecompositionType.
    static constexpr ASCIILiteral tags[] = {
        ""_s, ""_s, "<compat> "_s, "<circle> "_s, "<final> "_s, "<font> "_s, "<fraction> "_s, "<initial> "_s, "<isolated> "_s, "<medial> "_s,
        "<narrow> "_s, "<noBreak> "_s, "<small> "_s, "<square> "_s, "<sub> "_s, "<super> "_s, "<vertical> "_s, "<wide> "_s,
    };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "decomposition"_s, "argument"_s, character, c)))
        return { };
    if (version == UnicodeVersion::Old && unicodeDataIsNew(c))
        return jsEmptyString(vm);

    // A Hangul syllable is shown as all its jamo, where ICU would have a syllable and the last of them.
    static constexpr char32_t firstSyllable = 0xAC00;
    static constexpr unsigned vowelCount = 21;
    static constexpr unsigned trailingCount = 28;
    static constexpr unsigned syllableCount = 19 * vowelCount * trailingCount;
    if (c >= firstSyllable && c < firstSyllable + syllableCount) {
        unsigned index = c - firstSyllable;
        unsigned trailing = index % trailingCount;
        StrBuilder builder;
        builder.append(hex(0x1100 + index / (vowelCount * trailingCount), 4), ' ', hex(0x1161 + index / trailingCount % vowelCount, 4));
        if (trailing)
            builder.append(' ', hex(0x11A7 + trailing, 4));
        RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
    }

    if (auto* entry = version == UnicodeVersion::Current ? unicodeDataNewDecomposition(c) : nullptr) {
        StrBuilder builder;
        builder.append(unicodeDataNameOf(tags, entry->type));
        for (unsigned i = 0; i < entry->length; ++i)
            builder.append(i ? " "_s : ""_s, hex(static_cast<unsigned>(entry->parts[i]), 4));
        RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
    }

    const UNormalizer2* normalizer = unicodeDataNormalizer(UnicodeForm::NFKC);
    if (!normalizer)
        return jsEmptyString(vm);
    char16_t buffer[32];
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = unorm2_getRawDecomposition(normalizer, static_cast<UChar32>(c), buffer, std::size(buffer), &status);
    if (U_FAILURE(status) || length <= 0)
        return jsEmptyString(vm);

    StrBuilder builder;
    builder.append(unicodeDataNameOf(tags, u_getIntPropertyValue(static_cast<UChar32>(c), UCHAR_DECOMPOSITION_TYPE)));
    bool isFirst = true;
    for (char32_t part : StringView(std::span<const char16_t>(buffer, length)).codePoints()) {
        if (!isFirst)
            builder.append(' ');
        isFirst = false;
        builder.append(hex(static_cast<unsigned>(part), 4));
    }
    RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
}

// ---- Names ----

template<UnicodeVersion version>
static JSValue unicodeDataName(JSGlobalObject* globalObject, JSValue character, JSValue defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "name"_s, "argument 1"_s, character, c)))
        return { };
    char buffer[256];
    UErrorCode status = U_ZERO_ERROR;
    int32_t length = 0;
    if (version == UnicodeVersion::Current || !unicodeDataIsNew(c))
        length = u_charName(static_cast<UChar32>(c), U_UNICODE_CHAR_NAME, buffer, sizeof(buffer), &status);
    if (U_SUCCESS(status) && length > 0)
        return makeStr(vm, String(byteCast<Latin1Character>(std::span<const char>(buffer, length))));
    if constexpr (version == UnicodeVersion::Current) {
        if (String name = nameOfNewCodePoint(c); !name.isNull())
            return makeStr(vm, name);
    }
    if (defaultValue)
        return defaultValue;
    return raiseValueError(globalObject, scope, "no such name"_s);
}

template<UnicodeVersion version>
static JSValue unicodeDataLookup(JSGlobalObject* globalObject, JSValue nameValue)
{
    static constexpr size_t maxNameLength = 256;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    Vector<uint8_t, maxNameLength> nameBuffer;
    if (isStr(nameValue)) {
        String string = strValue(nameValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        nameBuffer.append(byteCast<uint8_t>(string.utf8().span()));
    } else if (auto* bytes = PyBytes::tryFrom(nameValue))
        nameBuffer.append(bytes->span());
    else if (isBytesLike(nameValue))
        return raiseTypeError(globalObject, scope, "lookup() argument must be read-only bytes-like object, not "_s, typeName(globalObject, nameValue));
    else
        return raiseTypeError(globalObject, scope, "a bytes-like object is required, not '"_s, typeName(globalObject, nameValue), '\'');
    std::span<const uint8_t> name = nameBuffer.span();
    if (name.size() > maxNameLength)
        return raiseKeyError(globalObject, scope, makeStr(vm, "name too long"_s));

    size_t firstNull = find(name, static_cast<uint8_t>(0));
    auto undefined = [&] {
        // As C prints the name.
        auto shown = firstNull == notFound ? name : name.first(firstNull);
        return raiseKeyError(globalObject, scope, makeStr(vm, makeString("undefined character name '"_s, String::fromUTF8ReplacingInvalidSequences(shown), '\'')));
    };
    if (firstNull != notFound)
        return undefined();

    if constexpr (version == UnicodeVersion::Old) {
        // There were no aliases, and no named sequences. (As in CPython, there are the names of characters added since.)
        Vector<char, maxNameLength + 1> buffer;
        buffer.append(byteCast<char>(name));
        buffer.append('\0');
        UErrorCode status = U_ZERO_ERROR;
        UChar32 c = u_charFromName(U_UNICODE_CHAR_NAME, buffer.span().data(), &status);
        if (U_FAILURE(status)) {
            auto added = newCodePointFromName(name);
            if (!added)
                return undefined();
            c = static_cast<UChar32>(*added);
        }
        StrBuilder builder;
        appendCodePoint(builder, c);
        RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
    } else {
        if (auto c = codePointFromName(name)) {
            StrBuilder builder;
            appendCodePoint(builder, *c);
            RELEASE_AND_RETURN(scope, makeStr(globalObject, builder));
        }
        Vector<char, maxNameLength> upperName;
        for (uint8_t byte : name)
            upperName.append(toASCIIUpper(static_cast<char>(byte)));
        auto compare = [](std::span<const char> a, std::span<const char> b) { return std::ranges::lexicographical_compare(a, b); };
        auto* sequence = std::ranges::lower_bound(unicodeNamedSequences, upperName.span(), compare, [](const UnicodeNamedSequence& sequence) { return sequence.name.span(); });
        if (sequence != std::end(unicodeNamedSequences) && equalSpans(sequence->name.span(), std::span<const char>(upperName.span())))
            return makeStr(vm, String(std::span<const char16_t>(sequence->characters, sequence->length)));
        return undefined();
    }
}

// ---- What only the module has ----

static JSValue unicodeDataBlock(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "block"_s, "argument"_s, character, c)))
        return { };
    auto* block = std::ranges::lower_bound(unicodeBlocks, c, { }, &UnicodeBlock::last);
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), block != std::end(unicodeBlocks) && block->first <= c ? block->name : "No_Block"_s));
}

static JSValue unicodeDataGraphemeClusterBreak(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // In the order of UGraphemeClusterBreak. Four of the values are no longer used.
    static constexpr ASCIILiteral names[] = {
        "Other"_s, "Control"_s, "CR"_s, "Extend"_s, "L"_s, "LF"_s, "LV"_s, "LVT"_s, "T"_s, "V"_s, "SpacingMark"_s, "Prepend"_s,
        "Regional_Indicator"_s, "Other"_s, "Other"_s, "Other"_s, "Other"_s, "ZWJ"_s,
    };
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "grapheme_cluster_break"_s, "argument"_s, character, c)))
        return { };
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), unicodeDataNameOf(names, unicodeDataCurrentGraphemeBreak(c))));
}

static JSValue unicodeDataIndicConjunctBreak(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral names[] = { "None"_s, "Consonant"_s, "Extend"_s, "Linker"_s };
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "indic_conjunct_break"_s, "argument"_s, character, c)))
        return { };
    RELEASE_AND_RETURN(scope, makeStr(getVM(globalObject), names[unicodeDataFind(unicodeConjunctBreaks, c).value_or(0)]));
}

static JSValue unicodeDataExtendedPictographic(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "extended_pictographic"_s, "argument"_s, character, c)))
        return { };
    return jsBoolean(unicodeDataIsCurrentlyPictographic(c));
}

static JSValue unicodeDataIsXIDStart(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "isxidstart"_s, "argument"_s, character, c)))
        return { };
    return jsBoolean(isXIDStart(c));
}

static JSValue unicodeDataIsXIDContinue(JSGlobalObject* globalObject, JSValue character)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    char32_t c;
    if (failed(scope, unicodeDataCharacter(globalObject, "isxidcontinue"_s, "argument"_s, character, c)))
        return { };
    return jsBoolean(isXIDContinue(c));
}

// ---- Grapheme clusters ----

namespace UnicodeSegment {
enum : unsigned { String, Start, End, FieldCount };
}

template<unsigned field>
static JSValue unicodeDataField(JSGlobalObject*, JSValue thisValue) { return PyRecord::from(thisValue)->get(field); }

static JSValue unicodeSegmentStr(JSGlobalObject* globalObject, JSValue thisValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* segment = PyRecord::from(thisValue);
    StrView view(globalObject, strValue(segment->get(UnicodeSegment::String)));
    RETURN_IF_EXCEPTION(scope, { });
    return makeStr(vm, view.substring(segment->get(UnicodeSegment::Start).asUInt32(), segment->get(UnicodeSegment::End).asUInt32()));
}

static JSValue unicodeSegmentRepr(JSGlobalObject* globalObject, JSValue thisValue)
{
    PyRecord* segment = PyRecord::from(thisValue);
    return makeStr(getVM(globalObject), makeString("<Segment "_s, segment->get(UnicodeSegment::Start).asUInt32(), ':', segment->get(UnicodeSegment::End).asUInt32(), '>'));
}

static constexpr NativeDef unicodeSegmentMethods[] = {
    PY_SLOT("__str__", unicodeSegmentStr, "($self, /)"),
    PY_SLOT("__repr__", unicodeSegmentRepr, "($self, /)"),
};

static constexpr NativeGetSet unicodeSegmentGetSets[] = {
    PY_MEMBER("start", unicodeDataField<UnicodeSegment::Start>),
    PY_MEMBER("end", unicodeDataField<UnicodeSegment::End>),
};

static constexpr TypeDef unicodeSegmentTypeDef { "unicodedata.Segment"_s, { }, unicodeSegmentMethods, unicodeSegmentGetSets, { } };

namespace UnicodeGraphemes {
// Positions count code points; UnitPosition is where Position is in the UTF-16.
enum : unsigned { String, Start, Position, UnitPosition, End, State, FieldCount };
}

// What of the characters before the position the rules of UAX #29 depend on.
struct UnicodeGraphemeState {
    uint8_t previousBreak { 0 }; // A UGraphemeClusterBreak.
    uint8_t pictographic { 0 }; // GB11. 1: after \p{Extended_Pictographic} Extend*. 2: and then ZWJ. 3: and then \p{Extended_Pictographic}.
    uint8_t conjunct { 0 }; // GB9c. 1: after a consonant and what extends it. 2: with a linker among that. 3: and then a consonant.
    bool oddRegionalIndicators { false }; // GB12, GB13.
};

static JSValue unicodeGraphemesNext(JSGlobalObject* globalObject, JSValue thisValue)
{
    enum : uint8_t { Consonant = 1, Extend, Linker };
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(thisValue);
    unsigned start = iterator->get(UnicodeGraphemes::Start).asUInt32();
    unsigned end = iterator->get(UnicodeGraphemes::End).asUInt32();
    if (start >= end)
        return raiseStopIteration(globalObject, scope);

    JSString* string = strValue(iterator->get(UnicodeGraphemes::String));
    String text = string->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    unsigned position = iterator->get(UnicodeGraphemes::Position).asUInt32();
    unsigned unitPosition = iterator->get(UnicodeGraphemes::UnitPosition).asUInt32();
    auto state = std::bit_cast<UnicodeGraphemeState>(iterator->get(UnicodeGraphemes::State).asUInt32());

    bool foundBreak = false;
    while (position < end && !foundBreak) {
        char32_t c = text[unitPosition];
        unsigned units = 1;
        if (U16_IS_LEAD(c) && unitPosition + 1 < text.length() && U16_IS_TRAIL(text[unitPosition + 1])) {
            c = U16_GET_SUPPLEMENTARY(c, text[unitPosition + 1]);
            units = 2;
        }
        int previous = state.previousBreak;
        int current = unicodeDataCurrentGraphemeBreak(c);
        uint8_t conjunctBreak = unicodeDataFind(unicodeConjunctBreaks, c).value_or(0);

        if (unicodeDataIsCurrentlyPictographic(c))
            state.pictographic = state.pictographic == 2 ? 3 : 1;
        else if ((state.pictographic == 1 || state.pictographic == 3) && current == U_GCB_EXTEND)
            state.pictographic = 1;
        else if ((state.pictographic == 1 || state.pictographic == 3) && current == U_GCB_ZWJ)
            state.pictographic = 2;
        else
            state.pictographic = 0;

        state.oddRegionalIndicators = current == U_GCB_REGIONAL_INDICATOR && !state.oddRegionalIndicators;

        if (conjunctBreak == Consonant)
            state.conjunct = state.conjunct == 2 ? 3 : 1;
        else if (state.conjunct && conjunctBreak == Linker)
            state.conjunct = 2;
        else if (state.conjunct && conjunctBreak == Extend)
            state.conjunct = state.conjunct == 2 ? 2 : 1;
        else
            state.conjunct = 0;

        state.previousBreak = current;

        auto isControl = [](int value) { return value == U_GCB_CR || value == U_GCB_LF || value == U_GCB_CONTROL; };
        auto isBreak = [&] {
            if (previous == U_GCB_CR && current == U_GCB_LF) // GB3
                return false;
            if (isControl(previous) || isControl(current)) // GB4, GB5
                return true;
            if (previous == U_GCB_L && (current == U_GCB_L || current == U_GCB_V || current == U_GCB_LV || current == U_GCB_LVT)) // GB6
                return false;
            if ((previous == U_GCB_LV || previous == U_GCB_V) && (current == U_GCB_V || current == U_GCB_T)) // GB7
                return false;
            if ((previous == U_GCB_LVT || previous == U_GCB_T) && current == U_GCB_T) // GB8
                return false;
            if (current == U_GCB_EXTEND || current == U_GCB_ZWJ || current == U_GCB_SPACING_MARK || previous == U_GCB_PREPEND) // GB9, GB9a, GB9b
                return false;
            if (state.conjunct == 3 || state.pictographic == 3) // GB9c, GB11
                return false;
            if (previous == U_GCB_REGIONAL_INDICATOR && current == U_GCB_REGIONAL_INDICATOR) // GB12, GB13
                return state.oddRegionalIndicators;
            return true; // GB999
        };
        foundBreak = position != start && isBreak();
        ++position;
        unitPosition += units;
    }

    // A break is before the character that showed it, which has been taken into the state already.
    unsigned segmentEnd = foundBreak ? position - 1 : position;
    iterator->set(vm, UnicodeGraphemes::Start, jsNumber(segmentEnd));
    iterator->set(vm, UnicodeGraphemes::Position, jsNumber(position));
    iterator->set(vm, UnicodeGraphemes::UnitPosition, jsNumber(unitPosition));
    iterator->set(vm, UnicodeGraphemes::State, jsNumber(std::bit_cast<uint32_t>(state)));

    PyRecord* segment = PyRecord::create(globalObject, runtime(globalObject).typeFor(unicodeSegmentTypeDef), UnicodeSegment::FieldCount);
    segment->set(vm, UnicodeSegment::String, string);
    segment->set(vm, UnicodeSegment::Start, jsNumber(start));
    segment->set(vm, UnicodeSegment::End, jsNumber(segmentEnd));
    return segment;
}

static JSValue unicodeGraphemesIter(JSGlobalObject*, JSValue thisValue) { return thisValue; }

static constexpr NativeDef unicodeGraphemesMethods[] = {
    PY_SLOT("__iter__", unicodeGraphemesIter, "($self, /)"),
    PY_SLOT("__next__", unicodeGraphemesNext, "($self, /)"),
};

static constexpr TypeDef unicodeGraphemesTypeDef { "unicodedata.GraphemeBreakIterator"_s, { }, unicodeGraphemesMethods, { }, { } };

static JSValue unicodeDataIterGraphemes(JSGlobalObject* globalObject, JSValue stringValue, JSValue startValue, JSValue endValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(stringValue))
        return raiseTypeError(globalObject, scope, "iter_graphemes() argument 1 must be str, not "_s, unicodeDataTypeName(globalObject, stringValue));
    int64_t start = 0;
    int64_t end = std::numeric_limits<int64_t>::max();
    if ((startValue && failed(scope, ssizeArgument(globalObject, startValue, start))) || (endValue && failed(scope, ssizeArgument(globalObject, endValue, end))))
        return { };
    JSString* string = strValue(stringValue);
    StrView view(globalObject, string);
    RETURN_IF_EXCEPTION(scope, { });

    // As for a slice.
    int64_t length = view.length();
    if (end > length)
        end = length;
    else if (end < 0)
        end = std::max<int64_t>(end + length, 0);
    if (start < 0)
        start = std::max<int64_t>(start + length, 0);
    start = std::min(start, length);

    PyRecord* iterator = PyRecord::create(globalObject, runtime(globalObject).typeFor(unicodeGraphemesTypeDef), UnicodeGraphemes::FieldCount);
    iterator->set(vm, UnicodeGraphemes::String, string);
    iterator->set(vm, UnicodeGraphemes::Start, jsNumber(start));
    iterator->set(vm, UnicodeGraphemes::Position, jsNumber(start));
    iterator->set(vm, UnicodeGraphemes::UnitPosition, jsNumber(view.unitOffset(static_cast<unsigned>(start))));
    iterator->set(vm, UnicodeGraphemes::End, jsNumber(end));
    iterator->set(vm, UnicodeGraphemes::State, jsNumber(std::bit_cast<uint32_t>(UnicodeGraphemeState { })));
    return iterator;
}

// ---- The module and unicodedata.UCD ----

// The functions of the module are also the methods of ucd_3_2_0, where they answer for that version.
template<auto function, typename... Arguments>
static JSValue unicodeDataMethod(JSGlobalObject* globalObject, JSValue, Arguments... arguments) { return function(globalObject, arguments...); }

#define FOR_EACH_UCD_FUNCTION(v) \
    v("decimal", unicodeDataDecimal, "chr, default=<unrepresentable>, /", JSValue, JSValue) \
    v("digit", unicodeDataDigit, "chr, default=<unrepresentable>, /", JSValue, JSValue) \
    v("numeric", unicodeDataNumeric, "chr, default=<unrepresentable>, /", JSValue, JSValue) \
    v("category", unicodeDataCategory, "chr, /", JSValue) \
    v("bidirectional", unicodeDataBidirectional, "chr, /", JSValue) \
    v("combining", unicodeDataCombining, "chr, /", JSValue) \
    v("mirrored", unicodeDataMirrored, "chr, /", JSValue) \
    v("east_asian_width", unicodeDataEastAsianWidth, "chr, /", JSValue) \
    v("decomposition", unicodeDataDecomposition, "chr, /", JSValue) \
    v("name", unicodeDataName, "chr, default=<unrepresentable>, /", JSValue, JSValue) \
    v("lookup", unicodeDataLookup, "name, /", JSValue) \
    v("is_normalized", unicodeDataIsNormalized, "form, unistr, /", JSValue, JSValue) \
    v("normalize", unicodeDataNormalize, "form, unistr, /", JSValue, JSValue)

static constexpr NativeDef unicodeDataFunctions[] = {
#define PY_DEFINE_FUNCTION(name, function, signature, ...) PY_FUNCTION(name, function<UnicodeVersion::Current>, "(" signature ")"),
    FOR_EACH_UCD_FUNCTION(PY_DEFINE_FUNCTION)
#undef PY_DEFINE_FUNCTION
    PY_FUNCTION("block", unicodeDataBlock, "(chr, /)"),
    PY_FUNCTION("grapheme_cluster_break", unicodeDataGraphemeClusterBreak, "(chr, /)"),
    PY_FUNCTION("indic_conjunct_break", unicodeDataIndicConjunctBreak, "(chr, /)"),
    PY_FUNCTION("extended_pictographic", unicodeDataExtendedPictographic, "(chr, /)"),
    PY_FUNCTION("iter_graphemes", unicodeDataIterGraphemes, "(unistr, start=0, end=sys.maxsize, /)"),
    PY_FUNCTION("isxidstart", unicodeDataIsXIDStart, "(chr, /)"),
    PY_FUNCTION("isxidcontinue", unicodeDataIsXIDContinue, "(chr, /)"),
};

static constexpr NativeDef unicodeDataUCDMethods[] = {
#define PY_DEFINE_METHOD(name, function, signature, ...) PY_METHOD(name, (unicodeDataMethod<function<UnicodeVersion::Old>, __VA_ARGS__>), "($self, " signature ")"),
    FOR_EACH_UCD_FUNCTION(PY_DEFINE_METHOD)
#undef PY_DEFINE_METHOD
};

#undef FOR_EACH_UCD_FUNCTION

static JSValue unicodeDataUCDVersion(JSGlobalObject* globalObject, JSValue) { return makeStr(getVM(globalObject), "3.2.0"_s); }

static constexpr NativeGetSet unicodeDataUCDGetSets[] = {
    PY_MEMBER("unidata_version", unicodeDataUCDVersion),
};

static constexpr TypeDef unicodeDataUCDTypeDef { "unicodedata.UCD"_s, { }, unicodeDataUCDMethods, unicodeDataUCDGetSets, { } };

void initializeUnicodeDataModule(JSGlobalObject* globalObject, PyModule* module)
{
    VM& vm = getVM(globalObject);
    Runtime& rt = runtime(globalObject);
    module->addFunctions(globalObject, unicodeDataFunctions);

    UVersionInfo version;
    u_getUnicodeVersion(version);
    String icuVersion = makeString(version[0], '.', version[1], '.', version[2]);
    module->add(globalObject, "unidata_version"_s, makeStr(vm, version[0] < unicodeNewMajorVersion ? String(unicodeNewVersion) : icuVersion));

    PyType* type = rt.typeFor(unicodeDataUCDTypeDef);
    module->add(globalObject, "UCD"_s, type);
    module->add(globalObject, "ucd_3_2_0"_s, PyRecord::create(globalObject, type, 0));
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
