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

#pragma once

#include "IntlObject.h"
#include <unicode/ubrk.h>
#include <wtf/unicode/icu/ICUHelpers.h>

namespace JSC {

using UBreakIteratorDeleter = ICUDeleter<ubrk_close>;

class IntlSegmenter final : public JSNonFinalObject {
public:
    using Base = JSNonFinalObject;

    static constexpr DestructionMode needsDestruction = NeedsDestruction;

    static void destroy(JSCell* cell)
    {
        static_cast<IntlSegmenter*>(cell)->IntlSegmenter::~IntlSegmenter();
    }

    template<typename CellType, SubspaceAccess mode>
    static GCClient::IsoSubspace* subspaceFor(VM& vm)
    {
        return vm.intlSegmenterSpace<mode>();
    }

    enum class Granularity : uint8_t { Grapheme, Word, Sentence };
#if USE(BUN_JSC_ADDITIONS)
    // Of a clone, with its caches and its UText.
    static constexpr size_t estimatedSizeOfBreakIterator = 2 * KB;
    // For a text of its own. One that was given back, else a clone.
    std::unique_ptr<UBreakIterator, UBreakIteratorDeleter> takeBreakIterator(UErrorCode&, bool& isClone) const;
    void giveBack(std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>&&) const;
    // For whoever has to name one and does not look at it.
    UBreakIterator& breakIterator() const { return *m_segmenter; }
    // Between two code units below U+0300 one grapheme cluster ends and the next begins, whatever stands around them, but between CR and LF. What joins its neighbour begins at U+0300.
    static constexpr char16_t firstCharacterThatMayJoin = 0x300;
    // From there on the ICU that is loaded is asked for the property: Other or Control, and no surrogate.
    static bool joinsNoNeighbour(char16_t character) { return character < firstCharacterThatMayJoin || joinsNoNeighbourByProperty(character); }
    static bool areInDifferentGraphemeClustersForCertain(char16_t before, char16_t after) { return joinsNoNeighbour(before) && joinsNoNeighbour(after) && !(before == '\r' && after == '\n'); }
    // It goes by graphemes, and the ICU that is loaded has it so: see initializeSegmenter().
    bool cutsSimpleGraphemesItself() const { return m_cutsSimpleGraphemesItself; }
#endif

    static IntlSegmenter* create(VM&, Structure*);
    static Structure* createStructure(VM&, JSGlobalObject*, JSValue);

    DECLARE_INFO;

    void initializeSegmenter(JSGlobalObject*, JSValue localesValue, JSValue optionsValue);

    JSValue segment(JSGlobalObject*, JSValue) const;
    JSObject* resolvedOptions(JSGlobalObject*) const;

private:
    IntlSegmenter(VM&, Structure*);
    DECLARE_DEFAULT_FINISH_CREATION;

    static ASCIILiteral granularityString(Granularity);
#if USE(BUN_JSC_ADDITIONS)
    static bool joinsNoNeighbourByProperty(char16_t);
#endif

    std::unique_ptr<UBreakIterator, UBreakIteratorDeleter> m_segmenter;
#if USE(BUN_JSC_ADDITIONS)
    mutable Vector<std::unique_ptr<UBreakIterator, UBreakIteratorDeleter>, 2> m_spareBreakIterators;
    bool m_cutsSimpleGraphemesItself { false };
#endif
    String m_locale;
    Granularity m_granularity { Granularity::Grapheme };
};

} // namespace JSC
