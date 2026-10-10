/*
 * Copyright (C) 2020-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "IntlSegmenter.h"

#include "IntlObjectInlines.h"
#include "IntlSegmentDataObject.h"
#include "IntlSegments.h"
#include "IntlWorkaround.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"

namespace JSC {

const ClassInfo IntlSegmenter::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(IntlSegmenter) };

IntlSegmenter* IntlSegmenter::create(VM& vm, Structure* structure)
{
    auto* object = new (NotNull, allocateCell<IntlSegmenter>(vm)) IntlSegmenter(vm, structure);
    object->finishCreation(vm);
    return object;
}

Structure* IntlSegmenter::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(ObjectType, StructureFlags), info());
}

IntlSegmenter::IntlSegmenter(VM& vm, Structure* structure)
    : Base(vm, structure)
{
}

#if USE(BUN_JSC_ADDITIONS)
// A bit for each code unit, asked of ICU 256 at a time when the first of them is met, for the process.
static std::array<std::atomic<uint64_t>, (1 << 16) / 64> s_joinsNoNeighbour;
static std::array<std::atomic<bool>, 256> s_isAskedAbout;

bool IntlSegmenter::joinsNoNeighbourByProperty(char16_t character)
{
    unsigned block = character >> 8;
    if (!s_isAskedAbout[block].load(std::memory_order_acquire)) [[unlikely]] {
        for (unsigned word = 0; word < 4; ++word) {
            uint64_t bits = 0;
            for (unsigned bit = 0; bit < 64; ++bit) {
                UChar32 asked = block << 8 | word << 6 | bit;
                if (U16_IS_SURROGATE(asked))
                    continue;
                int32_t property = u_getIntPropertyValue(asked, UCHAR_GRAPHEME_CLUSTER_BREAK);
                if (property == U_GCB_OTHER || property == U_GCB_CONTROL)
                    bits |= 1ull << bit;
            }
            s_joinsNoNeighbour[block * 4 + word].store(bits, std::memory_order_relaxed);
        }
        s_isAskedAbout[block].store(true, std::memory_order_release);
    }
    return s_joinsNoNeighbour[character >> 6].load(std::memory_order_relaxed) >> (character & 63) & 1;
}

// By the ICU that is loaded, which on some systems is the system's.
static bool charactersBelowLimitJoinNothing()
{
    static const bool result = [] {
        for (UChar32 character = 0; character < IntlSegmenter::firstCharacterThatMayJoin; ++character) {
            switch (u_getIntPropertyValue(character, UCHAR_GRAPHEME_CLUSTER_BREAK)) {
            case U_GCB_OTHER:
            case U_GCB_CONTROL:
                break;
            case U_GCB_CR:
                if (character != '\r')
                    return false;
                break;
            case U_GCB_LF:
                if (character != '\n')
                    return false;
                break;
            default:
                return false;
            }
        }
        return true;
    }();
    return result;
}
#endif

// https://tc39.es/proposal-intl-segmenter/#sec-intl.segmenter
void IntlSegmenter::initializeSegmenter(JSGlobalObject* globalObject, JSValue locales, JSValue optionsValue)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto requestedLocales = canonicalizeLocaleList(globalObject, locales);
    RETURN_IF_EXCEPTION(scope, void());

    JSObject* options = intlGetOptionsObject(globalObject, optionsValue);
    RETURN_IF_EXCEPTION(scope, void());

    ResolveLocaleOptions localeOptions;

    LocaleMatcher localeMatcher = intlOption<LocaleMatcher>(globalObject, options, vm.propertyNames->localeMatcher, { { "lookup"_s, LocaleMatcher::Lookup }, { "best fit"_s, LocaleMatcher::BestFit } }, "localeMatcher must be either \"lookup\" or \"best fit\""_s, LocaleMatcher::BestFit);
    RETURN_IF_EXCEPTION(scope, void());

    auto localeData = [](const String&, RelevantExtensionKey) -> Vector<String> {
        return { };
    };

    const auto& availableLocales = intlSegmenterAvailableLocales();
    auto resolved = resolveLocale(globalObject, availableLocales, requestedLocales, localeMatcher, localeOptions, { }, localeData);

    m_locale = resolved.locale;
    if (m_locale.isEmpty()) {
        throwTypeError(globalObject, scope, "failed to initialize Segmenter due to invalid locale"_s);
        return;
    }

    m_granularity = intlOption<Granularity>(globalObject, options, vm.propertyNames->granularity, { { "grapheme"_s, Granularity::Grapheme }, { "word"_s, Granularity::Word }, { "sentence"_s, Granularity::Sentence } }, "granularity must be either \"grapheme\", \"word\", or \"sentence\""_s, Granularity::Grapheme);
    RETURN_IF_EXCEPTION(scope, void());

    UBreakIteratorType type = UBRK_CHARACTER;
    switch (m_granularity) {
    case Granularity::Grapheme:
        type = UBRK_CHARACTER;
        break;
    case Granularity::Word:
        type = UBRK_WORD;
        break;
    case Granularity::Sentence:
        type = UBRK_SENTENCE;
        break;
    }

    UErrorCode status = U_ZERO_ERROR;
    m_segmenter = std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>(ubrk_open(type, m_locale.ascii().data(), nullptr, 0, &status));
    if (U_FAILURE(status)) {
        throwTypeError(globalObject, scope, "failed to initialize Segmenter"_s);
        return;
    }
#if USE(BUN_JSC_ADDITIONS)
    // The rules of the root: a locale with rules of its own is asked everything.
    if (m_granularity == Granularity::Grapheme && charactersBelowLimitJoinNothing()) {
        const char* actualLocale = ubrk_getLocaleByType(m_segmenter.get(), ULOC_ACTUAL_LOCALE, &status);
        m_cutsSimpleGraphemesItself = U_SUCCESS(status) && actualLocale && !strcmp(actualLocale, "root");
    }
#endif
}

#if USE(BUN_JSC_ADDITIONS)
std::unique_ptr<UBreakIterator, UBreakIteratorDeleter> IntlSegmenter::takeBreakIterator(UErrorCode& status, bool& isClone) const
{
    isClone = m_spareBreakIterators.isEmpty();
    if (!isClone)
        return m_spareBreakIterators.takeLast();
    return std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>(cloneUBreakIterator(m_segmenter.get(), &status));
}

void IntlSegmenter::giveBack(std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&& breakIterator) const
{
    if (m_spareBreakIterators.size() < m_spareBreakIterators.capacity())
        m_spareBreakIterators.append(WTF::move(breakIterator));
}
#endif

// https://tc39.es/proposal-intl-segmenter/#sec-intl.segmenter.prototype.segment
JSValue IntlSegmenter::segment(JSGlobalObject* globalObject, JSValue stringValue) const
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    JSString* jsString = stringValue.toString(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto string = jsString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
#if USE(BUN_JSC_ADDITIONS)
    // The characters are copied for ICU when it is asked: IntlSegments::ensureBuffer().
    {
        auto* segments = IntlSegments::create(vm, globalObject->segmentsStructure(), nullptr, { }, jsString, m_granularity);
        segments->setSegmenter(vm, const_cast<IntlSegmenter*>(this));
        return segments;
    }
#endif
    auto expectedCharacters = string->charactersWithoutNullTermination();
    if (!expectedCharacters) {
        throwOutOfMemoryError(globalObject, scope);
        return { };
    }

    auto upconvertedCharacters = Box<Vector<char16_t>>::create(expectedCharacters.value());

    UErrorCode status = U_ZERO_ERROR;
    auto segmenter = std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>(cloneUBreakIterator(m_segmenter.get(), &status));
    if (U_FAILURE(status)) {
        throwTypeError(globalObject, scope, "failed to initialize Segments"_s);
        return { };
    }
    ubrk_setText(segmenter.get(), upconvertedCharacters->span().data(), upconvertedCharacters->size(), &status);
    if (U_FAILURE(status)) {
        throwTypeError(globalObject, scope, "failed to initialize Segments"_s);
        return { };
    }

    return IntlSegments::create(vm, globalObject->segmentsStructure(), WTF::move(segmenter), WTF::move(upconvertedCharacters), jsString, m_granularity);
}

// https://tc39.es/proposal-intl-segmenter/#sec-intl.segmenter.prototype.resolvedoptions
JSObject* IntlSegmenter::resolvedOptions(JSGlobalObject* globalObject) const
{
    VM& vm = globalObject->vm();
    JSObject* options = constructEmptyObject(globalObject);
    options->putDirect(vm, vm.propertyNames->locale, jsString(vm, m_locale));
    options->putDirect(vm, vm.propertyNames->granularity, jsNontrivialString(vm, granularityString(m_granularity)));
    return options;
}

ASCIILiteral IntlSegmenter::granularityString(Granularity granularity)
{
    switch (granularity) {
    case Granularity::Grapheme:
        return "grapheme"_s;
    case Granularity::Word:
        return "word"_s;
    case Granularity::Sentence:
        return "sentence"_s;
    }
    ASSERT_NOT_REACHED();
    return { };
}

} // namespace JSC
