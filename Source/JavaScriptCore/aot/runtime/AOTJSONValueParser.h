/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCellFormat.h"
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
    struct RepeatedValue;
    class RepeatedValues;

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
        uint64_t objects { 0 };
    };

    struct NextMember {
        Structure* structure { nullptr };
        Cursor value { nullptr };
        bool isNew { false };
    };

    ParsedJSONValue parseValue(Cursor);
    LearnedString* parseString(Cursor quote);
    LearnedString* unlearnedString(Cursor quote, const ParsedJSONString&);
    bool isLearnedString(const LearnedString&, Cursor value) const;
    ParsedJSONValue parseNumber(Cursor);
    ParsedJSONValue parseNumberSlow(Cursor);
    ParsedJSONValue parseObject(Cursor brace);
    ParsedJSONValue parseArray(Cursor bracket, uint16_t& missedRepeats);
    ParsedJSONValue parseNested(Cursor bracket, uint16_t& missedRepeats);
    ParsedJSONValue parseOrCopy(Cursor bracket, uint16_t& missedRepeats);
    JSObject* copyOf(JSObject*);
    JSObject* copyOfObjectWithObjects(JSObject*);
    JSObject* copyOfOrdinaryObject(JSFinalObject*);
    JSObject* copyOfArray(JSArray*);

    struct LearnedName;
    NextMember findNextMember(Structure*, unsigned index, Cursor quote);
    NextMember findKnownMember(Structure*, unsigned index, Cursor quote);
    NextMember findOrAddMember(Structure*, unsigned index, Cursor quote);
    JSObject* createRecord(Members&, unsigned count, uint8_t kindsOfAll, LearnedName&);
    JSObject* createObject(Members&, unsigned count, uint8_t kindsOfAll, Structure*, LearnedName&);
    JSObject* createObjectSlow(Members&, unsigned count, uint8_t kindsOfAll, Structure*, LearnedName&);
    JSObject* createOrdinaryObject(Members&, unsigned count, Structure*);
    ParsedJSONValue parseRemainingMembers(JSObject*, Cursor quote);
    JSArray* createArray(const JSValue* values, unsigned length);
    JSArray* createArraySlow(const JSValue* values, unsigned length, Structure*);

    Instance& m_instance;
    const JSONWords& m_words;
    VM& m_vm;
    JSGlobalObject* m_globalObject;
    JSONCache& m_cache;
    JSONCache::ParsingScope m_parsingScope;
    LearnedNames& m_learnedNames;
    LearnedStrings& m_learnedStrings;
    uint16_t* const m_predictions;
    RepeatedValues& m_repeatedValues;
    const Cursor m_sourceEnd;
    void* const m_stackLimit;
    uint32_t m_sourceNumber;
    bool m_copiesRepeatedValues;
    MarkedArgumentBufferWithSize<inlineElementCapacity> m_elements;
};

struct alignas(64) JSONValueParser::LearnedString {
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

        uint64_t head;
        uint64_t tail;
        size_t length;
    };

    static constexpr unsigned headLength = 2 * sizeof(uint64_t);

    ALWAYS_INLINE bool matchesHead(Cursor value) const
    {
        uint64_t difference = (WTF::unalignedLoad<uint64_t>(value) | afterText[0]) ^ head[0];
        difference |= (WTF::unalignedLoad<uint64_t>(value + sizeof(uint64_t)) | afterText[1]) ^ head[1];
        return !difference;
    }

    uint64_t head[2];
    uint64_t afterText[2];
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
    static constexpr unsigned bitsOfIndex = 11;

    LearnedString& unlearned() { return m_strings[0]; }
    static uint64_t hashOf(const LearnedString::Key& key) { return (key.head * 0x9E3779B97F4A7C15ULL) ^ ((key.tail + key.length) * 0xC2B2AE3D27D4EB4FULL); }
    LearnedString& at(uint64_t hash) { return m_strings.data()[1 + placeOf(hash)]; }
    bool wasSeen(uint64_t hash)
    {
        uint16_t tag = hash >> 32;
        return std::exchange(m_tags.data()[placeOf(hash)], tag) == tag;
    }
    LearnedString& withIndex(unsigned index) { return m_strings.data()[index]; }
    unsigned indexOf(const LearnedString& string) const { return &string - m_strings.data(); }
    uint16_t* predictions(JSONSourceEncoding encoding) { return encoding == JSONSourceEncoding::Latin1 ? m_predictions.data() : m_noPredictions.data(); }
    static unsigned indexOfPrediction(const LearnedName&);
    void set(LearnedString&, Cursor quote, size_t textLength, JSString*);
    void clear()
    {
        if (!m_isEmpty)
            memset(static_cast<void*>(m_strings.data()), 0, sizeof(m_strings));
        m_isEmpty = true;
    }

private:
    static constexpr unsigned capacity = 1024;
    static_assert(capacity < 1 << bitsOfIndex);
    static constexpr unsigned numberOfPredictions = 2048;
    static size_t placeOf(uint64_t hash) { return hash >> (64 - WTF::fastLog2(capacity)); }

    bool m_isEmpty { true };
    std::array<uint16_t, numberOfPredictions> m_predictions;
    std::array<uint16_t, numberOfPredictions> m_noPredictions;
    std::array<uint16_t, capacity> m_tags;
    std::array<LearnedString, 1 + capacity> m_strings;
};

struct alignas(64) JSONValueParser::LearnedName {
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
    uint32_t recentStrings;
    bool completesOrdinaryObject;
    uint16_t missedRepeats;
    const ObjectBuilderPlan* plan;
    Structure* recordStructure;
    LearnedName* firstNext;
    LearnedName* sibling;
    uint64_t tail[2];
};

ALWAYS_INLINE unsigned JSONValueParser::LearnedStrings::indexOfPrediction(const LearnedName& name)
{
    uint64_t context = static_cast<uint64_t>(name.recentStrings) << 32 | static_cast<uint32_t>(std::bit_cast<uintptr_t>(&name));
    return (context * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfPredictions));
}

struct JSONValueParser::RepeatedValue {
    Cursor start;
    JSObject* object;
    uint32_t textLength;
    uint32_t sourceNumber;
};

class JSONValueParser::RepeatedValues {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(RepeatedValues);
public:
    static constexpr size_t minTextLength = 16;
    static constexpr uint16_t maxMissesInRow = 64;
    static constexpr uint16_t endOfSkippedRow = 4096;

    ALWAYS_INLINE RepeatedValue& at(Cursor bracket)
    {
        auto word = [&](unsigned index) ALWAYS_INLINE_LAMBDA {
            return WTF::unalignedLoad<uint64_t>(bracket + index * sizeof(uint64_t));
        };
        uint64_t hash = word(0) * 0x9E3779B97F4A7C15ULL ^ word(1) * 0xC2B2AE3D27D4EB4FULL ^ word(2) * 0x165667B19E3779F9ULL ^ word(3) * 0xD6E8FEB86659FD93ULL;
        return m_values.data()[hash >> (64 - WTF::fastLog2(capacity))];
    }
    void set(RepeatedValue& value, const RepeatedValue& newValue)
    {
        m_isEmpty = false;
        value = newValue;
    }

    struct PlanOfRecord {
        uint64_t key;
        const ObjectBuilderPlan* plan;
        uint64_t fieldsWithObjects;
    };
    static uint64_t keyOf(StructureID structureID, uint32_t orderAndCount) { return static_cast<uint64_t>(structureID.bits()) << 32 | orderAndCount; }
    static ALWAYS_INLINE uint64_t keyOf(const JSObject* object)
    {
        uint32_t orderWord = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(object) + offsetOfOrderWord);
        return keyOf(object->structureID(), orderWord & orderAndCountMask);
    }
    ALWAYS_INLINE PlanOfRecord* findPlan(uint64_t key)
    {
        PlanOfRecord* first = m_plans.data() + indexOf(key);
        for (unsigned distance = 0; distance < plansTried; ++distance) {
            if (first[distance].key == key) [[likely]]
                return first + distance;
        }
        return nullptr;
    }
    void addPlan(StructureID, const ObjectBuilderPlan&);
    void addFieldsWithObjects(StructureID, const ObjectBuilderPlan&, uint64_t fields);

    void clear()
    {
        if (!m_isEmpty) {
            m_values.fill({ });
            m_plans.fill({ });
        }
        m_isEmpty = true;
    }

private:
    static constexpr unsigned capacity = 512;
    static constexpr unsigned numberOfPlans = 512;
    static constexpr unsigned plansTried = 4;
    static size_t indexOf(uint64_t key) { return (key * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfPlans)); }

    bool m_isEmpty { true };
    std::array<PlanOfRecord, numberOfPlans + plansTried - 1> m_plans;
    std::array<RepeatedValue, capacity> m_values;
};

class JSONValueParser::LearnedNames {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(LearnedNames);
public:
    LearnedNames(LearnedStrings& strings, RepeatedValues& repeatedValues)
        : m_strings(strings)
        , m_repeatedValues(repeatedValues)
    {
        LearnedName& name = unlearned();
        name.text[0] = 1;
        name.likelyNext = &name;
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
    RepeatedValues& m_repeatedValues;
    uint32_t m_epoch { 0 };
    uint32_t m_sourceNumber { 0 };
    uint32_t m_count { 1 };
    bool m_hasNamesBeyondASCII { false };
    std::array<LearnedName*, numberOfFirstNames> m_first { };
    std::array<LearnedName, capacity> m_names;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
