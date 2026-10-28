/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTJSONKeyOrder.h"
#include "AOTJSONSource.h"
#include "AOTJSONValueParser.h"
#include "AOTJSONWords.h"
#include "AOTSlotRepresentation.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include "Allocator.h"
#include "JSCJSValue.h"
#include "Structure.h"
#include <array>
#include <limits>
#include <memory>
#include <wtf/FastMalloc.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/UnalignedAccess.h>
#include <wtf/text/StringImpl.h>

namespace JSC { namespace AOT {

class CacheRegistry;
class CompiledStructure;
class ObjectBuilderPlan;
struct Instance;

class JSONRuntime {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(JSONRuntime);
    WTF_MAKE_NONCOPYABLE(JSONRuntime);
public:
    JSONRuntime(Instance&, CacheRegistry&);
    ~JSONRuntime();

    bool open(JSValue textOrBytes, void* stackPointer);
    bool open(StringView text, void* stackPointer);
    bool open(std::span<const uint8_t> utf8, void* stackPointer);
    bool close(const uint8_t* cursor);
    void abandon();
    void dropSourcesBelow(void* stackPointer);

    void clear();

    Instance& instance() const { return m_instance; }
    const StringImpl* wideText() const { return m_wideText.get(); }
    JSONKeyOrderLists& keyOrderLists() { return m_keyOrderLists; }
    size_t numberOfSuspendedSources() const { return m_suspendedSources.size(); }
    size_t sourceCapacity() const { return m_sourceCapacity; }

    struct RecordPlan {
        const ObjectBuilderPlan* plan { nullptr };
        Structure* structure { nullptr };
    };

    JSONValueParser::LearnedNames& learnedNames();

    // How an object with members that its type does not declare was finished: the next one that is born alike, with the same names in the same places, is finished the same way.
    struct FinishedObject {
        Structure* structureAtBirth { nullptr };
        Structure* extraStructure { nullptr };
        uint64_t order { 0 };
        uint32_t orderWordAtBirth { 0 };
        uint32_t orderAndCount { 0 };
        CompiledStructure* structure { nullptr };
    };
    FinishedObject& finishedObjectFor(Structure* structureAtBirth, Structure* extraStructure)
    {
        return m_finishedObjects[((structureAtBirth->id().bits() ^ extraStructure->id().bits()) >> 4) % m_finishedObjects.size()];
    }

    struct LearnedName {
        static constexpr unsigned wordsOfText = 4;
        static constexpr size_t maxTextLength = wordsOfText * sizeof(uint64_t);

        ALWAYS_INLINE bool standsAt(const uint8_t* quote) const
        {
            uint64_t difference = ((WTF::unalignedLoad<uint64_t>(quote) & mask[0]) ^ text[0]) | ((WTF::unalignedLoad<uint64_t>(quote + sizeof(uint64_t)) & mask[1]) ^ text[1]);
            if (textLength > 2 * sizeof(uint64_t)) [[unlikely]]
                difference |= ((WTF::unalignedLoad<uint64_t>(quote + 2 * sizeof(uint64_t)) & mask[2]) ^ text[2]) | ((WTF::unalignedLoad<uint64_t>(quote + 3 * sizeof(uint64_t)) & mask[3]) ^ text[3]);
            return !difference;
        }
        bool trySetText(const uint8_t* quote, size_t length);
        void setTextThatStandsNowhere();

        uint64_t text[wordsOfText];
        uint64_t mask[wordsOfText];
        uint16_t textLength;
    };
    static_assert(LearnedName::maxTextLength <= jsonSourcePadding);

    struct LearnedExtraMembers {
        static constexpr unsigned capacity = 8;
        static constexpr uint64_t noKey = std::numeric_limits<uint64_t>::max();

        struct Name : LearnedName {
            uint16_t outOfLineCapacity;
            PropertyOffset offset;
            Structure* structure;
        };

        static uint64_t keyFor(uint32_t shape, StructureID structureBefore) { return shape | static_cast<uint64_t>(structureBefore.bits()) << 32; }
        void begin(uint64_t keyOfMembers, unsigned outOfLineCapacityInFront);
        void note(unsigned index, const uint8_t* quote, size_t textLength, Structure*);

        bool holdsNoNamesOf(uint64_t keyOfMembers, uint64_t firstWordOfText) const { return !count && key == keyOfMembers && firstWord == firstWordOfText; }

        uint64_t key { noKey };
        uint64_t firstWord { 0 };
        uint64_t orderBefore { JSONKeyOrder::unknown().word() };
        uint64_t orderAfter { JSONKeyOrder::unknown().word() };
        uint16_t positionedOrdinal { 0 };
        uint16_t outOfLineCapacityBefore { 0 };
        uint8_t count { 0 };
        bool wasFound { false };
        bool isOnlySeen { false };
        Name names[capacity];
    };
    ALWAYS_INLINE LearnedExtraMembers* findLearnedExtraMembers(uint64_t key, const uint8_t* firstName)
    {
        if (!m_learnedExtraMembers) [[unlikely]]
            return nullptr;
        LearnedExtraMembers* learned = &(*m_learnedExtraMembers)[indexOfLearnedExtraMembers(key, firstName)];
        if (learned->key == key && learned->names[0].standsAt(firstName)) [[likely]]
            return learned;
        learned = &(*m_learnedExtraMembers)[indexOfLearnedExtraMembers(key, firstName) ^ 1];
        return learned->key == key && learned->names[0].standsAt(firstName) ? learned : nullptr;
    }
    LearnedExtraMembers* learnedExtraMembersWithoutNames(uint64_t key, const uint8_t* firstName);
    LearnedExtraMembers& learnedExtraMembersToReplace(uint64_t key, const uint8_t* firstName);

    struct LearnedObject {
        AOT_CLASS_THRESHOLD(unsigned, maxNumberOfFields, 32);
        AOT_CLASS_THRESHOLD(unsigned, maxNumberOfExtraMembers, 64);
        AOT_CLASS_THRESHOLD(size_t, maxMembersParsedTwiceForEachReturnSaved, 3);

        enum Kind : uint8_t {
            SmallInteger = 1 << 0,
            String = 1 << 1,
            Boolean = 1 << 2,
            OtherKind = 1 << 3,
        };

        struct Member : LearnedName {
            uint8_t indexOfValue;
            uint32_t shapeOfValue;
        };

        enum class Store : uint8_t { Word, Cell32, Int32, Int32AsDouble, Boolean, LowByte };
        struct FieldStore {
            uint16_t offset;
            Store store;
            uint8_t indexOfValue;
            Kind kind;
        };

        bool isFor(uint32_t shapeOfObject, uint64_t firstWordOfText) const { return shape == shapeOfObject && firstWord == firstWordOfText; }
        bool holdsMembersOf(uint32_t shapeOfObject) const { return shape == shapeOfObject && !members.isEmpty(); }
        bool isRefused() const { return members.isEmpty() && !isOnlySeen; }
        void forgetCells();

        uint32_t shape { 0 };
        uint32_t orderWord { 0 };
        uint64_t firstWord { 0 };
        Structure* structureAtBirth { nullptr };
        Structure* extraStructure { nullptr };
        CompiledStructure* structure { nullptr };
        SlotData extraPropertiesSlot { };
        Allocator allocatorOfExtraProperties;
        std::unique_ptr<ObjectBuilderPlan> plan;
        Vector<Member> members;
        Vector<uint16_t> keys;
        Vector<FieldStore> stores;
        Vector<uint64_t> wordsBehindHeader;
        Allocator allocator;
        size_t allocationSize { 0 };
        bool hasPerCellBit { false };
        uint8_t numberOfExtraMembers { 0 };
        bool wasFound { false };
        bool isOnlySeen { false };
        bool isBeingLearned { false };
    };
    static constexpr unsigned numberOfLearnedObjects = 32;
    using LearnedObjects = std::array<LearnedObject, numberOfLearnedObjects>;
    LearnedObjects* learnedObjects() { return m_learnedObjects.get(); }
    static ALWAYS_INLINE uint64_t startOfName(const uint8_t* quote)
    {
        uint64_t bytes = WTF::unalignedLoad<uint64_t>(quote) >> 8;
        uint64_t zeroAtQuotes = bytes ^ 0x22222222222222ULL;
        uint64_t quotes = (zeroAtQuotes - 0x01010101010101ULL) & ~zeroAtQuotes & 0x80808080808080ULL;
        return quotes ? bytes & ((quotes & -quotes) - 1) : bytes;
    }
    static unsigned indexOfLearnedObject(uint32_t shape, uint64_t firstWord) { return ((firstWord ^ shape) * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfLearnedObjects)); }
    LearnedObject* learnedObjectToReplace(uint32_t shape, uint64_t firstWord);
    bool mayLearnAnotherObject();
    bool mayFinishAnotherObjectUnremembered();
    LearnedObject* learnedObjectFoundLast() const { return m_learnedObjectFoundLast; }
    void setLearnedObjectFoundLast(LearnedObject* learned) { m_learnedObjectFoundLast = learned; }
    void didSeeMember(uint32_t shape, uint16_t field, uint32_t shapeOfValue) { m_shapesOfMembers.set(static_cast<uint64_t>(shape) << 16 | field, shapeOfValue); }
    uint32_t shapeOfMember(uint32_t shape, uint16_t field) const;
    void didSeeExtraMembers(uint32_t shape);
    ALWAYS_INLINE bool remembersTextsOf(uint32_t shape) const { return m_timesNotFoundInARow[shape % numberOfJSONShapeBits] != maxTimesNotFoundInARow; }
    ALWAYS_INLINE void didFindTextOf(uint32_t shape) { m_timesNotFoundInARow[shape % numberOfJSONShapeBits] = 0; }
    ALWAYS_INLINE void didFindWholeTextOf(uint32_t shape)
    {
        didFindTextOf(shape);
        m_timesRefusedInARow[shape % numberOfJSONShapeBits] = 0;
    }
    void didRefuseTextOf(uint32_t shape);
    void didNotFindTextOf(uint32_t shape);

    bool hasParsedRecords() const { return m_hasParsedRecords; }
    ALWAYS_INLINE RecordPlan recordPlanFor(VM& vm, Structure* ordinaryStructure)
    {
        uint32_t id = ordinaryStructure->id().bits();
        if (m_recordPlanCache) [[likely]] {
            const CachedRecordPlan& cached = (*m_recordPlanCache)[recordPlanCacheIndex(id)];
            if (cached.ordinaryStructureID == id) [[likely]]
                return cached.recordPlan;
        }
        return findRecordPlan(vm, ordinaryStructure);
    }

private:
    struct CachedRecordPlan {
        uint32_t ordinaryStructureID { 0 };
        RecordPlan recordPlan;
    };
    struct RecordPlanForNames {
        Vector<RefPtr<UniquedStringImpl>, 8> names;
        std::unique_ptr<ObjectBuilderPlan> plan;
    };
    static constexpr size_t recordPlanCacheSize = 128;
    AOT_CLASS_THRESHOLD(size_t, maxRecordPlans, 256);
    using RecordPlanCache = std::array<CachedRecordPlan, recordPlanCacheSize>;
    static constexpr unsigned numberOfLearnedExtraMembers = 64;
    static unsigned indexOfLearnedExtraMembers(uint64_t key, const uint8_t* firstName)
    {
        return ((startOfName(firstName) ^ key) * 0x9E3779B97F4A7C15ULL) >> (64 - WTF::fastLog2(numberOfLearnedExtraMembers));
    }
    using LearnedExtraMembersTable = std::array<LearnedExtraMembers, numberOfLearnedExtraMembers>;
    AOT_CLASS_THRESHOLD(unsigned, maxObjectsLearnedBetweenCollections, 64);
    AOT_CLASS_THRESHOLD(unsigned, maxObjectsFinishedUnrememberedBetweenCollections, 16);
    AOT_CLASS_THRESHOLD(uint8_t, maxTimesNotFoundInARow, 255);
    AOT_CLASS_THRESHOLD(uint8_t, maxTimesRefusedInARow, 4);
    static constexpr size_t recordPlanCacheIndex(uint32_t structureID) { return (structureID >> 4) % recordPlanCacheSize; }

    RecordPlan findRecordPlan(VM&, Structure*);
    const ObjectBuilderPlan* findOrCreateRecordPlan(VM&, Structure*);
    std::unique_ptr<ObjectBuilderPlan> createRecordPlan(VM&, std::span<UniquedStringImpl* const> names);

    uint8_t* begin(void* stackPointer, size_t length);
    uint8_t* beginBesideOpenSource(void* stackPointer, size_t length);
    uint8_t* beginIn(uint8_t* start, void* stackPointer, size_t length, bool isNested);
    void end();
    void suspend();
    void resume();
    uint8_t* reserve(size_t size);
    uint8_t* replace(size_t size);
    void freeCurrent();

    bool openLatin1(std::span<const Latin1Character>, void* stackPointer);
    bool open16Bit(std::span<const char16_t>, void* stackPointer);

    Instance& m_instance;
    bool m_hasParsedRecords { false };
    std::unique_ptr<RecordPlanCache> m_recordPlanCache;
    std::unique_ptr<JSONValueParser::LearnedNames> m_learnedNames;
    std::unique_ptr<LearnedExtraMembersTable> m_learnedExtraMembers;
    std::unique_ptr<LearnedObjects> m_learnedObjects;
    unsigned m_numberOfObjectsLearned { 0 };
    unsigned m_numberOfObjectsFinishedUnremembered { 0 };
    LearnedObject* m_learnedObjectFoundLast { nullptr };
    UncheckedKeyHashMap<uint64_t, uint32_t> m_shapesOfMembers;
    std::array<uint8_t, numberOfJSONShapeBits> m_timesNotFoundInARow { };
    std::array<uint8_t, numberOfJSONShapeBits> m_timesRefusedInARow { };
    Vector<RecordPlanForNames> m_recordPlans;
    Vector<JSONSource, 2> m_suspendedSources;
    RefPtr<StringImpl> m_wideText;
    size_t m_sourceCapacity { 0 };
    JSONKeyOrderLists m_keyOrderLists;
    std::array<FinishedObject, 16> m_finishedObjects;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
