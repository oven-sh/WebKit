/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBitVector.h"
#include "AOTBody.h"
#include "AOTEffects.h"
#include "AOTFamilies.h"
#include "AOTLargeList.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTOpcode.h"
#include "AOTThreads.h"
#include "AOTType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OverflowHandler.h>
#include <wtf/ScopedLambda.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class Diagnostics;
class ParallelLoops;

namespace Typed {

class Program;

inline constexpr uint32_t noSummaryNode = UINT32_MAX;

class SlotOfContents {
public:
    constexpr SlotOfContents() = default;
    constexpr SlotOfContents(ContentsSlot slot)
        : m_number(static_cast<uint32_t>(slot))
    {
    }

    static constexpr SlotOfContents underUnknownNames() { return behindEnumerators(UnderUnknownNames); }
    static constexpr SlotOfContents underAnyName() { return behindEnumerators(UnderAnyName); }
    static constexpr SlotOfContents returnedAlone() { return behindEnumerators(ReturnedAlone); }
    static constexpr SlotOfContents noLongerAsMade() { return behindEnumerators(NoLongerAsMade); }
    static constexpr SlotOfContents mayHaveNumberNames() { return behindEnumerators(MayHaveNumberNames); }
    static constexpr SlotOfContents mayBeOfClassBelow() { return behindEnumerators(MayBeOfClassBelow); }
    static constexpr SlotOfContents mayBeShorter() { return behindEnumerators(MayBeShorter); }
    static constexpr SlotOfContents isChanged() { return behindEnumerators(IsChanged); }
    static constexpr SlotOfContents mayHaveHoles() { return behindEnumerators(MayHaveHoles); }
    static constexpr SlotOfContents mayLoseElements() { return behindEnumerators(MayLoseElements); }
    static constexpr SlotOfContents mayHaveNamesOfEngine() { return behindEnumerators(MayHaveNamesOfEngine); }
    static constexpr SlotOfContents hasSeveralRecordLayouts() { return behindEnumerators(HasSeveralRecordLayouts); }
    static constexpr bool hasSlotAt(uint32_t place) { return place < nameBit - firstWrittenPlace; }
    static constexpr SlotOfContents at(uint32_t place) { return fromNumber(firstWrittenPlace + place); }
    static constexpr SlotOfContents internalField(uint32_t index) { return fromNumber(firstInternalField + index); }
    static constexpr SlotOfContents stateOf(BuiltinSlotID slot) { return fromNumber(firstStateSlot + slot.index()); }
    static constexpr SlotOfContents under(PropertyKeyID key) { return fromNumber(nameBit | key.index()); }
    static constexpr SlotOfContents fromNumber(uint32_t number)
    {
        SlotOfContents slot;
        slot.m_number = number;
        return slot;
    }

    constexpr uint32_t number() const { return m_number; }
    constexpr explicit operator bool() const { return m_number != UINT32_MAX; }
    constexpr bool isUnderName() const { return !!*this && (m_number & nameBit); }
    constexpr PropertyKeyID key() const { return PropertyKeyID(m_number & ~nameBit); }
    constexpr std::optional<ContentsSlot> contentsSlot() const { return m_number < numberOfEnumerators<ContentsSlot> ? std::optional(static_cast<ContentsSlot>(m_number)) : std::nullopt; }
    constexpr std::optional<BuiltinSlotID> stateSlot() const { return m_number >= firstStateSlot && m_number < firstWrittenPlace ? std::optional(BuiltinSlotID(m_number - firstStateSlot)) : std::nullopt; }
    constexpr std::optional<uint32_t> indexOfInternalField() const { return m_number >= firstInternalField && m_number < firstStateSlot ? std::optional(m_number - firstInternalField) : std::nullopt; }

    constexpr std::optional<uint32_t> writtenPlace() const { return m_number >= firstWrittenPlace && m_number < nameBit ? std::optional(m_number - firstWrittenPlace) : std::nullopt; }

    friend constexpr bool operator==(SlotOfContents, SlotOfContents) = default;

private:
    // The slots that no enumerator of ContentsSlot names are numbered behind the enumerators, in this order. Whoever adds one adds it here, so the count below knows of it.
    enum SlotBehindEnumerators : uint32_t { UnderUnknownNames, UnderAnyName, ReturnedAlone, NoLongerAsMade, MayBeOfClassBelow, MayBeShorter, IsChanged, MayHaveHoles, MayLoseElements, MayHaveNamesOfEngine, HasSeveralRecordLayouts, MayHaveNumberNames, numberOfSlotsBehindEnumerators };
    static constexpr SlotOfContents behindEnumerators(SlotBehindEnumerators slot) { return fromNumber(numberOfEnumerators<ContentsSlot> + slot); }
    static constexpr uint32_t firstInternalField = 16;
    static_assert(numberOfEnumerators<ContentsSlot> + numberOfSlotsBehindEnumerators <= firstInternalField);
    static constexpr uint32_t firstStateSlot = 1u << 16;
    static constexpr uint32_t firstWrittenPlace = 1u << 30;
    static constexpr uint32_t nameBit = 1u << 31;

    uint32_t m_number { UINT32_MAX };
};

struct SummaryNode {
    enum class Kind : uint8_t {
        Unknown,

        Parameter,
        Result,
        Thrown,
        Capture,
        Variable,
        Field,
        Member,
        Contents,

        Constant,
        Allocation,
        ClassInstance,
        Closure,
        Builtin,

        Join,
        ResultOfCall,
        ResultOfAccess,
        ResultOfOperator,
        ResultOfConversion,
    };

    Kind kind { Kind::Unknown };
    uint32_t first { 0 };
    uint32_t second { 0 };

    friend constexpr bool operator==(const SummaryNode&, const SummaryNode&) = default;
};

class WrittenUnit {
public:
    enum class Kind : uint8_t { None, Text, Integer, Boolean };

    constexpr WrittenUnit() = default;
    static constexpr WrittenUnit text(StringID text) { return text.index() < payloadLimit ? WrittenUnit(Kind::Text, text.index()) : WrittenUnit(); }
    static constexpr WrittenUnit integer(int64_t value) { return value >= -static_cast<int64_t>(payloadLimit / 2) && value < static_cast<int64_t>(payloadLimit / 2) ? WrittenUnit(Kind::Integer, static_cast<uint32_t>(value + payloadLimit / 2)) : WrittenUnit(); }
    static constexpr WrittenUnit boolean(bool value) { return WrittenUnit(Kind::Boolean, value); }
    static constexpr WrittenUnit fromBits(uint32_t bits)
    {
        WrittenUnit unit;
        if (bits >> payloadBits)
            unit.m_bits = bits;
        return unit;
    }
    static constexpr uint32_t literalString = 1;

    constexpr uint32_t bits() const { return m_bits; }
    constexpr explicit operator bool() const { return !!m_bits; }
    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> 30); }
    constexpr StringID text() const { return StringID(m_bits & (payloadLimit - 1)); }
    constexpr int64_t integer() const { return static_cast<int64_t>(m_bits & (payloadLimit - 1)) - payloadLimit / 2; }
    constexpr bool boolean() const { return m_bits & 1; }

    friend constexpr bool operator==(WrittenUnit, WrittenUnit) = default;

private:
    static constexpr unsigned payloadBits = 30;
    static constexpr uint32_t payloadLimit = 1u << payloadBits;

    constexpr WrittenUnit(Kind kind, uint32_t payload)
        : m_bits(static_cast<uint32_t>(kind) << 30 | payload)
    {
    }

    uint32_t m_bits { 0 };
};

struct SummaryEdge {
    SummaryEdge() = default;
    SummaryEdge(uint32_t from, uint32_t to, TypeID passes, bool runsCodeOfValue, bool isConversionByEquality)
        : from(from)
        , runsCodeOfValue(runsCodeOfValue)
        , isConversionByEquality(isConversionByEquality)
        , to(to)
        , passes(passes)
    {
        RELEASE_ASSERT(this->from == from);
    }

    uint32_t from : 30 { 0 };
    uint32_t runsCodeOfValue : 1 { false };
    uint32_t isConversionByEquality : 1 { false };
    uint32_t to { 0 };
    TypeID passes;
};
static_assert(sizeof(SummaryEdge) == 12);

struct SummaryCall {
    enum class Callee : uint8_t {
        Binding,
        Name,
        Value,
        Builtin,
    };

    ValueID site;
    Callee callee { Callee::Value };
    bool isConstruction : 1 { false };
    bool isInLoop : 1 { false };
    bool isInTailPosition : 1 { false };
    bool passesList : 1 { false };
    bool isMadeByProtocol : 1 { false };
    bool resultIsReadAsKey : 1 { false };
    bool liesOnNoWayToReturn : 1 { false };
    bool passesOnGatheredArguments : 1 { false };
    bool appliesGatheredArguments : 1 { false };
    bool sharesArgumentsObject : 1 { false };
    bool expectsCalleeItself : 1 { false };
    bool isMadeByHost : 1 { false };
    bool passesLiteral : 1 { false };
    bool continuesConstruction : 1 { false };
    bool describesPrototypeInSight : 1 { false };
    bool passesWrittenParts : 1 { false };
    uint16_t lengthOfWrittenList : 13 { 0 };
    FunctionID function;
    BuiltinID builtin;
    BuiltinID expectedBuiltin;
    PropertyKeyID key;
    uint32_t subject { noSummaryNode };
    uint32_t receiver { noSummaryNode };
    uint32_t newTarget { noSummaryNode };
    uint32_t waitsFor { noSummaryNode };
    uint32_t thrownTo { noSummaryNode };
    uint32_t firstArgument { 0 };
    uint32_t numberOfArguments { 0 };
};

struct SummaryAccess {
    enum class Kind : uint8_t {
        Read,
        Store,
        Define,
        Has,
    };

    ValueID site;
    Kind kind { Kind::Read };
    bool isPartOfConstruction : 1 { false };
    bool subjectIsAnsweredBySuperCall : 1 { false };
    bool storesObjectMadeForIt : 1 { false };
    bool storesConstructedObject : 1 { false };
    bool standsForObjectMadeAtSite : 1 { false };
    bool liesOnNoWayToReturn : 1 { false };
    bool keyIsSurelyIndex : 1 { false };
    bool storesSurelyIndex : 1 { false };
    bool storesZero : 1 { false };
    bool standsRightBehindCall : 1 { false };
    bool answerIsOnlyCalled : 1 { false };
    bool laysAccessorAsWritten : 1 { false };
    bool isByDescriptor : 1 { false };
    bool holdsNewValueOfNativeCode : 1 { false };
    bool laysTextNumberOrBooleanThatIsWritten : 1 { false };
    bool subjectIsMadeInBody : 1 { false };
    bool isSpelledByCopy : 1 { false };
    bool storesExport : 1 { false };
    bool keyIsNameThatLoopOverSubjectGave : 1 { false };
    bool isCopySpelledByLoop : 1 { false };
    bool readsPrototypeOfHeritage : 1 { false };
    bool isByMethodDecorator : 1 { false };
    std::optional<OptionSet<FieldForm>> formCreated;
    uint32_t subject { 0 };
    uint32_t receiver { noSummaryNode };
    PropertyKeyID key;
    uint32_t keyNode { noSummaryNode };

    uint32_t value { noSummaryNode };
    SlotOfContents slot;
    uint32_t emptyHolderOrKeyPrefix { noSummaryNode };
    uint32_t thrownTo { noSummaryNode };
    union {
        uint32_t descriptorFromElsewhere { noSummaryNode };
        uint32_t writtenIndexOfRead;
    };


    bool isToContents() const { return !!slot; }
    uint32_t whatHoldsNothing() const { return isToContents() ? emptyHolderOrKeyPrefix : noSummaryNode; }
    StringID beginningOfKey() const { return isToContents() ? StringID() : StringID::fromBits(emptyHolderOrKeyPrefix); }
    bool laysValueInSightUnderName() const { return !isToContents() && (kind == Kind::Store || (kind == Kind::Define && value && value != noSummaryNode)); }
};

static_assert(sizeof(SummaryAccess) == 48 && StringID().bits() == noSummaryNode);

struct SummaryWrittenAccessor {
    uint32_t access { 0 };
    FunctionID getter;
    FunctionID setter;
    LayoutID map;
};

struct SummaryOperator {
    static constexpr uint32_t mostOperands = 2;

    ValueID site;
    Opcode opcode { Opcode::Add };
    uint32_t result { 0 };
    uint32_t operands[mostOperands] { noSummaryNode, noSummaryNode };
    uint32_t converted[mostOperands] { noSummaryNode, noSummaryNode };
    enum class WrittenDivisor : uint8_t { None, FromOneBelowTwoTo31, FromTwoTo31ToTwoTo32 };
    WrittenDivisor writtenDivisor { WrittenDivisor::None };
    bool shiftsRightByConstant { false };
    int8_t step { 0 };
    uint8_t steppedOperand { 0 };

    bool isConversionByEquality() const { return opcode == Opcode::Eq; }
};

struct SummaryParameterTest {
    uint32_t parameter { 0 };
    TypeID tested;
    bool holds { false };
};

struct SummaryReturn {
    uint32_t node { 0 };
    uint32_t firstTest { 0 };
    uint32_t numberOfTests { 0 };
};

struct SummaryMember {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MethodKind kind { MethodKind::Method };
    bool isRedefined { false };
    uint32_t index { 0 };
    PropertyKeyID key;
    uint32_t value { 0 };
};

bool canHoldAccessors(const Program&, LayoutID);

struct SummaryChange {
    enum class Kind : uint8_t {
        Freezes,
        Deletes,
        DefinesByDescriptor,
        ChangesPrototype,
        BecomesPrototype,
        CopiesInto,
    };

    ValueID site;
    Kind kind { Kind::Freezes };
    uint32_t subject { 0 };
    PropertyKeyID key;
    uint32_t keyNode { noSummaryNode };
    uint32_t other { noSummaryNode };
    enum class Lays : uint8_t { WhatDescriptorSays, Getter, Setter };
    Lays lays { Lays::WhatDescriptorSays };
    uint32_t getter { noSummaryNode };
    bool liesOnNoWayToReturn { false };
    bool heirIsMadeByCall { false };
    bool definesWhatItCopies { false };
    bool isSpelledByLoop { false };
    bool subjectIsMadeInBody { false };
    bool isUnderNameThatNobodyElseNames { false };
    bool descriptorIsInSight { false };
    bool descriptorLaysNoAccessor { false };
    bool isEntryWrittenInMap { false };
    bool laysAccessorAsWritten { false };
    bool laysNewValueOfNativeCode { false };
    bool laysTextNumberOrBooleanThatIsWritten { false };
    bool definesValueWithAttributes { false };
    bool getterLeavesOnlyThroughLink { false };
    bool accessorLeavesOnlyThroughLink { false };
    bool fillsObjectBeingMade { false };
    uint32_t getterInSight { noSummaryNode };
    uint32_t setterInSight { noSummaryNode };
    bool laysValueInSight { false };
    uint32_t valueInSight { noSummaryNode };
    uint32_t valueLaidThroughConstant { noSummaryNode };
};

struct SummaryCopyOfMembers {
    uint32_t target { 0 };
    uint32_t source { 0 };
    bool copiesAccessorsAsTheyAre { false };
};

struct HalfOfAccessorInSummary {
    FieldReference field;
    bool isGetter { false };

    friend constexpr auto operator<=>(const HalfOfAccessorInSummary&, const HalfOfAccessorInSummary&) = default;
};

struct SummaryConstruction {
    enum class Object : uint8_t {
        Allocated,
        AnsweredBySuperCall,
        This,
    };

    struct Key {
        PropertyKeyID key;
        bool isFilled { false };
        bool isLookedUp { false };
        bool holdsObject { false };
    };

    struct Part {
        FunctionID function;
        bool runsOnEveryWay { false };
        bool runsOnEveryWayToEnd { false };
    };

    Object object { Object::Allocated };
    bool isSeenFromOutside { false };
    uint32_t firstKey { 0 };
    uint32_t numberOfKeys { 0 };
    uint32_t firstKeyFilledAtEnd { 0 };
    uint32_t numberOfKeysFilledAtEnd { 0 };
    uint32_t firstPart { 0 };
    uint32_t numberOfParts { 0 };
};

enum class SummaryTrait : uint16_t {
    HasLoops = 1 << 0,
    Suspends = 1 << 1,
    UsesArguments = 1 << 2,
    HasDirectEval = 1 << 3,
    ListsKeys = 1 << 4,
    MayThrow = 1 << 5,
    ReadsUnderComputedKey = 1 << 6,
    StoresUnderComputedKey = 1 << 7,
    OnlyPassesOnGatheredArguments = 1 << 8,
    HoldsInt32Promise = 1 << 9,
    NeverReturns = 1 << 10,
    SuspendsAfterStart = 1 << 11,
};

struct OtherNameOfObjectBeingMade {
    ValueID parameter;
    ValueID object;
};

struct SummaryPlaceInBody {
    uint32_t number { 0 };
    uint32_t position { 0 };

    friend constexpr auto operator<=>(const SummaryPlaceInBody&, const SummaryPlaceInBody&) = default;
};

struct SummaryMethodOfClass {
    FunctionID constructor;
    FunctionID method;

    friend constexpr auto operator<=>(const SummaryMethodOfClass&, const SummaryMethodOfClass&) = default;
};

struct SummaryStoreOnPrototype {
    ValueID site;
    uint32_t position { 0 };
    VariableID variable;
    FunctionID function;
    FunctionID method;
    PropertyKeyID key;
    bool isOnEveryPath { false };

    friend constexpr auto operator<=>(const SummaryStoreOnPrototype&, const SummaryStoreOnPrototype&) = default;
};

struct SummaryUnitInVariable {
    VariableID variable;
    uint32_t unit { 0 };

    friend constexpr auto operator<=>(const SummaryUnitInVariable&, const SummaryUnitInVariable&) = default;
};

bool isBodyOfWrappedModule(const Program&, FunctionID);
bool isModuleOfBundle(const Program&, ModuleID);

struct SummaryClassInVariable {
    VariableID variable;
    FunctionID constructor;

    friend constexpr auto operator<=>(const SummaryClassInVariable&, const SummaryClassInVariable&) = default;
};

struct SummaryDefinition {
    VariableID variable;
    ValueID made;
    ValueID store;
    bool makesClosure { false };
};

struct SummaryHandler {
    BlockID handler;
    uint32_t caught { 0 };
};

struct SummaryScopeStoredFirst {
    ValueID allocation;
    uint32_t blockOfLeaving { UINT32_MAX };
    uint32_t positionOfLeaving { 0 };
    uint32_t scopeAtThisPlaceInOrderOfLeaving { 0 };
    Vector<uint32_t, 4> fields;
};

struct SummaryMakerThatEndsWhereItLeaves {
    ValueID allocation;
    uint32_t blockOfLeaving { UINT32_MAX };
    uint32_t positionOfLeaving { 0 };
};

struct SummaryTailCallThatIsOnlyCall {
    ValueID site;
    uint32_t numberOfArguments { 0 };
};

struct SummaryKeyAtSite {
    ValueID site;
    PropertyKeyID key;
};

struct LiteralArrayLocation {
    static constexpr uint32_t isVariable = UINT32_MAX;

    static LiteralArrayLocation of(VariableID variable) { return { isVariable, variable.index() }; }
    static LiteralArrayLocation of(FieldReference field) { return { field.layout.index(), field.field }; }
    friend constexpr auto operator<=>(const LiteralArrayLocation&, const LiteralArrayLocation&) = default;

    uint32_t layout;
    uint32_t number;
};

struct SummaryWrittenListOfVariable {
    LiteralArrayLocation place;
    DataID data;
    ValueID made;
    uint32_t length;
    uint8_t storage;
};

struct SummaryWithoutLists {
    uint64_t entryParametersCalledInLoops { 0 };
    uint32_t numberOfOperations { 0 };
    uint32_t numberOfOperationsThatOnlyTurnTruthRound { 0 };
    uint32_t weightOfOperations { 0 };
    uint32_t weightThatSwitchesSave { 0 };
    uint32_t numberOfOwnCalls { 0 };
    uint16_t entryParametersReadAsNames { 0 };
    uint16_t entryParametersReadAsKeys { 0 };
    uint32_t numberOfArgumentsOfOwnCalls { 0 };
    OptionSet<SummaryTrait> traits;
    bool operatorsLeftApartMayJoinTexts { false };
    bool someBlockHasHandler { false };
    bool someResultIsReadAsKey { false };
    bool mayReadPathOfAnyModule { true };
    bool makesObjects { false };
    bool hasClosures { false };

    bool has(SummaryTrait trait) const { return traits.contains(trait); }
};

struct SummaryStorage {
    struct Entry {
        uint32_t offset;
        uint32_t size;
    };

    static constexpr unsigned numberOfListsInRecord = 5;
    static constexpr unsigned numberOfListsKeptForWalk = 14;
    static constexpr uint64_t listsKeptForWalk = (uint64_t { 1 } << numberOfListsKeptForWalk) - 1;
    static constexpr uint64_t otherListsAreFreed = uint64_t { 1 } << 63;

    template<unsigned number>
    static constexpr uint64_t bitOf()
    {
        static_assert(number >= numberOfListsInRecord && number < numberOfListsInRecord + 63);
        return uint64_t { 1 } << (number - numberOfListsInRecord);
    }

    template<unsigned number>
    bool isEmpty() const
    {
        if constexpr (number < numberOfListsInRecord)
            return !sizesOfListsInRecord[number];
        else {
            if (!(listsInDirectories & bitOf<number>()))
                return true;
            RELEASE_ASSERT((bitOf<number>() & listsKeptForWalk) || !(listsInDirectories & otherListsAreFreed));
            return false;
        }
    }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    template<typename T>
    static T* pointerInto(void* block, size_t offset) { return static_cast<T*>(static_cast<void*>(static_cast<std::byte*>(block) + offset)); }

    template<typename T, unsigned number>
    std::span<T> list() const
    {
        if constexpr (number < numberOfListsInRecord) {
            ASSERT(listsInRecord[number] || !sizesOfListsInRecord[number]);
            return unsafeMakeSpan(static_cast<T*>(listsInRecord[number]), sizesOfListsInRecord[number]);
        } else {
            if (!(listsInDirectories & bitOf<number>()))
                return { };
            constexpr bool isKeptForWalk = !!(bitOf<number>() & listsKeptForWalk);
            constexpr uint64_t listsInFront = (bitOf<number>() - 1) & (isKeptForWalk ? listsKeptForWalk : ~listsKeptForWalk);
            void* block = isKeptForWalk ? blockKeptForWalk : listsInRecord[0];
            Entry entry = pointerInto<Entry>(block, isKeptForWalk ? 0 : offsetOfDirectory)[std::popcount(listsInDirectories & listsInFront)];
            return unsafeMakeSpan(pointerInto<T>(block, entry.offset), entry.size);
        }
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    std::array<void*, numberOfListsInRecord> listsInRecord;
    void* blockKeptForWalk;
    uint64_t listsInDirectories;
    std::array<uint32_t, numberOfListsInRecord> sizesOfListsInRecord;
    uint32_t offsetOfDirectory;
};

struct ThinSummary;

template<typename T, unsigned number>
class SummaryList {
    WTF_MAKE_NONCOPYABLE(SummaryList);
public:
    SummaryList() = default;

    bool isEmpty() const { return m_storage.isEmpty<number>(); }
    size_t size() const { return span().size(); }
    std::span<const T> span() const LIFETIME_BOUND { return m_storage.list<T, number>(); }
    const T& operator[](size_t index) const LIFETIME_BOUND { return at(index); }
    T& operator[](size_t index) LIFETIME_BOUND { return at(index); }
    const T& last() const LIFETIME_BOUND { return at(size() - 1); }

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
    const T* begin() const LIFETIME_BOUND { return span().data(); }
    const T* end() const LIFETIME_BOUND { return span().data() + span().size(); }

private:
    friend struct ThinSummary;
    friend class FactMisstater;

    std::span<T> mutableSpan() LIFETIME_BOUND { return m_storage.list<T, number>(); }

    T& at(size_t index) const LIFETIME_BOUND
    {
        std::span<T> list = m_storage.list<T, number>();
        if (index >= list.size()) [[unlikely]]
            CrashOnOverflow::overflowed();
        return list.data()[index];
    }
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

    SummaryStorage m_storage;
};

struct SummaryBeingMade;

struct ThinSummary : SummaryWithoutLists {
    struct AnswerOfRequire {
        ModuleID module;
        uint32_t node { 0 };
    };

    struct NameThatLoaderTakes {
        uint32_t read { 0 };
        VariableID variable;
    };

    struct Inspection {
        enum class Kind : uint8_t { WalksChain, ListsKeys };

        ValueID site;
        Kind kind { Kind::WalksChain };
        uint32_t subject { 0 };
    };
    struct AccessUnderConstantKey {
        ValueID site;
        PropertyKeyID key;
    };

    static constexpr uint32_t keyInListOfAllocation = 1u << 31;

    ThinSummary() = default;
    explicit ThinSummary(SummaryBeingMade&);

    ThinSummary(ThinSummary&& other)
        : SummaryWithoutLists(other)
    {
        nodes.m_storage = std::exchange(other.nodes.m_storage, { });
    }

    ThinSummary& operator=(ThinSummary&& other)
    {
        SummaryWithoutLists::operator=(other);
        std::swap(nodes.m_storage, other.nodes.m_storage);
        return *this;
    }

    ~ThinSummary()
    {
        if (nodes.m_storage.blockKeptForWalk) {
            std::ranges::destroy(scopesStoredFirst.mutableSpan());
            fastFree(nodes.m_storage.blockKeptForWalk);
        }
        trimForWalk();
    }

    void trimForWalk()
    {
        SummaryStorage& storage = nodes.m_storage;
        if (!storage.listsInRecord[0])
            return;
        fastFree(storage.listsInRecord[0]);
        storage.listsInRecord = { };
        storage.sizesOfListsInRecord.fill(UINT32_MAX);
        storage.listsInDirectories |= ~SummaryStorage::listsKeptForWalk;
        storage.offsetOfDirectory = 0;
    }

    union {
        SummaryList<SummaryNode, 0> nodes { };
        SummaryList<SummaryEdge, 1> edges;
        SummaryList<SummaryCall, 2> calls;
        SummaryList<uint32_t, 3> arguments;
        SummaryList<SummaryAccess, 4> accesses;

        SummaryList<BlockID, 5> blocksNeverEntered;
        SummaryList<SummaryScopeStoredFirst, 6> scopesStoredFirst;
        SummaryList<SummaryMakerThatEndsWhereItLeaves, 7> makersThatEndWhereTheyLeave;
        SummaryList<ValueID, 8> allocationsOfMakersThatEndWhereTheyLeave;
        SummaryList<SummaryConstruction, 9> constructions;
        SummaryList<SummaryConstruction::Key, 10> keysOfConstructions;
        SummaryList<PropertyKeyID, 11> keysFilledAtEnds;
        SummaryList<OtherNameOfObjectBeingMade, 12> otherNamesOfObjectsBeingMade;
        SummaryList<SummaryTailCallThatIsOnlyCall, 13> tailCallThatIsOnlyCall;
        SummaryList<SummaryKeyAtSite, 14> symbolsMadeOnce;
        SummaryList<ValueID, 15> callThatGivesResult;
        SummaryList<SummaryKeyAtSite, 16> storesOnResult;
        SummaryList<ValueID, 17> constantListsStoredIntoInLoops;
        SummaryList<SummaryWrittenListOfVariable, 18> writtenListsOfVariables;

        SummaryList<PropertyKeyID, 19> keysOfTestedFields;
        SummaryList<SummaryOperator, 20> operators;
        SummaryList<SummaryChange, 21> changes;
        SummaryList<SummaryMember, 22> members;
        SummaryList<VariableID, 23> functionVariablesStoredInto;
        SummaryList<SummaryConstruction::Part, 24> partsOfConstructions;
        SummaryList<uint32_t, 25> fieldsHeldOfLayoutsLeftEmpty;
        SummaryList<FieldReference, 26> fieldsFilledAtEndAlone;
        SummaryList<FieldReference, 27> fieldsEmptyAtEndAlone;
        SummaryList<HalfOfAccessorInSummary, 28> halvesOfAccessorsNotLaid;
        SummaryList<uint32_t, 29> fieldsListedByAllocations;
        SummaryList<LayoutID, 30> classesEvaluated;
        SummaryList<ValueID, 31> environmentsMade;
        SummaryList<ValueID, 32> sitesThatHandOverCaptures;
        SummaryList<SummaryDefinition, 33> definitions;
        SummaryList<FunctionID, 34> functionsNamed;
        SummaryList<SummaryHandler, 35> handlers;
        SummaryList<ModuleID, 36> modulesNamedInBlocksNeverEntered;
        SummaryList<ModuleID, 37> modulesHandedOut;
        SummaryList<SummaryPlaceInBody, 38> firstStoresOfBody;
        SummaryList<SummaryPlaceInBody, 39> firstUsesOfFunctions;
        SummaryList<ModuleID, 40> otherModulesNamed;
        SummaryList<AnswerOfRequire, 41> answersOfRequire;
        SummaryList<AnswerOfRequire, 42> objectsOfCompiledModules;
        SummaryList<NameThatLoaderTakes, 43> namesThatLoaderTakes;
        SummaryList<Inspection, 44> inspections;
        SummaryList<PropertyKeyID, 45> namesRead;
        SummaryList<PropertyKeyID, 46> namesCalled;
        SummaryList<PropertyKeyID, 47> namesStored;
        SummaryList<PropertyKeyID, 48> textsThatNameEvents;
        SummaryList<AccessUnderConstantKey, 49> accessesUnderConstantKeys;
        SummaryList<ModuleID, 50> modulesWhosePathIsRead;
        SummaryList<LocationMasks, 51> ownWritesOnWaysToReturn;
        SummaryList<uint32_t, 52> operandsOfOperatorsLeftApart;
        SummaryList<uint64_t, 53> wordsOfEffects;
        SummaryList<uint32_t, 54> namesThatHostLooksFor;
        SummaryList<SummaryCopyOfMembers, 55> copiesOfMembers;
        SummaryList<uint32_t, 56> placesOfWrittenLists;
        SummaryList<uint32_t, 57> definitionsByDescriptorInOrderOfSites;
        SummaryList<uint32_t, 58> callsAtSitesOfCopiesThatSpellStores;
        SummaryList<SummaryWrittenAccessor, 59> writtenAccessors;
        SummaryList<SummaryMethodOfClass, 60> methodsOfClasses;
        SummaryList<SummaryClassInVariable, 61> classesInVariables;
        SummaryList<SummaryUnitInVariable, 62> unitsInVariables;
        SummaryList<SummaryStoreOnPrototype, 63> storesOnPrototypes;
        SummaryList<SummaryPlaceInBody, 64> firstUsesOfVariables;
        SummaryList<uint32_t, 65> nodesHandedToEvals;
        SummaryList<SummaryReturn, 66> returnStatements;
        SummaryList<SummaryParameterTest, 67> parameterTests;
    };

    std::span<const SummaryParameterTest> testsOf(const SummaryReturn& statement) const LIFETIME_BOUND { return parameterTests.span().subspan(statement.firstTest, statement.numberOfTests); }

    const SummaryWrittenAccessor& writtenAccessorOf(uint32_t access) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(writtenAccessors, access, { }, &SummaryWrittenAccessor::access);
        RELEASE_ASSERT(found != writtenAccessors.end() && found->access == access);
        return *found;
    }

    uint64_t layoutsOfOwnWrites() const { return wordsOfEffects.isEmpty() ? 0 : wordsOfEffects[0]; }
    bool ownEffectsMayCallUnknownCode() const;
    JS_EXPORT_PRIVATE Effects ownEffects() const;
    JS_EXPORT_PRIVATE std::optional<Effects> ownEffectsOfOperators() const;

    bool holdsWrittenList(DataID data) const
    {
        return std::ranges::any_of(writtenListsOfVariables, [&](const SummaryWrittenListOfVariable& written) { return written.data == data; });
    }
    const SummaryWrittenListOfVariable* findLiteralArray(LiteralArrayLocation place) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(writtenListsOfVariables, place, { }, &SummaryWrittenListOfVariable::place);
        return found != writtenListsOfVariables.end() && found->place == place ? &*found : nullptr;
    }
    bool constantListIsStoredIntoInLoop(ValueID list) const { return std::ranges::binary_search(constantListsStoredIntoInLoops, list.index(), { }, &ValueID::index); }
    bool makerEndsWhereItLeaves(ValueID allocation) const { return std::ranges::binary_search(allocationsOfMakersThatEndWhereTheyLeave, allocation.index(), { }, &ValueID::index); }

    void forEachListOfAllocation(NOESCAPE const Invocable<void(LayoutID, std::span<const uint32_t>)> auto& functor) const
    {
        std::span<const uint32_t> rest = fieldsListedByAllocations.span();
        while (!rest.empty()) {
            functor(LayoutID(rest[0]), rest.subspan(2, rest[1]));
            rest = rest.subspan(2 + rest[1]);
        }
    }

    static constexpr uint32_t isFilledByCopy = 1u << 31;
    static constexpr uint32_t noCopy = UINT32_MAX;
    void forEachUnfilledLayout(NOESCAPE const Invocable<void(LayoutID, std::span<const uint32_t> fieldsHeld, uint32_t copy)> auto& functor) const
    {
        std::span<const uint32_t> rest = fieldsHeldOfLayoutsLeftEmpty.span();
        while (!rest.empty()) {
            bool isFilled = rest[1] & isFilledByCopy;
            size_t first = isFilled ? 3 : 2;
            uint32_t numberHeld = rest[1] & ~isFilledByCopy;
            functor(LayoutID(rest[0]), rest.subspan(first, numberHeld), isFilled ? rest[2] : noCopy);
            rest = rest.subspan(first + numberHeld);
        }
    }
    void forEachLayoutLeftEmpty(NOESCAPE const Invocable<void(LayoutID, std::span<const uint32_t> fieldsHeld)> auto& functor) const
    {
        forEachUnfilledLayout([&](LayoutID layout, std::span<const uint32_t> fieldsHeld, uint32_t) {
            functor(layout, fieldsHeld);
        });
    }

    bool leavesEmpty(FieldReference field) const
    {
        bool leavesEmpty = false;
        forEachLayoutLeftEmpty([&](LayoutID layout, std::span<const uint32_t> fieldsHeld) {
            leavesEmpty |= layout == field.layout && !std::ranges::binary_search(fieldsHeld, field.field);
        });
        return leavesEmpty;
    }
};

static_assert(sizeof(ThinSummary) <= 128);

struct SummaryBeingMade : SummaryWithoutLists {
    bool makesCopies { false };
    Vector<SummaryNode> nodes;
    Vector<SummaryEdge> edges;
    Vector<SummaryCall> calls;
    Vector<uint32_t> arguments;
    Vector<SummaryAccess> accesses;
    Vector<BlockID> blocksNeverEntered;
    Vector<SummaryScopeStoredFirst> scopesStoredFirst;
    Vector<SummaryMakerThatEndsWhereItLeaves> makersThatEndWhereTheyLeave;
    Vector<ValueID> allocationsOfMakersThatEndWhereTheyLeave;
    Vector<ValueID> constantListsStoredIntoInLoops;
    Vector<SummaryWrittenListOfVariable> writtenListsOfVariables;
    Vector<SummaryConstruction> constructions;
    Vector<SummaryConstruction::Key> keysOfConstructions;
    Vector<PropertyKeyID> keysFilledAtEnds;
    Vector<OtherNameOfObjectBeingMade> otherNamesOfObjectsBeingMade;
    Vector<SummaryTailCallThatIsOnlyCall> tailCallThatIsOnlyCall;
    Vector<SummaryKeyAtSite> symbolsMadeOnce;
    Vector<ValueID> callThatGivesResult;
    Vector<SummaryKeyAtSite> storesOnResult;
    Vector<PropertyKeyID> keysOfTestedFields;
    Vector<SummaryOperator> operators;
    Vector<SummaryChange> changes;
    Vector<SummaryMember> members;
    Vector<VariableID> functionVariablesStoredInto;
    Vector<SummaryConstruction::Part> partsOfConstructions;
    Vector<uint32_t> fieldsHeldOfLayoutsLeftEmpty;
    Vector<FieldReference> fieldsFilledAtEndAlone;
    Vector<FieldReference> fieldsEmptyAtEndAlone;
    Vector<HalfOfAccessorInSummary> halvesOfAccessorsNotLaid;
    Vector<uint32_t> fieldsListedByAllocations;
    Vector<LayoutID> classesEvaluated;
    Vector<ValueID> environmentsMade;
    Vector<ValueID> sitesThatHandOverCaptures;
    Vector<SummaryDefinition> definitions;
    Vector<FunctionID> functionsNamed;
    Vector<SummaryHandler> handlers;
    Vector<ModuleID> modulesNamedInBlocksNeverEntered;
    Vector<ModuleID> modulesHandedOut;
    Vector<SummaryPlaceInBody> firstStoresOfBody;
    Vector<SummaryPlaceInBody> firstUsesOfFunctions;
    Vector<ModuleID> otherModulesNamed;
    Vector<ThinSummary::AnswerOfRequire> answersOfRequire;
    Vector<ThinSummary::AnswerOfRequire> objectsOfCompiledModules;
    Vector<ThinSummary::NameThatLoaderTakes> namesThatLoaderTakes;
    Vector<uint32_t> namesThatHostLooksFor;
    Vector<ThinSummary::Inspection> inspections;
    Vector<PropertyKeyID> namesRead;
    Vector<PropertyKeyID> namesCalled;
    Vector<PropertyKeyID> namesStored;
    Vector<PropertyKeyID> textsThatNameEvents;
    Vector<ThinSummary::AccessUnderConstantKey> accessesUnderConstantKeys;
    Vector<ModuleID> modulesWhosePathIsRead;
    Vector<LocationMasks, 1> ownWritesOnWaysToReturn;
    Vector<uint32_t> operandsOfOperatorsLeftApart;
    Vector<SummaryCopyOfMembers> copiesOfMembers;
    Vector<uint32_t> placesOfWrittenLists;
    Vector<uint32_t> definitionsByDescriptorInOrderOfSites;
    Vector<uint32_t> callsAtSitesOfCopiesThatSpellStores;
    Vector<SummaryWrittenAccessor> writtenAccessors;
    Vector<SummaryMethodOfClass> methodsOfClasses;
    Vector<SummaryClassInVariable> classesInVariables;
    Vector<SummaryUnitInVariable> unitsInVariables;
    Vector<SummaryStoreOnPrototype> storesOnPrototypes;
    Vector<SummaryPlaceInBody> firstUsesOfVariables;
    Vector<uint32_t> nodesHandedToEvals;
    Vector<SummaryReturn> returnStatements;
    Vector<SummaryParameterTest> parameterTests;
    uint64_t layoutsOfOwnWrites { 0 };
    Effects ownEffects;
    std::optional<Effects> ownEffectsOfOperators;
};

class ThinSummaries {
public:
    explicit ThinSummaries(size_t numberOfFunctions) { m_summaries.grow(numberOfFunctions); }
    ThinSummaries(size_t numberOfFunctions, const ParallelLoops& loops) { m_summaries.grow(loops, numberOfFunctions); }

    const ThinSummary& at(FunctionID id) const LIFETIME_BOUND { return m_summaries[id.index()]; }
    ThinSummary& at(FunctionID id) LIFETIME_BOUND { return m_summaries[id.index()]; }
    size_t size() const { return m_summaries.size(); }
    void append(ThinSummary&& summary) { m_summaries.append(WTF::move(summary)); }

    void trimForWalk(const ParallelLoops& loops)
    {
        forEachRangeOfIndicesInParallel(loops, m_summaries.size(), [&](size_t first, size_t end) {
            for (ThinSummary& summary : m_summaries.mutableSpan().subspan(first, end - first))
                summary.trimForWalk();
        });
    }

    const BitVector& textsSpelledThatNameFieldsNeverWritten() const LIFETIME_BOUND { return m_textsSpelledThatNameFieldsNeverWritten; }
    void setTextsSpelledThatNameFieldsNeverWritten(BitVector&& texts) { m_textsSpelledThatNameFieldsNeverWritten = WTF::move(texts); }
    const BitVector& namesUnderWhichEventsHang() const LIFETIME_BOUND { return m_namesUnderWhichEventsHang; }
    void setNamesUnderWhichEventsHang(const BitVector& names) { m_namesUnderWhichEventsHang = names; }
    const SetsOfLayoutsWritten& setsOfLayoutsWrittenInBodies() const LIFETIME_BOUND { return m_setsOfLayoutsWrittenInBodies; }
    void setSetsOfLayoutsWrittenInBodies(SetsOfLayoutsWritten&& sets) { m_setsOfLayoutsWrittenInBodies = WTF::move(sets); }
    uint64_t numberOfValuesScanned() const { return m_numberOfValuesScanned; }
    void setNumberOfValuesScanned(uint64_t number) { m_numberOfValuesScanned = number; }
    std::span<const FunctionID> functionsThatCopyMembers() const LIFETIME_BOUND { return m_functionsThatCopyMembers.span(); }
    void setFunctionsThatCopyMembers(Vector<FunctionID>&& functions) { m_functionsThatCopyMembers = WTF::move(functions); }
    std::span<const FunctionID> copyingFunctions() const LIFETIME_BOUND { return m_copyingFunctions.span(); }
    void setCopyingFunctions(Vector<FunctionID>&& functions) { m_copyingFunctions = WTF::move(functions); }
    struct MakerOfClosures {
        FunctionID closure;
        FunctionID maker;
    };
    FunctionID findMakerAboveCopyOfMembers(FunctionID closure) const
    {
        auto found = std::ranges::lower_bound(m_makersAboveCopiesOfMembers, closure, { }, &MakerOfClosures::closure);
        return found != m_makersAboveCopiesOfMembers.end() && found->closure == closure ? found->maker : FunctionID();
    }
    void setMakersAboveCopiesOfMembers(Vector<MakerOfClosures>&& makers) { m_makersAboveCopiesOfMembers = WTF::move(makers); }
    PropertyKeyID symbolHeldBy(VariableID variable) const { return variable.index() < m_symbolsOfVariables.size() ? m_symbolsOfVariables[variable.index()] : PropertyKeyID(); }
    void setSymbolsOfVariables(Vector<PropertyKeyID>&& symbols) { m_symbolsOfVariables = WTF::move(symbols); }
    struct ElementStores {
        struct LiteralArgument {
            FunctionID caller;
            ValueID site;
            uint32_t argument { 0 };
            DataID literal;
        };
        void merge(const ElementStores& other)
        {
            literalArguments.appendVector(other.literalArguments);
            literalResults.appendVector(other.literalResults);
            parameters.appendVector(other.parameters);
            results.appendVector(other.results);
        }

        Vector<LiteralArgument> literalArguments;
        Vector<std::pair<FunctionID, DataID>> literalResults;
        Vector<std::pair<FunctionID, uint32_t>> parameters;
        Vector<std::pair<FunctionID, ValueID>> results;
    };
    void setElementStores(ElementStores&& stores) { m_elementStores = WTF::move(stores); }
    ElementStores takeElementStores() { return WTF::move(m_elementStores); }
    void noteStoredByElement(DataID literal) { m_literalsStoredByElement.set(literal.index()); }
    bool storesElementInto(VariableID variable) const { return m_variablesStoredByElement.get(variable.index()); }
    bool storesElementInto(DataID literal) const { return m_literalsStoredByElement.get(literal.index()); }
    void setStoresByElement(BitVector&& variables, BitVector&& literals)
    {
        m_variablesStoredByElement = WTF::move(variables);
        m_literalsStoredByElement = WTF::move(literals);
    }

private:
    LargeList<ThinSummary> m_summaries;
    BitVector m_textsSpelledThatNameFieldsNeverWritten;
    BitVector m_namesUnderWhichEventsHang;
    SetsOfLayoutsWritten m_setsOfLayoutsWrittenInBodies;
    uint64_t m_numberOfValuesScanned { 0 };
    Vector<FunctionID> m_functionsThatCopyMembers;
    Vector<FunctionID> m_copyingFunctions;
    Vector<MakerOfClosures> m_makersAboveCopiesOfMembers;
    Vector<PropertyKeyID> m_symbolsOfVariables;
    BitVector m_variablesStoredByElement;
    BitVector m_literalsStoredByElement;
    ElementStores m_elementStores;
};

ThinSummaries scanBodies(const Program&, Diagnostics&, const ParallelLoops&, Mechanisms = allMechanisms);

PropertyKeyID keyOfSymbolThatCallMakes(const Program&, FunctionID, const BodyValue& call, const ScopedLambda<BodyValue(ValueID)>& definitionOf);
PropertyKeyID keyOfSymbolMadeOnceBy(const Program&, const SummaryCall&);
bool requireAnswersWithResultOfBody(const Program&, ModuleID referrer, ModuleID target);
LayoutID classOfConstructorFunction(const Program&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
