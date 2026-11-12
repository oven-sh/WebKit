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

class ObjectBuilderPlan;

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
    struct LearnedString;
    class LearnedStrings;

    uint32_t numberOfRecordsMade() const { return m_numberOfRecordsMade; }
    uint32_t numberOfOrdinaryObjectsMade() const { return m_numberOfOrdinaryObjectsMade; }
    uint32_t numberOfTransitionsCached() const { return m_numberOfTransitionsCached; }

private:
    using Cursor = const uint8_t*;

    uint32_t m_numberOfRecordsMade { 0 };
    uint32_t m_numberOfOrdinaryObjectsMade { 0 };
    uint32_t m_numberOfTransitionsCached { 0 };

    static constexpr size_t inlineElementCapacity = 32;

    struct Members {
        EncodedJSValue values[JSFinalObject::maxInlineCapacity];
        uint8_t kinds[JSFinalObject::maxInlineCapacity];
        uint8_t kindsOfAll { 0 };
    };

    struct NextMember {
        Structure* structure { nullptr };
        Cursor value { nullptr };
        bool isNew { false };
    };

    ParsedJSONValue parseValue(Cursor);
    LearnedString* parseString(Cursor quote);
    LearnedString* unlearnedString(Cursor quote, const ParsedJSONString&);
    ParsedJSONValue parseNumber(Cursor);
    ParsedJSONValue parseObject(Cursor brace);
    ParsedJSONValue parseArray(Cursor bracket);

    struct LearnedName;
    NextMember findNextMember(Structure*, unsigned index, Cursor quote);
    NextMember findKnownMember(Structure*, unsigned index, Cursor quote);
    NextMember findOrAddMember(Structure*, unsigned index, Cursor quote);
    JSObject* createObject(Members&, unsigned count, Structure*, LearnedName&);
    JSObject* createObjectSlow(Members&, unsigned count, Structure*, LearnedName&);
    JSObject* createOrdinaryObject(Members&, unsigned count, Structure*);
    ParsedJSONValue parseRemainingMembers(JSObject*, Cursor quote);
    JSArray* createArray(const JSValue* values, unsigned length);

    Instance& m_instance;
    const JSONWords& m_words;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    JSONCache& m_cache;
    JSONCache::ParsingScope m_parsingScope;
    LearnedNames& m_learnedNames;
    LearnedStrings& m_learnedStrings;
    const Cursor m_sourceEnd;
    uint32_t m_sourceNumber;
    MarkedArgumentBufferWithSize<inlineElementCapacity> m_elements;
};

struct JSONValueParser::LearnedString {
    struct Key {
        static ALWAYS_INLINE Key of(std::span<const uint8_t> text)
        {
            size_t length = text.size();
            uint64_t head = WTF::unalignedLoad<uint64_t>(text.data());
            if (length >= sizeof(uint64_t))
                return { head, WTF::unalignedLoad<uint64_t>(text.data() + length - sizeof(uint64_t)), length };
            head = length ? head << (64 - 8 * length) : 0;
            return { head, head, length };
        }

        bool operator==(const Key&) const = default;

        uint64_t head;
        uint64_t tail;
        size_t length;
    };

    Key key;
    JSString* string;
    Cursor quote;
    uint32_t textLength;
    uint32_t sourceNumber;
    LearnedString* next;
};

class JSONValueParser::LearnedStrings {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LearnedStrings);
public:
    static constexpr size_t maxLength = 256;

    LearnedStrings() { m_unlearned.next = &m_unlearned; }

    LearnedString& unlearned() { return m_unlearned; }
    LearnedString& at(const LearnedString::Key& key) { return m_strings[((key.head * 0x9E3779B97F4A7C15ULL) ^ ((key.tail + key.length) * 0xC2B2AE3D27D4EB4FULL)) >> (64 - WTF::fastLog2(capacity))]; }
    void set(LearnedString& learned, const LearnedString::Key& key, JSString* string)
    {
        m_isEmpty = false;
        learned.key = key;
        learned.string = string;
        learned.textLength = key.length + 2;
        learned.next = &m_unlearned;
    }
    void clear()
    {
        if (!m_isEmpty)
            memset(static_cast<void*>(m_strings.data()), 0, sizeof(m_strings));
        m_isEmpty = true;
    }

private:
    static constexpr unsigned capacity = 1024;

    bool m_isEmpty { true };
    LearnedString m_unlearned { };
    std::array<LearnedString, capacity> m_strings;
};

struct JSONValueParser::LearnedName {
    static constexpr unsigned maxTextLength = 32;

    ALWAYS_INLINE bool matches(Cursor cursor) const
    {
        uint64_t difference = (WTF::unalignedLoad<uint64_t>(cursor) & mask[0]) ^ text[0];
        difference |= (WTF::unalignedLoad<uint64_t>(cursor + sizeof(uint64_t)) & mask[1]) ^ text[1];
        if (difference)
            return false;
        if (textLength <= sizeof(text)) [[likely]]
            return true;
        Cursor last = cursor + textLength - sizeof(tail);
        return !((WTF::unalignedLoad<uint64_t>(last) ^ tail[0]) | (WTF::unalignedLoad<uint64_t>(last + sizeof(uint64_t)) ^ tail[1]));
    }

    bool isLearned() const { return textLength; }

    uint64_t text[2];
    uint64_t mask[2];
    LearnedName* likelyNext;
    Structure* structure;
    uint32_t textLength;
    bool completesOrdinaryObject;
    LearnedString* string;
    const ObjectBuilderPlan* plan;
    Structure* recordStructure;
    LearnedName* firstNext;
    LearnedName* sibling;
    uint64_t tail[2];
};

class JSONValueParser::LearnedNames {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LearnedNames);
public:
    explicit LearnedNames(LearnedStrings& strings)
        : m_strings(strings)
    {
        LearnedName& name = unlearned();
        name.text[0] = 1;
        name.likelyNext = &name;
        name.string = &m_strings.unlearned();
    }

    LearnedName& unlearned() { return m_names[0]; }

    uint32_t epoch() const { return m_epoch; }
    ALWAYS_INLINE uint32_t beginSource(JSONSourceEncoding encoding)
    {
        if (!++m_sourceNumber || (m_hasNamesBeyondASCII && encoding != JSONSourceEncoding::Latin1)) [[unlikely]]
            forgetForNewSource();
        return m_sourceNumber;
    }

    ALWAYS_INLINE LearnedName& findFirst(Cursor quote)
    {
        LearnedName* name = m_first[firstIndex(quote)];
        if (!name) [[unlikely]]
            return unlearned();
        return name->matches(quote) ? *name : findOtherFirst(quote);
    }
    ALWAYS_INLINE LearnedName& findNext(LearnedName& name, Cursor quote)
    {
        LearnedName& next = *name.likelyNext;
        return next.matches(quote) ? next : findOtherNext(name, quote);
    }

    LearnedName& addFirst(const JSONWords&, Cursor quote, const NextMember&);
    LearnedName& addNext(const JSONWords&, LearnedName&, Cursor quote, const NextMember&);

    void clear();

private:
    static constexpr unsigned capacity = 1024;
    static constexpr unsigned numberOfFirstNames = 64;
    static constexpr unsigned maxSiblingsTried = 8;

    static unsigned firstIndex(Cursor quote) { return (WTF::unalignedLoad<uint32_t>(quote) * 0x9E3779B9U) >> (32 - WTF::fastLog2(numberOfFirstNames)); }

    void forgetForNewSource();
    LearnedName& findOtherFirst(Cursor quote);
    LearnedName& findOtherNext(LearnedName&, Cursor quote);
    LearnedName* add(const JSONWords&, Cursor quote, const NextMember&);

    LearnedStrings& m_strings;
    uint32_t m_epoch { 0 };
    uint32_t m_sourceNumber { 0 };
    uint32_t m_count { 1 };
    bool m_hasNamesBeyondASCII { false };
    std::array<LearnedName*, numberOfFirstNames> m_first { };
    std::array<LearnedName, capacity> m_names;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
