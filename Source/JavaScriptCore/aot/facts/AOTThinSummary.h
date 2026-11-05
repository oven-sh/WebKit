/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTEffects.h"
#include "AOTLargeList.h"
#include "AOTLayout.h"
#include "AOTMechanisms.h"
#include "AOTOpcode.h"
#include "AOTThreads.h"
#include "AOTType.h"
#include <algorithm>
#include <memory>
#include <optional>
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
    constexpr std::optional<uint32_t> indexOfInternalField() const { return m_number >= firstInternalField && m_number < firstStateSlot ? std::optional(m_number - firstInternalField) : std::nullopt; }

    friend constexpr bool operator==(SlotOfContents, SlotOfContents) = default;

private:
    // The slots that no enumerator of ContentsSlot names are numbered behind the enumerators, in this order. Whoever adds one adds it here, so the count below knows of it.
    enum SlotBehindEnumerators : uint32_t { UnderUnknownNames, UnderAnyName, ReturnedAlone, numberOfSlotsBehindEnumerators };
    static constexpr SlotOfContents behindEnumerators(SlotBehindEnumerators slot) { return fromNumber(numberOfEnumerators<ContentsSlot> + slot); }
    static constexpr uint32_t firstInternalField = 16;
    static_assert(numberOfEnumerators<ContentsSlot> + numberOfSlotsBehindEnumerators <= firstInternalField);
    static constexpr uint32_t firstStateSlot = 1u << 16;
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
        unit.m_bits = bits;
        return unit;
    }

    constexpr uint32_t bits() const { return m_bits; }
    constexpr explicit operator bool() const { return !!m_bits; }
    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> 30); }
    constexpr StringID text() const { return StringID(m_bits & (payloadLimit - 1)); }
    constexpr int64_t integer() const { return static_cast<int64_t>(m_bits & (payloadLimit - 1)) - payloadLimit / 2; }
    constexpr bool boolean() const { return m_bits & 1; }

    friend constexpr bool operator==(WrittenUnit, WrittenUnit) = default;

private:
    static constexpr uint32_t payloadLimit = 1u << 30;

    constexpr WrittenUnit(Kind kind, uint32_t payload)
        : m_bits(static_cast<uint32_t>(kind) << 30 | payload)
    {
    }

    uint32_t m_bits { 0 };
};

struct SummaryEdge {
    uint32_t from { 0 };
    uint32_t to { 0 };
    TypeID passes;
    bool runsCodeOfValue { false };
};

struct SummaryCall {
    enum class Callee : uint8_t {
        Binding,
        Name,
        Value,
        Builtin,
    };

    ValueID site;
    Callee callee { Callee::Value };
    bool isConstruction { false };
    bool isInLoop { false };
    bool isInTailPosition { false };
    bool passesList { false };
    bool isMadeByProtocol { false };
    bool resultIsReadAsKey { false };
    bool liesOnNoWayToReturn { false };
    FunctionID function;
    BuiltinID builtin;
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
    bool isPartOfConstruction { false };
    bool subjectIsAnsweredBySuperCall { false };
    bool storesObjectMadeForIt { false };
    bool standsForObjectMadeAtSite { false };
    bool liesOnNoWayToReturn { false };
    uint32_t subject { 0 };
    uint32_t receiver { noSummaryNode };
    PropertyKeyID key;
    uint32_t keyNode { noSummaryNode };
    StringID beginningOfKey;
    uint32_t value { noSummaryNode };
    SlotOfContents slot;
    uint32_t whatHoldsNothing { noSummaryNode };
    bool standsRightBehindCall { false };
    uint32_t thrownTo { noSummaryNode };
    bool laysAccessorAsWritten { false };
    FunctionID writtenGetter;
    FunctionID writtenSetter;
    LayoutID mapOfWrittenAccessor;
    uint32_t descriptorFromElsewhere { noSummaryNode };
    bool isByDescriptor { false };
    std::optional<OptionSet<FieldForm>> formCreated;

    bool isToContents() const { return !!slot; }
    bool laysValueInSightUnderName() const { return !isToContents() && (kind == Kind::Store || (kind == Kind::Define && value && value != noSummaryNode)); }
};

struct SummaryOperator {
    static constexpr uint32_t mostOperands = 2;

    ValueID site;
    Opcode opcode { Opcode::Add };
    uint32_t result { 0 };
    uint32_t operands[mostOperands] { noSummaryNode, noSummaryNode };
    uint32_t converted[mostOperands] { noSummaryNode, noSummaryNode };
};

struct SummaryMember {
    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    MethodKind kind { MethodKind::Method };
    uint32_t index { 0 };
    PropertyKeyID key;
    uint32_t value { 0 };
};

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
    uint32_t other { noSummaryNode };
    enum class Lays : uint8_t { WhatDescriptorSays, Getter, Setter };
    Lays lays { Lays::WhatDescriptorSays };
    uint32_t getter { noSummaryNode };
    bool liesOnNoWayToReturn { false };
    bool definesWhatItCopies { false };
    bool isUnderNameThatNobodyElseNames { false };
    bool descriptorIsInSight { false };
    uint32_t getterInSight { noSummaryNode };
    uint32_t setterInSight { noSummaryNode };
    bool laysValueInSight { false };
    uint32_t valueInSight { noSummaryNode };
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
    Vector<uint32_t, 4> fields;
};

struct SummaryMakerThatEndsWhereItLeaves {
    ValueID allocation;
    uint32_t blockOfLeaving { UINT32_MAX };
    uint32_t positionOfLeaving { 0 };
};

struct ThinSummary {
    Vector<SummaryNode> nodes;
    Vector<SummaryEdge> edges;
    Vector<PropertyKeyID> keysOfTestedFields;
    Vector<SummaryCall> calls;
    Vector<uint32_t> arguments;
    Vector<SummaryAccess> accesses;
    Vector<SummaryOperator> operators;
    Vector<SummaryChange> changes;
    Vector<SummaryMember> members;
    Vector<SummaryConstruction> constructions;
    Vector<SummaryConstruction::Key> keysOfConstructions;
    Vector<VariableID> functionVariablesStoredInto;
    Vector<SummaryConstruction::Part> partsOfConstructions;
    Vector<FieldReference> fieldsLeftEmpty;
    Vector<SummaryMakerThatEndsWhereItLeaves> makersThatEndWhereTheyLeave;
    Vector<FieldReference> fieldsFilledAtEndAlone;
    Vector<FieldReference> fieldsEmptyAtEndAlone;
    Vector<HalfOfAccessorInSummary> halvesOfAccessorsNotLaid;
    Vector<PropertyKeyID> keysFilledAtEnds;
    static constexpr uint32_t keyInListOfAllocation = 1u << 31;
    Vector<uint32_t> fieldsListedByAllocations;
    Vector<LayoutID> classesEvaluated;
    Vector<ValueID> environmentsMade;
    Vector<SummaryScopeStoredFirst> scopesStoredFirst;
    Vector<ValueID> environmentsHandedToFunctionsEntered;
    Vector<SummaryDefinition> definitions;
    Vector<FunctionID> functionsNamed;
    Vector<BlockID> blocksNeverEntered;
    Vector<OtherNameOfObjectBeingMade> otherNamesOfObjectsBeingMade;
    Vector<SummaryHandler> handlers;
    Vector<ModuleID> modulesNamedInBlocksNeverEntered;
    Vector<ModuleID> modulesHandedOut;
    Vector<SummaryPlaceInBody> firstStoresOfBody;
    Vector<SummaryPlaceInBody> firstUsesOfFunctions;
    Vector<ModuleID> otherModulesNamed;
    struct AnswerOfRequire {
        ModuleID module;
        uint32_t node { 0 };
    };
    Vector<AnswerOfRequire> answersOfRequire;
    Vector<AnswerOfRequire> objectsOfCompiledModules;
    struct NameThatLoaderTakes {
        uint32_t read { 0 };
        VariableID variable;
    };
    Vector<NameThatLoaderTakes> namesThatLoaderTakes;
    struct Inspection {
        enum class Kind : uint8_t { WalksChain, ListsKeys };

        ValueID site;
        Kind kind { Kind::WalksChain };
        uint32_t subject { 0 };
    };
    Vector<Inspection> inspections;

    void forEachListOfAllocation(NOESCAPE const Invocable<void(LayoutID, std::span<const uint32_t>)> auto& functor) const
    {
        std::span<const uint32_t> rest = fieldsListedByAllocations.span();
        while (!rest.empty()) {
            functor(LayoutID(rest[0]), rest.subspan(2, rest[1]));
            rest = rest.subspan(2 + rest[1]);
        }
    }
    Vector<PropertyKeyID> namesRead;
    Vector<PropertyKeyID> namesCalled;
    Vector<PropertyKeyID> namesStored;
    Effects ownEffects;
    class KnownEffectsApart {
    public:
        struct Lists {
            Vector<LocationMasks> ownWritesOnWaysToReturn;
            Vector<Effects> ownEffectsOfOperators;
            Vector<uint32_t> operandsOfOperatorsLeftApart;
        };

        KnownEffectsApart() = default;
        KnownEffectsApart(KnownEffectsApart&&) = default;
        KnownEffectsApart& operator=(KnownEffectsApart&&) = default;
        KnownEffectsApart(const KnownEffectsApart& other)
            : m_lists(other.m_lists ? makeUniqueWithoutFastMallocCheck<Lists>(*other.m_lists) : nullptr)
        {
        }
        KnownEffectsApart& operator=(const KnownEffectsApart& other)
        {
            m_lists = other.m_lists ? makeUniqueWithoutFastMallocCheck<Lists>(*other.m_lists) : nullptr;
            return *this;
        }

        Lists& ensureLists()
        {
            if (!m_lists)
                m_lists = makeUniqueWithoutFastMallocCheck<Lists>();
            return *m_lists;
        }
        const LocationMasks* ownWritesOnWaysToReturn() const LIFETIME_BOUND { return m_lists && !m_lists->ownWritesOnWaysToReturn.isEmpty() ? &m_lists->ownWritesOnWaysToReturn[0] : nullptr; }
        const Effects* ownEffectsOfOperators() const LIFETIME_BOUND { return m_lists && !m_lists->ownEffectsOfOperators.isEmpty() ? &m_lists->ownEffectsOfOperators[0] : nullptr; }
        std::span<const uint32_t> operandsOfOperatorsLeftApart() const LIFETIME_BOUND { return m_lists ? m_lists->operandsOfOperatorsLeftApart.span() : std::span<const uint32_t>(); }

    private:
        std::unique_ptr<Lists> m_lists;
    };
    KnownEffectsApart knownEffectsApart;
    uint64_t layoutsOfOwnWrites { 0 };
    bool operatorsLeftApartMayJoinTexts { false };
    bool someBlockHasHandler { false };
    bool someResultIsReadAsKey { false };
    OptionSet<SummaryTrait> traits;
    uint64_t entryParametersCalledInLoops { 0 };
    uint64_t entryParametersLetGo { 0 };
    uint32_t numberOfOperations { 0 };
    uint32_t weightOfOperations { 0 };
    Vector<PropertyKeyID> textsThatNameEvents;
    struct AccessUnderConstantKey {
        ValueID site;
        PropertyKeyID key;
    };
    Vector<AccessUnderConstantKey> accessesUnderConstantKeys;
    Vector<ModuleID> modulesWhosePathIsRead;
    bool mayReadPathOfAnyModule { true };

    bool has(SummaryTrait trait) const { return traits.contains(trait); }
};

struct LiteralOfVariable {
    VariableID variable;
    Opcode opcode { Opcode::Undefined };
    uint64_t bits { 0 };
};

class ThinSummaries {
public:
    explicit ThinSummaries(size_t numberOfFunctions) { m_summaries.grow(numberOfFunctions); }
    ThinSummaries(size_t numberOfFunctions, const ParallelLoops& loops) { m_summaries.grow(loops, numberOfFunctions); }

    const ThinSummary& at(FunctionID id) const LIFETIME_BOUND { return m_summaries[id.index()]; }
    ThinSummary& at(FunctionID id) LIFETIME_BOUND { return m_summaries[id.index()]; }
    size_t size() const { return m_summaries.size(); }
    void append(ThinSummary&& summary) { m_summaries.append(WTF::move(summary)); }

    const BitVector& textsSpelledThatNameFieldsNeverWritten() const LIFETIME_BOUND { return m_textsSpelledThatNameFieldsNeverWritten; }
    void setTextsSpelledThatNameFieldsNeverWritten(BitVector&& texts) { m_textsSpelledThatNameFieldsNeverWritten = WTF::move(texts); }

    const LiteralOfVariable* findLiteralOfVariable(VariableID variable) const LIFETIME_BOUND
    {
        auto found = std::ranges::lower_bound(m_literalsOfVariables, variable, { }, &LiteralOfVariable::variable);
        return found != m_literalsOfVariables.end() && found->variable == variable ? &*found : nullptr;
    }
    void setLiteralsOfVariables(Vector<LiteralOfVariable>&& literals) { m_literalsOfVariables = WTF::move(literals); }

private:
    LargeList<ThinSummary> m_summaries;
    BitVector m_textsSpelledThatNameFieldsNeverWritten;
    Vector<LiteralOfVariable> m_literalsOfVariables;
};

ThinSummaries scanBodies(const Program&, Diagnostics&, const ParallelLoops&, Mechanisms = allMechanisms);

PropertyKeyID keyOfSymbolThatCallMakes(const Program&, FunctionID, const BodyValue& call, const ScopedLambda<BodyValue(ValueID)>& definitionOf);
PropertyKeyID keyOfSymbolMadeOnceBy(const Program&, const SummaryCall&);
bool requireAnswersWithResultOfBody(const Program&, ModuleID referrer, ModuleID target);
LayoutID classOfConstructorFunction(const Program&, FunctionID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
