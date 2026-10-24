/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTProgramTables.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include "JSString.h"
#include "RememberedSlot.h"
#include <array>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringView.h>

namespace JSC { namespace AOT {

struct Instance;
struct State;

class ProgramStrings {
    WTF_MAKE_NONCOPYABLE(ProgramStrings);
    WTF_MAKE_TZONE_ALLOCATED(ProgramStrings);
public:
    explicit ProgramStrings(State&);

    Instance& instance() const { return m_instance; }

    static String text(const Image& image, StringID string) { return image.text().createString(string); }
    static bool hasText(const Image& image, StringID string, StringView text) { return image.text().equals(string, text); }
    static AtomString atomOf(const Image& image, StringID string) { return AtomString(image.text().createAtom(string)); }
    static String text(const ProgramTables&, StringID string) { return text(*Image::current(), string); }
    static StringView viewInImage(const ProgramTables&, StringID string) { return Image::current()->text().view(string); }
    static StringID identifierString(const ProgramTables&, uint32_t identifier) { return Image::current()->text().identifier(identifier).text(); }

    // A constant is made once: its cell is kept, or its site runs at most once.
    enum class Made : bool { AnyNumberOfTimes, Once };
    JSString* string(VM&, StringID, Made = Made::AnyNumberOfTimes) const;
    JSString* constant(VM&, uint32_t number);
    JSString* constantIfCreated(uint32_t number) const;
    JSString* createConstant(VM&, uint32_t stringIndex);
    // The name as it is written, without "get ", "set " or "bound ".
    JSString* writtenNameOfFunction(VM&, FunctionID);
    // By the position of the name in ProgramTables::functionNames(). operationAOTNewStringConstant() takes such a position with this bit in place of the index of a string.
    static constexpr uint32_t isPositionOfFunctionName = 1u << 31;
    JSString* functionNameAt(VM&, uint32_t position);

    JSString* stringForLiteralStringWord(uint64_t) const;
    JSString* literalStringIfCreated(uint32_t number) const;
    uint64_t literalStringWordFor(JSString*);
    // The position in ProgramTables::literalStrings().
    std::optional<uint32_t> findLiteralString(JSString*);

    // The number is the position of the key's identifier in ProgramTables::keyCellIdentifiers().
    JSCell* keyCell(VM&, uint32_t number);
    JSCell* keyCellIfCreated(uint32_t number) const;
    JSValue bigIntConstant(JSGlobalObject*, uint32_t number);

    void willAnalyzeHeap(VM&);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    // A StringImpl over characters of the image, as the image has one for other texts: static, so nobody counts references to it.
    StringImpl* tryCreateHeader(StringView charactersInImage, unsigned hash) const;
    JSCell* immortalCellAt(const uint32_t* narrowCells, JSCell* const* wideCells, uint32_t number) const;
    void setImmortalCellAt(uint32_t* narrowCells, JSCell** wideCells, uint32_t number, JSCell*) const;
    std::optional<uint32_t> findLiteralString(unsigned hash, StringView) const;
    bool isTextOfLiteralStringWithoutText(uint32_t number, StringView) const;

    Instance& m_instance;
    State& m_state;
    const Image& m_image;
    const ProgramTables& m_tables;
    const bool m_usesWideReferences;
    RememberedSlotSet m_constantCells;
    std::array<RefPtr<StringImpl>, 1 << literalStringCacheIndexBits> m_literalStringCacheKeys;
};

// No rope is made flat, nothing is allocated in the heap and nothing is thrown.
template<typename Functor>
auto withCharactersOf(JSString* string, const Functor& functor)
{
    if (const StringImpl* impl = string->tryGetValueImpl())
        return functor(StringView { *impl });
    if (string->isSubstring()) {
        auto* rope = uncheckedDowncast<JSRopeString>(string);
        return functor(StringView { *rope->substringBase()->tryGetValueImpl() }.substring(rope->substringOffset(), rope->length()));
    }
    auto resolve = [&]<typename CharacterType>(Vector<CharacterType, 64> characters) {
        string->resolveToBuffer(characters.mutableSpan());
        return functor(StringView { characters.span() });
    };
    return string->is8Bit() ? resolve(Vector<Latin1Character, 64>(string->length())) : resolve(Vector<char16_t, 64>(string->length()));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
