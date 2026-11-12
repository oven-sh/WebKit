/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTImage.h"
#include "AOTProgramTables.h"
#include "JSString.h"
#include "RememberedSlot.h"
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

    static String text(const Image& image, StringID string) { return image.text().createString(string); }
    static AtomString atomOf(const Image& image, StringID string) { return AtomString(image.text().createAtom(string)); }
    static String text(const ProgramTables&, StringID string) { return text(*Image::current(), string); }
    static StringView viewInImage(const ProgramTables&, StringID string) { return Image::current()->text().view(string); }
    static StringID identifierString(const ProgramTables&, uint32_t identifierOrString)
    {
        const ImageText& text = Image::current()->text();
        return identifierOrString < text.numberOfIdentifiers() ? text.identifier(identifierOrString).text() : StringID(identifierOrString);
    }

    enum class Made : bool { AnyNumberOfTimes, Once };
    JSString* string(VM& vm, StringID string, Made made = Made::AnyNumberOfTimes) const { return this->string(vm, m_image.text().wordOf(string), made); }
    JSString* string(VM&, ImageStringWord, Made) const;
    JSString* constant(VM&, uint32_t number);
    JSString* constantIfCreated(uint32_t number) const;
    JSString* writtenNameOfFunction(VM&, FunctionID);

    JSCell* keyCell(VM&, uint32_t number);
    JSCell* keyCellIfCreated(uint32_t number) const;
    JSValue bigIntConstant(JSGlobalObject*, uint32_t number);

    void willAnalyzeHeap(VM&);
    template<typename Visitor> void visitRoots(Visitor&);

private:
    template<typename Appender> void visitRegion(Appender&, size_t begin, size_t end);
    StringImpl* tryCreateHeader(StringView charactersInImage, unsigned hash) const;

    Instance& m_instance;
    State& m_state;
    const Image& m_image;
    const ProgramTables& m_tables;
    const bool m_usesWideReferences;
    bool m_hasNamedVariablesForHeapSnapshots { false };
    RememberedSlotSet m_constantCells;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
