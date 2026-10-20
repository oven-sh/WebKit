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

    JSString* stringForLiteralStringWord(uint64_t) const;
    uint64_t literalStringWordFor(JSString*);

    JSCell* keyCell(VM&, uint32_t identifier);
    JSValue bigIntConstant(JSGlobalObject*, uint32_t number);

    void willAnalyzeHeap(VM&);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    JSString* createString(VM&, StringID) const;
    void setConstant(uint32_t number, JSString*);
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
