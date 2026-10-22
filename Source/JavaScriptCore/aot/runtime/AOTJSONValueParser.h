/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONScanner.h"
#include "Identifier.h"
#include "JSArray.h"
#include "JSObject.h"
#include "MarkedVector.h"
#include <wtf/ForbidHeapAllocation.h>
#include <wtf/Noncopyable.h>
#include <wtf/UnalignedAccess.h>

namespace JSC { namespace AOT {

class JSONValueParser {
    WTF_FORBID_HEAP_ALLOCATION;
    WTF_MAKE_NONCOPYABLE(JSONValueParser);
public:
    JSONValueParser(Instance&, VM&, JSGlobalObject*);

    ParsedJSONValue parse(const uint8_t* start);
    ParsedJSONValue parseWholeSource(const uint8_t* start);

    struct ParsedName {
        Identifier name;
        const uint8_t* afterColon { nullptr };
    };
    ParsedName parseNameAndColon(const uint8_t* quote);

    class LearnedNames;

private:
    using Cursor = const uint8_t*;

    static constexpr size_t inlineElementCapacity = 32;

    struct Members {
        Structure* structure { nullptr };
        unsigned nameIndex { 0 };
        unsigned count { 0 };
        EncodedJSValue values[JSFinalObject::maxInlineCapacity];
    };

    struct NextMember {
        Structure* structure { nullptr };
        Cursor value { nullptr };
        unsigned nameIndex { 0 };
    };

    // The elements of an array mostly have the same members in the same order. How the source spells the names of the element in front, from the quote to the colon,
    // and what each leads to: names[i] goes on from the Structure that names[i - 1] has led to.
    struct NamesOfElementInFront {
        static constexpr unsigned capacity = 12;
        static constexpr unsigned maxTextLength = 2 * sizeof(uint64_t);

        // Nothing is read of a name at or behind `count`, and an array makes twelve of them: they are left as the stack has them.
        struct Name {
            uint64_t text[2];
            uint64_t mask[2];
            Structure* structure;
            unsigned nameIndex;
            unsigned textLength;
        };

        void note(const JSONWords&, unsigned index, Cursor quote, const NextMember&);
        bool continuesFrom(unsigned index, Structure* structure) const { return index <= count && (!index || names[index - 1].structure == structure); }

        unsigned count { 0 };
        bool isSharedBetweenSources { false };
        Name names[capacity];
    };

    ParsedJSONValue parseValue(Cursor);
    ParsedJSONValue parseString(Cursor quote);
    ParsedJSONValue parseNumber(Cursor);
    ParsedJSONValue parseObject(Cursor brace, NamesOfElementInFront* = nullptr);
    template<bool isShared> std::optional<ParsedJSONValue> parseMembersInFront(Members&, Cursor&, NamesOfElementInFront&);
    ParsedJSONValue parseArray(Cursor bracket);

    NextMember findKnownMember(const Members&, Cursor quote);
    NextMember findOrAddMember(const Members&, Cursor quote);
    enum class ObjectKind : bool { Ordinary, RecordIfPlanned };
    JSObject* createObject(Members&, ObjectKind = ObjectKind::RecordIfPlanned);
    ParsedJSONValue parseRemainingMembers(JSObject*, Cursor quote);
    JSArray* createArray(const JSValue* values, unsigned length);

    Instance& m_instance;
    const JSONWords& m_words;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    JSONCache& m_cache;
    JSONCache::ParsingScope m_parsingScope;
    LearnedNames& m_learnedNames;
    MarkedArgumentBufferWithSize<inlineElementCapacity> m_elements;
};

// The names of the objects parsed last outside arrays, by how their text begins.
class JSONValueParser::LearnedNames {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LearnedNames);
public:
    LearnedNames()
    {
        for (NamesOfElementInFront& entry : m_entries)
            entry.isSharedBetweenSources = true;
    }

    NamesOfElementInFront& at(Cursor firstName)
    {
        uint64_t prefix = WTF::unalignedLoad<uint64_t>(firstName);
        return m_entries[(prefix * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfEntries))];
    }

    void clear()
    {
        for (NamesOfElementInFront& entry : m_entries)
            entry.count = 0;
    }

private:
    static constexpr unsigned numberOfEntries = 16;

    NamesOfElementInFront m_entries[numberOfEntries];
};

ParsedJSONValue parseJSONValueAt(Instance&, VM&, JSGlobalObject*, const uint8_t* start);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
