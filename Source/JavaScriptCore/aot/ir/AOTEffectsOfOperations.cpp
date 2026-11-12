/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffectsOfOperations.h"

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTBodyIndex.h"
#include "AOTCode.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeRelations.h"
#include "JSString.h"
#include <algorithm>
#include <array>
#include <wtf/dtoa.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(declaredBoundOf(Opcode::NewObject).writes.containsEntire(AbstractHeap::MemberGuards));
static_assert(declaredBoundOf(Opcode::BeginMemberGuards).writes.isSubsetOf(declaredBoundOf(Opcode::NewObject).writes) && declaredBoundOf(Opcode::NewObject).mayCollect);

bool hasTextByItself(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::NumberLike | Primitive::AnyString | Primitive::BigInt);
}

uint64_t longestTextOf(TypeReader reader, const Type& type)
{
    constexpr uint64_t longestTextOfNumberLike = std::tuple_size_v<NumberToStringBuffer>;
    static_assert(longestTextOfNumberLike >= "undefined"_s.length());

    if (type.primitives & (Primitive::String | Primitive::BigInt))
        return JSString::MaxLength;
    uint64_t longest = type.primitives & Primitive::NumberLike ? longestTextOfNumberLike : 0;
    if (!type.stringLiterals)
        return longest;
    const Program* program = reader.program();
    if (!program)
        return JSString::MaxLength;
    reader.at(type.stringLiterals).forEach([&](StringID literal) {
        longest = std::max<uint64_t>(longest, program->strings[literal].size());
    });
    return longest;
}

namespace {

class TextsJoinedByOperation {
public:
    void add(TypeReader reader, const Type& type)
    {
        m_haveAllTextByThemselves &= hasTextByItself(type);
        if (m_haveAllTextByThemselves)
            m_longest += longestTextOf(reader, type);
    }

    bool fitInString() const { return m_haveAllTextByThemselves && m_longest <= JSString::MaxLength; }

private:
    uint64_t m_longest { 0 };
    bool m_haveAllTextByThemselves { true };
};

enum class CodeRunByProtocols : uint8_t { None, IterationOfStrings, Unknown };

struct OperandSeenByProtocols {
    uint16_t primitives { 0 };
    bool mayBeMoreThanPrimitive { false };
};

class EffectsBuilderInPlace {
public:
    EffectsBuilderInPlace(const Program& program, Effects& effects)
        : m_program(program)
        , m_effects(effects)
    {
    }

    ALWAYS_INLINE void reads(const MemoryLocation& location) { m_effects.reads.add(location.heap(), locationMaskIn(m_program, location)); }
    ALWAYS_INLINE void writes(const MemoryLocation& location) { m_effects.writes.add(location.heap(), locationMaskIn(m_program, location)); }
    ALWAYS_INLINE void readsAllHeaps() { m_effects.reads = LocationMasks::top(); }
    ALWAYS_INLINE void throws() { m_effects.mayThrow = true; }
    ALWAYS_INLINE void collects() { m_effects.mayCollect = true; }
    ALWAYS_INLINE void suspends() { m_effects.maySuspend = true; }
    ALWAYS_INLINE void terminates() { m_effects.mayBeTerminated = true; }
    ALWAYS_INLINE void unknownCode() { m_effects.merge(Effects::unknownCode()); }
    ALWAYS_INLINE void asOverload() { unknownCode(); }

private:
    const Program& m_program;
    Effects& m_effects;
};

class BuilderOfEffectsOfEveryOperation {
public:
    constexpr void reads(const MemoryLocation& location) { add(m_effects.reads, location); }
    constexpr void writes(const MemoryLocation& location) { add(m_effects.writes, location); }
    constexpr void readsAtPayload(AbstractHeap) { m_hangsOnOperation = true; }
    constexpr void writesAtPayload(AbstractHeap) { m_hangsOnOperation = true; }
    constexpr void readsAllHeaps() { m_effects.reads = LocationMasks::top(); }
    constexpr void throws() { m_effects.mayThrow = true; }
    constexpr void collects() { m_effects.mayCollect = true; }
    constexpr void suspends() { m_effects.maySuspend = true; }
    constexpr void terminates() { m_effects.mayBeTerminated = true; }
    constexpr void unknownCode() { m_effects.merge(Effects::unknownCode()); }
    constexpr void asOverload()
    {
        m_namesOnlyEntireHeaps = false;
        unknownCode();
    }

    constexpr const Effects& effects() const LIFETIME_BOUND { return m_effects; }
    constexpr bool hangsOnOperation() const { return m_hangsOnOperation; }
    constexpr bool namesOnlyEntireHeaps() const { return m_namesOnlyEntireHeaps && !m_hangsOnOperation; }

private:
    constexpr void add(LocationMasks& masks, const MemoryLocation& location)
    {
        if (location.isEntireHeap()) {
            masks.add(location.heap());
            return;
        }
        m_namesOnlyEntireHeaps = false;
        if (location.heap() == AbstractHeap::Elements)
            masks.add(location.heap(), locationMaskOfElementBase(location.elementBase()));
        else
            m_hangsOnOperation = true;
    }

    Effects m_effects { Effects::none() };
    bool m_hangsOnOperation { false };
    bool m_namesOnlyEntireHeaps { true };
};

template<size_t capacity>
struct EffectsOfEveryOperation {
    OpcodeSet opcodes;
    OpcodeSet opcodesNamingOnlyEntireHeaps;
    std::array<uint8_t, numberOfOpcodes> rowOf { };
    std::array<Effects, capacity> rows { };
    size_t numberOfRows { 0 };
};

constexpr auto effectsOfEveryOperationWithRoomForAll = [] {
    EffectsOfEveryOperation<numberOfOpcodes> result;
    for (Opcode opcode : allMembersOf<Opcode>) {
        BuilderOfEffectsOfEveryOperation builder;
        forEachDeclaredEffect(opcode, builder);
        if (builder.hangsOnOperation())
            continue;
        Effects effects = withDeclaredKindOfThrow(opcode, builder.effects());
        if (opcode == Opcode::PutDoubleIndex || opcode == Opcode::PutOwnDoubleIndex) {
            uint64_t written = effects.writes.maskOf(AbstractHeap::Elements) & ~cellHoldingElementsMask;
            effects.writes.remove(AbstractHeapSet { AbstractHeap::Elements });
            effects.writes.add(AbstractHeap::Elements, written);
        }
        size_t row = 0;
        while (row < result.numberOfRows && result.rows[row] != effects)
            ++row;
        if (row == result.numberOfRows)
            result.rows[result.numberOfRows++] = effects;
        result.rowOf[static_cast<unsigned>(opcode)] = static_cast<uint8_t>(row);
        result.opcodes.add(opcode);
        if (builder.namesOnlyEntireHeaps())
            result.opcodesNamingOnlyEntireHeaps.add(opcode);
    }
    return result;
}();
static_assert(effectsOfEveryOperationWithRoomForAll.numberOfRows && effectsOfEveryOperationWithRoomForAll.numberOfRows <= 256);

constexpr auto effectsOfEveryOperation = [] {
    EffectsOfEveryOperation<effectsOfEveryOperationWithRoomForAll.numberOfRows> result;
    result.opcodes = effectsOfEveryOperationWithRoomForAll.opcodes;
    result.opcodesNamingOnlyEntireHeaps = effectsOfEveryOperationWithRoomForAll.opcodesNamingOnlyEntireHeaps;
    result.rowOf = effectsOfEveryOperationWithRoomForAll.rowOf;
    for (; result.numberOfRows < result.rows.size(); ++result.numberOfRows)
        result.rows[result.numberOfRows] = effectsOfEveryOperationWithRoomForAll.rows[result.numberOfRows];
    return result;
}();

constexpr const Effects& effectsOfEveryOperationWith(Opcode opcode)
{
    return effectsOfEveryOperation.rows[effectsOfEveryOperation.rowOf[static_cast<unsigned>(opcode)]];
}

static_assert(effectsOfEveryOperation.opcodesNamingOnlyEntireHeaps.contains(Opcode::IsNotFrozen));
static_assert([] {
    bool rawStoresLackMarkAlone = true;
    for (Opcode opcode : { Opcode::PutDoubleIndex, Opcode::PutOwnDoubleIndex })
        rawStoresLackMarkAlone &= effectsOfEveryOperationWith(opcode).writes.maskOf(AbstractHeap::Elements) == (locationMaskOfElementBase(ElementBase::Array) & ~cellHoldingElementsMask);
    for (Opcode opcode : { Opcode::PutContiguousIndex, Opcode::PutOwnContiguousIndex, Opcode::PutOwnInt32Index })
        rawStoresLackMarkAlone &= effectsOfEveryOperationWith(opcode).writes.maskOf(AbstractHeap::Elements) == locationMaskOfElementBase(ElementBase::Array);
    return rawStoresLackMarkAlone && std::popcount(locationMaskOfElementBase(ElementBase::Array)) == 2;
}());
static_assert([] {
    bool areAllTheBound = true;
    effectsOfEveryOperation.opcodesNamingOnlyEntireHeaps.forEach([&](Opcode opcode) {
        areAllTheBound &= effectsOfEveryOperationWith(opcode) == declaredBoundOf(opcode);
    });
    return areAllTheBound;
}());

} // anonymous namespace

static constexpr OperandUses usesThatDeclareNoCodeOfOperandsForEffects {
    OperandUse::Inspects, OperandUse::Retains, OperandUse::Returns, OperandUse::ReadsContents, OperandUse::WritesContents, OperandUse::RetainsContents,
    OperandUse::MovesElements, OperandUse::StoresByKey, OperandUse::DeletesByKey, OperandUse::AssignsFrom, OperandUse::DefinesName, OperandUse::TakesDescriptor,
    OperandUse::ChangesIntegrity, OperandUse::ChangesPrototype, OperandUse::BecomesPrototype, OperandUse::AppliesTransitively, OperandUse::AppliesToContents,
};
static_assert(everyOperandUseIsInExactlyOneOf({ usesThatCallOperand, OperandUse::PassesToUserCode, usesThatDeclareNoCodeOfOperandsForEffects }));

static constexpr OpcodeSet opcodesThatMakeThisForEffects { Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList };
static constexpr Effects effectsOfMakingThisOfConstruction = [] {
    Effects effects = declaredBoundOf(Opcode::GetConstructPrototype);
    effects.merge(declaredBoundOf(Opcode::NewObject));
    return effects;
}();

static_assert([] {
    bool liesInsideEveryBound = true;
    opcodesThatMakeThisForEffects.forEach([&](Opcode opcode) {
        liesInsideEveryBound &= effectRuleOf(opcode) == EffectRule::Call && effectsOfMakingThisOfConstruction.isSubsetOf(declaredBoundOf(opcode));
    });
    return liesInsideEveryBound;
}());

static constexpr Effects effectsOfIteratingString = [] {
    Effects effects = declaredBoundOf(NamedEffectSet::CreatesString);
    effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}();

static constexpr OpcodeSet opcodesWithEffectsOfResolvingRopesAlone = [] {
    OpcodeSet opcodes;
    Effects named = declaredBoundOf(NamedEffectSet::ResolvesRopes);
    for (Opcode opcode : allMembersOf<Opcode>) {
        Effects bound = declaredBoundOf(opcode);
        if (effectRuleOf(opcode) != EffectRule::Declared || !bound.mayThrow)
            continue;
        named.mayThrow = bound.mayThrow;
        if (bound == named)
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr OpcodeSet opcodesWithEffectsFittedToOperands = [] {
    OpcodeSet opcodes = opcodesWithEffectsOfResolvingRopesAlone;
    if (effectRuleOf(Opcode::Strcat) == EffectRule::Declared)
        opcodes.add(Opcode::Strcat);
    if (effectRuleOf(Opcode::GetArrayElement) == EffectRule::Declared)
        opcodes.add(Opcode::GetArrayElement);
    opcodes.add(Opcode::ElemDefine);
    opcodes.add(Opcode::DefineOwnProperty);
    if (effectRuleOf(Opcode::NewArrayLike) == EffectRule::Declared)
        opcodes.add(Opcode::NewArrayLike);
    if (effectRuleOf(Opcode::PutArrayElement) == EffectRule::Declared)
        opcodes.add(Opcode::PutArrayElement);
    opcodes.add(Opcode::PutContiguousIndex);
    opcodes.add(Opcode::ModuleNamespace);
    return opcodes;
}();

static constexpr AbstractHeapSet heapsThatReadInsideListLeavesAloneForEffects { AbstractHeap::StringStorage, AbstractHeap::AllocatorState, AbstractHeap::NamedProperties };

static constexpr OpcodeSet opcodesUnderRuleOfOperators = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (effectRuleOf(opcode) == EffectRule::Operator)
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr OpcodeSet operatorsWithTwoOperands = [] {
    OpcodeSet opcodes;
    opcodesUnderRuleOfOperators.forEach([&](Opcode opcode) {
        if (numberOfNamedOperandsOf(opcode) == 2)
            opcodes.add(opcode);
    });
    return opcodes;
}();

static constexpr OpcodeSet operatorsWithOneOrTwoOperands = [] {
    OpcodeSet opcodes;
    opcodesUnderRuleOfOperators.forEach([&](Opcode opcode) {
        uint32_t numberOfOperands = numberOfNamedOperandsOf(opcode);
        if (numberOfOperands && numberOfOperands <= 2 && !hasVariadicOperand(opcode) && (opcode != Opcode::Eq || numberOfOperands == 2))
            opcodes.add(opcode);
    });
    return opcodes;
}();

static constexpr OpcodeSet opcodesWithBoundInsideThatOfGenericOperation = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        std::optional<Opcode> generic = genericOperationOf(opcode);
        if (!generic)
            continue;
        Effects united = declaredBoundOf(*generic);
        united.merge(declaredBoundOf(opcode));
        if (united == declaredBoundOf(*generic))
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr bool hasEffectsOfRowWhereOperandsRunNoCode(Opcode opcode)
{
    return effectsOfEveryOperation.opcodes.contains(opcode)
        && opcode != Opcode::IsNotFrozen
        && !genericOperationOf(opcode)
        && !opcodesUnderRuleOfOperators.contains(opcode)
        && !opcodesWithEffectsFittedToOperands.contains(opcode);
}

static constexpr OpcodeSet opcodesWithEffectsOfRowWhateverOperandsAre = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (!hasEffectsOfRowWhereOperandsRunNoCode(opcode))
            continue;
        const Effects& own = effectsOfEveryOperationWith(opcode);
        Effects withUnknownCode = own;
        withUnknownCode.merge(Effects::unknownCode());
        if (own.maySuspend ? withUnknownCode == own : own.mayCallUnknownCode)
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr OpcodeSet opcodesWithEffectsOfRowUnlessOperandsRunCode = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (hasEffectsOfRowWhereOperandsRunNoCode(opcode) && !effectsOfEveryOperationWith(opcode).mayRunOtherCode())
            opcodes.add(opcode);
    }
    return opcodes;
}();

static constexpr OpcodeSet opcodesWithPureRow = [] {
    OpcodeSet opcodes;
    effectsOfEveryOperation.opcodes.forEach([&](Opcode opcode) {
        if (effectsOfEveryOperationWith(opcode) == Effects::none())
            opcodes.add(opcode);
    });
    return opcodes;
}();

static TypeID typeOrAnythingForEffects(TypeID type)
{
    return type ? type : TypeTable::anything;
}

static bool mayBeCellForEffects(const TypingContext& context, TypeID type)
{
    if (!type)
        return true;
    if (holdsOnlyPrimitives(type))
        return primitivesInID(type) & cellPrimitives;
    return context.at(type).hasCells();
}

static constexpr uint16_t primitivesWithMembersForProtocols = Primitive::Defined & ~(Primitive::UndefinedOrNull | Primitive::Absent);

static constexpr uint16_t primitiveReceiversOfProtocol(Protocol protocol)
{
    switch (protocol) {
    case Protocol::Iterate:
    case Protocol::Named:
    case Protocol::CallsNamed:
        return primitivesWithMembersForProtocols;
    case Protocol::ToJSON:
        return Primitive::BigInt;
    case Protocol::OwnKeys:
    case Protocol::OwnValues:
    case Protocol::ToPrimitive:
    case Protocol::Thenable:
    case Protocol::Species:
    case Protocol::HasInstance:
    case Protocol::PrototypeChain:
        return 0;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static OperandSeenByProtocols operandSeenByProtocols(const TypingContext& context, TypeID operand)
{
    if (holdsOnlyPrimitives(operand)) {
        uint16_t primitives = primitivesInID(operand);
        return { primitives, !!(primitives & Primitive::Unspecified) };
    }
    const Type& type = context.types.recordOf(operand);
    return { type.primitives, type.hasObjects() || !type.isJavaScriptValue() || type.mayBeUnspecified() };
}

static CodeRunByProtocols codeRunByProtocolsOn(Protocols protocols, OperandSeenByProtocols operand)
{
    if (protocols.isEmpty())
        return CodeRunByProtocols::None;
    if (operand.mayBeMoreThanPrimitive)
        return CodeRunByProtocols::Unknown;
    CodeRunByProtocols code = CodeRunByProtocols::None;
    for (Protocol protocol : protocols) {
        uint16_t receivers = operand.primitives & primitiveReceiversOfProtocol(protocol);
        if (!receivers)
            continue;
        if (protocol != Protocol::Iterate || receivers & ~Primitive::AnyString)
            return CodeRunByProtocols::Unknown;
        code = CodeRunByProtocols::IterationOfStrings;
    }
    return code;
}

static bool mayBeFunctionUnderProtocol(const TypingContext& context, TypeID operand)
{
    if (holdsOnlyPrimitives(operand))
        return primitivesInID(operand) & Primitive::Unspecified;
    Type type = context.at(operand);
    return !type.isJavaScriptValue() || type.mayBeUnspecified() || type.mayBeFunction() || mayContainAnyCallable(context.types, type);
}

template<typename Entry>
static constexpr bool declaresProtocolOrUseOfOperand(Entry entry, OperandUses uses)
{
    return !protocolsOfOperandsOf(entry).isEmpty() || usesOfOperandsOf(entry).containsAny(uses);
}

namespace {

struct OpcodesByWhatOperandsDeclare {
    OpcodeSet declareCodeOfOperands;
    OpcodeSet haveOperandsThatMayRunCode;
    OpcodeSet haveEffectsOfRow;
};

} // anonymous namespace

static const OpcodesByWhatOperandsDeclare& opcodesByWhatOperandsDeclare()
{
    static constexpr OpcodesByWhatOperandsDeclare sorted = [] {
        constexpr OperandUses usesThatCall { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::PassesToUserCode };
        static_assert(everyOperandUseIsInExactlyOneOf({ usesThatCall, usesThatDeclareNoCodeOfOperandsForEffects }));
        OpcodesByWhatOperandsDeclare result;
        result.haveEffectsOfRow = opcodesWithEffectsOfRowWhateverOperandsAre;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (declaresProtocolOrUseOfOperand(opcode, usesThatCall))
                result.haveOperandsThatMayRunCode.add(opcode);
            if (declaresProtocolOrUseOfOperand(opcode, usesThatCallOperand))
                result.declareCodeOfOperands.add(opcode);
            else if (opcodesWithEffectsOfRowUnlessOperandsRunCode.contains(opcode))
                result.haveEffectsOfRow.add(opcode);
        }
        return result;
    }();
    return sorted;
}

static const TerminatorKindSet& terminatorKindsDeclaringCodeOfOperands()
{
    static constexpr TerminatorKindSet kinds = [] {
        TerminatorKindSet result;
        for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
            if (declaresProtocolOrUseOfOperand(kind, usesThatCallOperand))
                result.add(kind);
        }
        return result;
    }();
    return kinds;
}

bool operandsMayRunCode(Opcode opcode)
{
    return opcodesByWhatOperandsDeclare().haveOperandsThatMayRunCode.contains(opcode);
}

bool letsNothingElseRun(Opcode opcode)
{
    const Effects& bound = declaredBoundInTable(opcode);
    return bound.alwaysCompletes() && !bound.mayThrow && !operandsMayRunCode(opcode);
}

Effects effectsOfProtocolsOn(const TypingContext& context, OptionSet<Protocol> protocols, TypeID operand)
{
    switch (codeRunByProtocolsOn(protocols, operandSeenByProtocols(context, typeOrAnythingForEffects(operand)))) {
    case CodeRunByProtocols::None:
        return Effects::none();
    case CodeRunByProtocols::IterationOfStrings:
        return effectsOfIteratingString;
    case CodeRunByProtocols::Unknown:
        return Effects::unknownCode();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct CodeRunByOperands {
    bool isUnknown { false };
    bool iteratesStrings { false };
    bool mayCreatePrototypeProperty { false };

    void add(const TypingContext& context, Protocols protocols, OperandUses uses, TypeID operand, bool looksForFunctions)
    {
        bool isCalled = uses.containsAny(usesThatCallOperand);
        if (protocols.isEmpty() && !isCalled)
            return;
        operand = typeOrAnythingForEffects(operand);
        OperandSeenByProtocols seen = operandSeenByProtocols(context, operand);
        isUnknown |= isCalled && seen.mayBeMoreThanPrimitive;
        switch (codeRunByProtocolsOn(protocols, seen)) {
        case CodeRunByProtocols::None:
            return;
        case CodeRunByProtocols::IterationOfStrings:
            iteratesStrings = true;
            return;
        case CodeRunByProtocols::Unknown:
            isUnknown = true;
            mayCreatePrototypeProperty |= looksForFunctions && mayBeFunctionUnderProtocol(context, operand);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
};

} // anonymous namespace

static NEVER_INLINE void cutBoundByWhatCallerFoundOrCodeRuns(Effects& effects, const CodeRunByOperands& code, const Effects* foundByCaller)
{
    if (!foundByCaller) {
        if (code.isUnknown)
            effects.merge(Effects::unknownCode());
        else if (code.iteratesStrings)
            effects.merge(effectsOfIteratingString);
        return;
    }

    uint64_t ownWritesOfGuards = effects.mayCallUnknownCode ? 0 : effects.writes.maskOf(AbstractHeap::MemberGuards);
    if (code.isUnknown)
        effects.merge(Effects::unknownCode());
    else if (code.iteratesStrings)
        effects.merge(effectsOfIteratingString);
    effects.intersect(*foundByCaller);
    effects.writes.add(AbstractHeap::MemberGuards, ownWritesOfGuards);
    if (code.mayCreatePrototypeProperty)
        effects.writes.add(AbstractHeap::MemberGuards);
}

static ALWAYS_INLINE void cutBoundByWhatCallerFound(Effects& effects, const CodeRunByOperands& code, const Effects* foundByCaller)
{
    if (foundByCaller || code.isUnknown || code.iteratesStrings)
        cutBoundByWhatCallerFoundOrCodeRuns(effects, code, foundByCaller);
}

static bool declaresCodeOfOperands(Opcode opcode)
{
    return opcodesByWhatOperandsDeclare().declareCodeOfOperands.contains(opcode);
}

static bool declaresCodeOfOperands(TerminatorKind kind)
{
    return terminatorKindsDeclaringCodeOfOperands().contains(kind);
}

template<typename Entry, typename Holder>
static ALWAYS_INLINE void addCodeOfOperands(const TypingContext& context, Entry entry, const Holder& holder, OperandTypes operandTypes, Effects& effects, const Effects* foundByCaller)
{
    if (effects.maySuspend) {
        effects.merge(Effects::unknownCode());
        return;
    }

    CodeRunByOperands code;
    if (!effects.mayCallUnknownCode && declaresCodeOfOperands(entry)) {
        holder.forEachOperand([&](auto, uint32_t operandIndex) {
            const OperandInfo& info = operandInfoOf(entry, operandIndex);
            code.add(context, info.protocols, info.uses, operandTypes[operandIndex], !!foundByCaller);
        });
    }
    cutBoundByWhatCallerFound(effects, code, foundByCaller);
}

namespace {

struct OperandOfOperatorForEffects {
    OperandOfOperatorForEffects() = default;
    OperandOfOperatorForEffects(const TypingContext& context, TypeID type)
        : record(holdsOnlyPrimitives(type) ? nullptr : &context.types.recordOf(type))
        , primitives(record ? record->primitives : primitivesInID(type))
        , hasObjects(record && record->hasObjects())
        , isJavaScriptValue(!record || record->isJavaScriptValue())
    {
    }

    bool isAnyString() const { return record ? record->isAnyString() : primitives && !(primitives & ~Primitive::AnyString); }

    const Type* record { nullptr };
    uint16_t primitives { 0 };
    bool hasObjects { false };
    bool isJavaScriptValue { true };
};

} // anonymous namespace

static bool mayBeConvertedByLooseEqualityForEffects(const OperandOfOperatorForEffects& operand, const OperandOfOperatorForEffects& other)
{
    bool isMoreThanNullish = operand.primitives & ~Primitive::UndefinedOrNull || operand.hasObjects || !operand.isJavaScriptValue;
    bool isComparedWithPrimitive = other.primitives & ~Primitive::UndefinedOrNull || !other.isJavaScriptValue;
    return isMoreThanNullish && isComparedWithPrimitive;
}

static Effects effectsOfOperatorWithin(const TypingContext& context, Opcode opcode, std::span<const TypeID> operandTypes, TypeID result, const Effects* foundByCaller)
{
    RELEASE_ASSERT(operatorsWithOneOrTwoOperands.contains(opcode));
    uint32_t numberOfOperands = operatorsWithTwoOperands.contains(opcode) ? 2 : 1;
    std::array<TypeID, 2> typeIDs;

    bool makesNoCell = !mayBeCellForEffects(context, result);
    bool areAllNumberLike = true;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        typeIDs[operandIndex] = typeOrAnythingForEffects(operandIndex < operandTypes.size() ? operandTypes[operandIndex] : TypeID());
        areAllNumberLike &= hasOnlyPrimitivesIn(typeIDs[operandIndex], Primitive::NumberLike);
    }
    if (areAllNumberLike && makesNoCell)
        return Effects::none();

    std::array<OperandOfOperatorForEffects, 2> operands;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        operands[operandIndex] = OperandOfOperatorForEffects(context, typeIDs[operandIndex]);

    CodeRunByOperands code;
    TextsJoinedByOperation texts;
    bool mayMakeText = opcode == Opcode::ToString || opcode == Opcode::Add;
    bool areAllInert = true;
    bool mayAllBeStrings = true;
    bool hasConvertedOperand = false;
    bool hasStringOperand = false;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        const OperandOfOperatorForEffects& operand = operands[operandIndex];
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        Protocols protocols = info.protocols;
        if (opcode == Opcode::Eq && !mayBeConvertedByLooseEqualityForEffects(operand, operands[1 - operandIndex]))
            protocols = { };
        bool isConverted = !protocols.isEmpty();
        if (isConverted && operand.hasObjects && !foundByCaller) {
            Effects bound = declaredBoundOf(opcode);
            RELEASE_ASSERT(!bound.mayRunOtherCode());
            bound.merge(Effects::unknownCode());
            return bound;
        }
        bool hasOnlyInertPrimitives = !(operand.primitives & ~Primitive::NumberLike) && operand.isJavaScriptValue;
        areAllInert &= hasOnlyInertPrimitives && !(isConverted && operand.hasObjects);
        mayAllBeStrings &= !!(operand.primitives & Primitive::AnyString);
        hasConvertedOperand |= isConverted;
        hasStringOperand |= operand.isAnyString();
        if (mayMakeText && operand.record)
            texts.add(context.types, *operand.record);
        else if (mayMakeText)
            texts.add(context.types, context.at(typeIDs[operandIndex]));
        code.add(context, protocols, info.uses, typeIDs[operandIndex], !!foundByCaller);
    }

    if (areAllInert && makesNoCell)
        return Effects::none();
    bool comparesWithoutConversion = provesOf(opcode) == Proof::Equality && !hasConvertedOperand;
    if (comparesWithoutConversion && !mayAllBeStrings)
        return Effects::none();

    Effects own = declaredBoundOf(opcode);
    RELEASE_ASSERT(!own.mayRunOtherCode());
    bool makesTextThatFitsInString = mayMakeText && texts.fitInString() && (opcode == Opcode::ToString || hasStringOperand);
    if (own.mayThrow && (makesTextThatFitsInString || comparesWithoutConversion))
        own.mayThrow = ThrowEffect::onlyForLackOfMemory();
    cutBoundByWhatCallerFound(own, code, foundByCaller);
    return own;
}

Effects effectsOfOperator(const TypingContext& context, Opcode opcode, std::span<const TypeID> operandTypes, TypeID result)
{
    return effectsOfOperatorWithin(context, opcode, operandTypes, result, nullptr);
}

template<typename Holder>
static bool readsOnlyFlatStringsForEffects(const TypingContext& context, Opcode opcode, const Holder& operation, OperandTypes operandTypes)
{
    bool readsString = false;
    bool mayReadRope = false;
    operation.forEachOperand([&](auto, uint32_t operandIndex) {
        if (operandKindOf(opcode, operandIndex) != OperandKind::String)
            return;
        readsString = true;
        TypeID type = operandTypes[operandIndex];
        mayReadRope |= !type || !context.at(type).isOpaque(OpaqueKind::FlatString);
    });
    return readsString && !mayReadRope;
}

template<typename Holder>
static bool joinsTextsThatFitForEffects(const TypingContext& context, const Holder& operation, OperandTypes operandTypes)
{
    TextsJoinedByOperation texts;
    operation.forEachOperand([&](auto, uint32_t operandIndex) {
        texts.add(context.types, context.at(typeOrAnythingForEffects(operandTypes[operandIndex])));
    });
    return texts.fitInString();
}

template<typename Holder>
static ALWAYS_INLINE Effects declaredEffectsBuiltInPlace(const Program& program, Opcode opcode, const Holder& operation)
{
    if (effectsOfEveryOperation.opcodes.contains(opcode)) {
        Effects effects = effectsOfEveryOperationWith(opcode);
        if (lookAtCellHasNoFlagThatReads(opcode, operation))
            effects.reads.remove(heapsThatOnlyFlagsOfLooksAtCellRead);
        return effects;
    }
    Effects effects = Effects::none();
    EffectsBuilderInPlace builder(program, effects);
    forEachDeclaredEffect(operation, builder);
    if (dependsOnBufferVariables(opcodeOf(operation), operation))
        effects.reads.add(AbstractHeap::ModuleVariables);
    if (lookAtCellHasNoFlagThatReads(opcode, operation))
        effects.reads.remove(heapsThatOnlyFlagsOfLooksAtCellRead);
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

template<typename Holder>
static bool readsInsideListAlone(const Holder& read)
{
    return (read.isInBounds() || read.isPresent()) && !read.asksCellWhereNothingIsFound();
}

template<typename Holder>
static bool readInsideListOnlyLoadsForEffects(const TypingContext& context, const Holder& read, TypeID result)
{
    if (!readsInsideListAlone(read))
        return false;
    ArrayStorage storage = read.arrayStorage();
    if (storage == ArrayStorage::Contiguous || storage == ArrayStorage::Int32)
        return true;
    return storage == ArrayStorage::Double && result && context.types.intersectionOf(result, TypeTable::number) == TypeID(TypeTable::number);
}

static bool lengthIsThatOfListForEffects(const TypingContext& context, OperandTypes operandTypes)
{
    TypeID length = operandTypes[OperandIndex::NewArrayLike::length];
    if (!length || !context.at(length).isInteger())
        return false;
    Range lengths = context.at(length).integers;
    return lengths.minimum >= 0 && lengths.maximum <= static_cast<int64_t>(UINT32_MAX);
}

static bool definesElementOfListForEffects(const TypingContext& context, OperandTypes operandTypes)
{
    TypeID object = operandTypes[OperandIndex::ElemDefine::object];
    TypeID key = operandTypes[OperandIndex::ElemDefine::property];
    if (!object || !key || !context.at(object).isObject() || !context.at(key).isNumber())
        return false;
    const ClassPart* onlyClass = singleBuiltinClassOf(context.types, context.at(object));
    return onlyClass && onlyClass->builtinClass == BuiltinClasses::Array;
}

template<typename Operation>
bool namespaceReadRunsNoCode(const Program& program, const Operation& read)
{
    constexpr OptionSet<ModuleFlags> excludedFlags { ModuleFlags::MayBeReevaluated, ModuleFlags::MayBeMocked, ModuleFlags::MayBeEvicted, ModuleFlags::IsLoadedByLoader };
    if (read.ofModuleItself() || read.asksAnew() || read.phase() != ImportPhase::Evaluation)
        return false;
    const Module& module = program.at(read.module());
    if (module.kind != ModuleKind::SourceText || module.exportRule != SyntheticExportRule::None || module.flags.containsAny(excludedFlags))
        return false;
    return std::ranges::none_of(module.requests, [](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
}

template<typename Holder>
static Effects declaredEffectsWithOperands(const TypingContext& context, Opcode opcode, const Holder& operation, OperandTypes operandTypes, TypeID result)
{
    if (opcode == Opcode::ModuleNamespace && namespaceReadRunsNoCode(context.program, operation))
        return Effects::ofReadByNameThatRunsNoCode();
    if (opcode == Opcode::ElemDefine && definesElementOfListForEffects(context, operandTypes)) {
        Effects effects = declaredBoundOf(Opcode::PutArrayElement);
        effects.writes.add(perObjectHeaps);
        effects.mayThrow = true;
        return effects;
    }
    Effects effects = declaredEffectsBuiltInPlace(context.program, opcode, operation);
    if (opcodesWithEffectsOfResolvingRopesAlone.contains(opcode) && readsOnlyFlatStringsForEffects(context, opcode, operation, operandTypes)) {
        LocationMasks reads = effects.reads;
        effects = Effects::none();
        effects.reads = reads;
    }
    if (opcode == Opcode::NewArrayLike && effects.mayThrow && lengthIsThatOfListForEffects(context, operandTypes))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    if (opcode == Opcode::PutContiguousIndex && operation.hasNumericStorage()) {
        uint64_t written = effects.writes.maskOf(AbstractHeap::Elements) & ~cellHoldingElementsMask;
        effects.writes.remove(AbstractHeapSet { AbstractHeap::Elements });
        effects.writes.add(AbstractHeap::Elements, written);
    }
    if (opcode == Opcode::PutArrayElement && effects.mayThrow && operation.fillsPlaceOfListMadeHere())
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    if (opcode == Opcode::Strcat && effects.mayThrow && joinsTextsThatFitForEffects(context, operation, operandTypes))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    if (opcode == Opcode::GetArrayElement && readsInsideListAlone(operation)) {
        bool onlyLoads = readInsideListOnlyLoadsForEffects(context, operation, result);
        if (onlyLoads || (operation.arrayStorage() == ArrayStorage::Double && result)) {
            LocationMasks reads = effects.reads;
            reads.remove(heapsThatReadInsideListLeavesAloneForEffects);
            effects = Effects::none();
            effects.reads = reads;
            if (!onlyLoads)
                effects.mayThrow = ThrowEffect::stopsProgram();
        }
    }
    return effects;
}

static bool definesOnLayoutObjects(const TypingContext& context, OperandTypes operandTypes)
{
    TypeID object = operandTypes[OperandIndex::DefineOwnProperty::object];
    return object && context.at(object).isObject() && context.at(object).hasOnlyLayouts();
}

template<typename Operation>
static NEVER_INLINE Effects effectsThatHangOnOperandsOf(const TypingContext& context, const Operation& operation, OperandTypes operandTypes, TypeID result, const Effects* foundByCaller)
{
    Opcode opcode = opcodeOf(operation);
    if (testsItsOwnGuess(operation)) {
        Effects bound = declaredBoundOf(*genericOperationOf(opcode));
        if (!opcodesWithBoundInsideThatOfGenericOperation.contains(opcode))
            bound.merge(declaredEffectsBuiltInPlace(context.program, opcode, operation));
        if (foundByCaller)
            bound.intersect(*foundByCaller);
        return bound;
    }

    if (opcodesUnderRuleOfOperators.contains(opcode))
        return effectsOfOperatorWithin(context, opcode, operandTypes.types, result, foundByCaller);

    bool areFittedToOperands = opcodesWithEffectsFittedToOperands.contains(opcode);
    Effects effects = areFittedToOperands ? declaredEffectsWithOperands(context, opcode, operation, operandTypes, result) : declaredEffectsBuiltInPlace(context.program, opcode, operation);
    if (areFittedToOperands && opcode == Opcode::DefineOwnProperty && definesOnLayoutObjects(context, operandTypes))
        cutBoundByWhatCallerFound(effects, CodeRunByOperands { }, foundByCaller);
    else
        addCodeOfOperands(context, opcode, operation, operandTypes, effects, foundByCaller);
    if (foundByCaller && opcodesThatMakeThisForEffects.contains(opcode))
        effects.merge(effectsOfMakingThisOfConstruction);
    if (foundByCaller && opcode == Opcode::GetBuiltinValue)
        effects.intersect(*foundByCaller);
    return effects;
}

template<typename Operation>
Effects effectsOf(const TypingContext& context, const Operation& operation, OperandTypes operandTypes, TypeID result, const Effects* foundByCaller)
{
    Opcode opcode = opcodeOf(operation);
    if (!foundByCaller && opcodesByWhatOperandsDeclare().haveEffectsOfRow.contains(opcode)) {
        if (opcodesWithPureRow.contains(opcode))
            return Effects::none();
        return effectsOfEveryOperationWith(opcode);
    }
    return effectsThatHangOnOperandsOf(context, operation, operandTypes, result, foundByCaller);
}

template<typename Terminator>
Effects effectsOfTerminator(const TypingContext& context, const Terminator& terminator, OperandTypes operandTypes, const Effects* foundByCaller)
{
    Effects effects = Effects::none();
    EffectsBuilderInPlace builder(context.program, effects);
    forEachDeclaredEffect(terminator, builder);
    addCodeOfOperands(context, terminatorKindOf(terminator), terminator, operandTypes, effects, foundByCaller);
    return effects;
}

template bool namespaceReadRunsNoCode<BodyValue>(const Program&, const BodyValue&);
template Effects effectsOf<Draft>(const TypingContext&, const Draft&, OperandTypes, TypeID, const Effects*);
template Effects effectsOf<Operation>(const TypingContext&, const Operation&, OperandTypes, TypeID, const Effects*);
template Effects effectsOf<BodyValue>(const TypingContext&, const BodyValue&, OperandTypes, TypeID, const Effects*);
template Effects effectsOfTerminator<TerminatorDraft>(const TypingContext&, const TerminatorDraft&, OperandTypes, const Effects*);
template Effects effectsOfTerminator<Terminator>(const TypingContext&, const Terminator&, OperandTypes, const Effects*);
template Effects effectsOfTerminator<BodyTerminator>(const TypingContext&, const BodyTerminator&, OperandTypes, const Effects*);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
