/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTProgramTables.h"
#include "AOTSlotRepresentation.h"
#include "RememberedSlot.h"
#include <array>
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/text/AtomString.h>
#include <wtf/text/StringView.h>

namespace JSC {

class JSCell;
class JSGlobalObject;
class JSString;
class VM;

namespace AOT {

class Image;
struct Instance;
struct State;

class ProgramStrings {
    WTF_MAKE_NONCOPYABLE(ProgramStrings);
    WTF_MAKE_TZONE_ALLOCATED(ProgramStrings);
public:
    explicit ProgramStrings(State&);

    Instance& instance() const { return m_instance; }

    static String text(const Image&, StringID);
    static bool hasText(const Image&, StringID, StringView);
    static AtomString atomOf(const Image&, StringID);
    static String text(const ProgramTables&, StringID);
    static StringView viewInImage(const ProgramTables&, StringID);
    static StringID identifierString(const ProgramTables&, uint32_t identifier);

    JSString* constant(VM&, uint32_t number);
    JSString* constantIfCreated(uint32_t number) const;
    JSString* createConstant(VM&, uint32_t stringIndex);
    JSString* string(VM&, StringID);
    // The name as it is written, without "get ", "set " or "bound ".
    JSString* writtenNameOfFunction(VM&, FunctionID);
    // By the position of the name in ProgramTables::functionNames(). operationAOTNewStringConstant() takes such a position with this bit in place of the index of a string.
    static constexpr uint32_t isPositionOfFunctionName = 1u << 31;
    JSString* functionNameAt(VM&, uint32_t position);

    JSString* stringForLiteralStringWord(uint64_t) const;
    JSString* literalStringIfCreated(uint32_t number) const;
    uint64_t literalStringWordFor(JSString*);

    // The number is the position of the key's identifier in ProgramTables::keyCellIdentifiers().
    JSCell* keyCell(VM&, uint32_t number);
    JSCell* keyCellIfCreated(uint32_t number) const;
    JSValue bigIntConstant(JSGlobalObject*, uint32_t number);

    void willAnalyzeHeap(VM&);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    // A constant is made once: its cell is kept, or its site runs at most once.
    enum class Made : bool { AnyNumberOfTimes, Once };
    JSString* createString(VM&, StringID, Made = Made::AnyNumberOfTimes) const;
    // A StringImpl over characters of the image, as the image has one for other texts: static, so nobody counts references to it.
    // Such a string may outlive the instance in the hands of native code, so the memory is never given back, and only the first instance of the process has any: in a Worker the string counts its references.
    StringImpl* tryCreateHeader(StringView charactersInImage, unsigned hash) const;
    bool isHeaderOfConstant(const StringImpl&) const;
    void setConstant(uint32_t number, JSString*);
    JSCell* immortalCellAt(const uint32_t* narrowCells, JSCell* const* wideCells, uint32_t number) const;
    void setImmortalCellAt(uint32_t* narrowCells, JSCell** wideCells, uint32_t number, JSCell*) const;
    std::optional<uint32_t> findLiteralString(JSString*);
    std::optional<uint32_t> findLiteralString(unsigned hash, StringView) const;

    Instance& m_instance;
    State& m_state;
    const Image& m_image;
    const ProgramTables& m_tables;
    const bool m_usesWideReferences;
    RememberedSlotSet m_constantCells;
    std::array<RefPtr<StringImpl>, 1 << literalStringCacheIndexBits> m_literalStringCacheKeys;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
