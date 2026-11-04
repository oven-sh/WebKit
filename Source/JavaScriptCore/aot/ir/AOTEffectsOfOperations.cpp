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
// It does what the first allocation of the class does. So whoever counts a construction has counted it: the pre-scans of loops, and Facts.
static_assert(declaredBoundOf(Opcode::BeginMemberGuards).writes.isSubsetOf(declaredBoundOf(Opcode::NewObject).writes) && declaredBoundOf(Opcode::NewObject).mayCollect);

bool hasTextByItself(const Type& type)
{
    return !type.isNever() && type.hasOnlyPrimitives(Primitive::NumberLike | Primitive::AnyString | Primitive::BigInt);
}

// A string of the program is held in no fewer bytes than it has characters.
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
    // An object, or a word of which the type does not say what it holds.
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
    bool areAllTheBound = true;
    effectsOfEveryOperation.opcodesNamingOnlyEntireHeaps.forEach([&](Opcode opcode) {
        areAllTheBound &= effectsOfEveryOperationWith(opcode) == declaredBoundOf(opcode);
    });
    return areAllTheBound;
}());

} // anonymous namespace

static constexpr OperandUses usesThatCallOperandForEffects { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly };

// In front of the body that it enters, a construction by value reads `prototype` off new.target and makes `this`. What is known of a callee speaks of its body alone.
static constexpr OpcodeSet opcodesThatMakeThisForEffects { Opcode::Construct, Opcode::ConstructList, Opcode::SuperConstruct, Opcode::SuperConstructList, Opcode::ConstructSuperForwardVarargs };
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

// String.prototype[Symbol.iterator] and the next() of its iterators are read-only, so no code of the program runs. The iterator and each character are made.
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
    for (Opcode opcode : { Opcode::Decode, Opcode::Strcat }) {
        if (effectRuleOf(opcode) == EffectRule::Declared)
            opcodes.add(opcode);
    }
    return opcodes;
}();

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

// The primitives on which the protocol looks a member up. A program may add members to the prototypes of numbers, booleans, symbols and BigInts: Number.prototype[Symbol.iterator], BigInt.prototype.toJSON.
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
    Type type = context.at(operand);
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
static bool declaresProtocolOrUseOfOperand(Entry entry, OperandUses uses)
{
    uint32_t numberOfDeclaredOperands = numberOfNamedOperandsOf(entry) + (hasVariadicOperand(entry) ? 1 : 0);
    for (uint32_t operandIndex = 0; operandIndex < numberOfDeclaredOperands; ++operandIndex) {
        const OperandInfo& info = operandInfoOf(entry, operandIndex);
        if (!info.protocols.isEmpty() || info.uses.containsAny(uses))
            return true;
    }
    return false;
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
    static const OpcodesByWhatOperandsDeclare sorted = [] {
        constexpr OperandUses usesThatCall { OperandUse::Calls, OperandUse::CallsPerElement, OperandUse::CallsRepeatedly, OperandUse::PassesToUserCode };
        OpcodesByWhatOperandsDeclare result;
        result.haveEffectsOfRow = opcodesWithEffectsOfRowWhateverOperandsAre;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (declaresProtocolOrUseOfOperand(opcode, usesThatCall))
                result.haveOperandsThatMayRunCode.add(opcode);
            if (declaresProtocolOrUseOfOperand(opcode, usesThatCallOperandForEffects))
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
    static const TerminatorKindSet kinds = [] {
        TerminatorKindSet result;
        for (TerminatorKind kind : allMembersOf<TerminatorKind>) {
            if (declaresProtocolOrUseOfOperand(kind, usesThatCallOperandForEffects))
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

static ValueID valueBehindWhatOnlyHandsOnForEffects(const BodyIndex& index, ValueID id)
{
    while (!index.isParameter(id)) {
        BodyValue definition = index.value(id);
        if (!inputOperationsThatOnlyHandOn.contains(definition.opcode()) || !definition.operand(0))
            break;
        id = definition.operand(0);
    }
    return id;
}

// A definition of a value on an object that nobody else has seen calls nothing and is never refused.
static uint32_t numberOfStaticFieldsThatInitializerDefinesFirst(const Layout& layout, const BodyIndex& initializer, ValueID classObject)
{
    uint32_t numberDefined = 0;
    BodyBlock block = initializer.block(BlockID(0u));
    for (uint32_t unvisitedBlocks = initializer.numberOfBlocks(); unvisitedBlocks; --unvisitedBlocks) {
        for (BodyValue operation : block.values()) {
            Opcode opcode = operation.opcode();
            if (letsNothingElseRun(opcode))
                continue;
            if (opcode != Opcode::PropDefine || numberDefined == layout.staticFields.size())
                return numberDefined;
            auto definition = operation.as<Opcode::PropDefine>();
            PropertyKeyID key = layout.staticFields[numberDefined].key;
            if (!key || definition.key() != key || valueBehindWhatOnlyHandsOnForEffects(initializer, definition.objectOperand()) != classObject)
                return numberDefined;
            ++numberDefined;
        }
        BodyTerminator terminator = block.terminator();
        if (terminator.kind() != TerminatorKind::Jump)
            return numberDefined;
        BlockID next = terminator.next().block();
        if (initializer.numberOfIncomingEdges(next) != 1 || initializer.isEnteredByHandlerEdge(next))
            return numberDefined;
        block = initializer.block(next);
    }
    return numberDefined;
}

uint32_t numberOfStaticFieldsDefinedBeforeOtherCodeRuns(const Program& program, const BodyIndexTable& bodies, const BodyIndex& definer, ValueID newClass)
{
    const Layout& layout = program.at(definer.value(newClass).as<Opcode::NewClass>().layout());
    if (layout.staticFields.isEmpty())
        return 0;

    auto isClassObject = [&](ValueID value) {
        return valueBehindWhatOnlyHandsOnForEffects(definer, value) == newClass;
    };
    uint32_t endOfBlock = definer.block(definer.blockOf(newClass)).endOfValues().index();
    for (uint32_t number = newClass.index() + 1; number < endOfBlock; ++number) {
        BodyValue operation = definer.value(ValueID(number));
        Opcode opcode = operation.opcode();
        if (letsNothingElseRun(opcode))
            continue;
        // Under a written key it lays a member on a holder that nobody else has seen. `static ["prototype"]() {}` throws.
        if (opcode == Opcode::DefineMethod) {
            auto method = operation.as<Opcode::DefineMethod>();
            if (!method.key() || !isClassObject(method.classObjectOperand()))
                return 0;
            continue;
        }
        if (opcode != Opcode::CallDirect)
            return 0;

        auto call = operation.as<Opcode::CallDirect>();
        const Function& callee = program.at(call.function());
        const BodyIndex* initializer = bodies.find(call.function());
        std::optional<unsigned> thisParameter = callee.thisParameterIndex();
        std::span<const ValueID> arguments = call.argumentsOperands();
        if (callee.kind != FunctionKind::Initializer || !initializer || !thisParameter || *thisParameter >= arguments.size() || !isClassObject(arguments[*thisParameter]))
            return 0;
        BodyBlock entry = initializer->block(BlockID(0u));
        if (*thisParameter >= entry.numberOfParameters())
            return 0;
        return numberOfStaticFieldsThatInitializerDefinesFirst(layout, *initializer, entry.parameter(*thisParameter));
    }
    return 0;
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

// Gathered operand by operand, and turned into effects once.
struct CodeRunByOperands {
    bool isUnknown { false };
    bool iteratesStrings { false };
    bool mayCreatePrototypeProperty { false };

    void add(const TypingContext& context, Protocols protocols, OperandUses uses, TypeID operand, bool looksForFunctions)
    {
        bool isCalled = uses.containsAny(usesThatCallOperandForEffects);
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

// The first object of a constructor function ends a set of guards, and so does the prototype property that a function gets when it is first asked for.
// Whoever resolves members or callees sees neither, so no caller takes those writes away.
// A row of unknown code declares every write and so names none that is its own: effectsOf() keeps what a construction does by itself.
// `effects` comes in as the operation's own.
static void cutBoundByWhatCallerFound(Effects& effects, const CodeRunByOperands& code, const Effects* foundByCaller)
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

static bool declaresCodeOfOperands(Opcode opcode)
{
    return opcodesByWhatOperandsDeclare().declareCodeOfOperands.contains(opcode);
}

static bool declaresCodeOfOperands(TerminatorKind kind)
{
    return terminatorKindsDeclaringCodeOfOperands().contains(kind);
}

// `effects` comes in as the operation's own.
template<typename Entry, typename Holder>
static ALWAYS_INLINE void addCodeOfOperands(const TypingContext& context, Entry entry, const Holder& holder, OperandTypes operandTypes, Effects& effects, const Effects* foundByCaller)
{
    // Any code may run before a suspended operation goes on, whatever is known of what it calls.
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

// IsLooselyEqual leaves undefined and null as they are, and converts anything else only to compare it with a boolean, number, string, symbol or BigInt.
static bool mayBeConvertedByLooseEqualityForEffects(const Type& operand, const Type& other)
{
    bool isMoreThanNullish = operand.primitives & ~Primitive::UndefinedOrNull || operand.hasObjects() || !operand.isJavaScriptValue();
    bool isComparedWithPrimitive = other.primitives & ~Primitive::UndefinedOrNull || !other.isJavaScriptValue();
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

    std::array<Type, 2> types;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex)
        types[operandIndex] = context.at(typeIDs[operandIndex]);

    CodeRunByOperands code;
    TextsJoinedByOperation texts;
    bool mayMakeText = opcode == Opcode::ToString || opcode == Opcode::Add;
    bool areAllInert = true;
    bool mayAllBeStrings = true;
    bool hasConvertedOperand = false;
    bool hasStringOperand = false;
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        const Type& type = types[operandIndex];
        const OperandInfo& info = operandInfoOf(opcode, operandIndex);
        Protocols protocols = info.protocols;
        if (opcode == Opcode::Eq && !mayBeConvertedByLooseEqualityForEffects(type, types[1 - operandIndex]))
            protocols = { };
        bool isConverted = !protocols.isEmpty();
        bool hasOnlyInertPrimitives = !(type.primitives & ~Primitive::NumberLike) && type.isJavaScriptValue();
        areAllInert &= hasOnlyInertPrimitives && !(isConverted && type.hasObjects());
        mayAllBeStrings &= !!(type.primitives & Primitive::AnyString);
        hasConvertedOperand |= isConverted;
        hasStringOperand |= type.isAnyString();
        if (mayMakeText)
            texts.add(context.types, type);
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

// A narrow reference to a cell names a cell that is there, and decoding it is arithmetic. Only the word of a literal may have to make its string.
static bool decodesWordOfNoStringForEffects(const TypingContext& context, TypeID type)
{
    if (!type)
        return false;
    if (holdsOnlyPrimitives(type))
        return !(primitivesInID(type) & (Primitive::AnyString | Primitive::Unspecified));
    Type decoded = context.at(type);
    return decoded.isJavaScriptValue() && !decoded.mayBeUnspecified() && !(decoded.primitives & Primitive::AnyString) && !decoded.stringLiterals;
}

template<typename Holder>
static ALWAYS_INLINE Effects declaredEffectsBuiltInPlace(const Program& program, Opcode opcode, const Holder& operation)
{
    if (effectsOfEveryOperation.opcodes.contains(opcode)) {
        Effects effects = effectsOfEveryOperationWith(opcode);
        if (opcode == Opcode::IsNotFrozen && !operation.hasNoExtraProperties() && !operation.hasNoFieldOutsideSlot())
            effects.reads.remove(heapsThatOnlyFlagsOfIsNotFrozenRead);
        return effects;
    }
    Effects effects = Effects::none();
    EffectsBuilderInPlace builder(program, effects);
    forEachDeclaredEffect(operation, builder);
    if (effects.mayThrow && opcodesThrowingOnlyForLackOfMemory.contains(opcode))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

template<typename Holder>
static Effects declaredEffectsWithOperands(const TypingContext& context, Opcode opcode, const Holder& operation, OperandTypes operandTypes)
{
    if (opcode == Opcode::Decode && decodesWordOfNoStringForEffects(context, operandTypes[0]))
        return Effects::none();
    Effects effects = declaredEffectsBuiltInPlace(context.program, opcode, operation);
    if (opcodesWithEffectsOfResolvingRopesAlone.contains(opcode) && readsOnlyFlatStringsForEffects(context, opcode, operation, operandTypes)) {
        LocationMasks reads = effects.reads;
        effects = Effects::none();
        effects.reads = reads;
    }
    if (opcode == Opcode::Strcat && effects.mayThrow && joinsTextsThatFitForEffects(context, operation, operandTypes))
        effects.mayThrow = ThrowEffect::onlyForLackOfMemory();
    return effects;
}

// A test of the mark of a cell reads NamedProperties. Whether a field holds a raw value from birth, so that undefined or null in its slot sets the mark, is a fact, which no row declares.
static constexpr OpcodeSet slotStoresThatMaySetMarkForEffects { Opcode::PutSlot, Opcode::PutSlotOrAdd };

template<typename Operation>
Effects effectsOf(const TypingContext& context, const Operation& operation, OperandTypes operandTypes, TypeID result, const Effects* foundByCaller)
{
    Opcode opcode = opcodeOf(operation);
    if (!foundByCaller && opcodesByWhatOperandsDeclare().haveEffectsOfRow.contains(opcode)) {
        if (opcodesWithPureRow.contains(opcode))
            return Effects::none();
        return effectsOfEveryOperationWith(opcode);
    }

    if (testsItsOwnGuess(operation)) {
        Effects bound = declaredBoundOf(*genericOperationOf(opcode));
        if (!opcodesWithBoundInsideThatOfGenericOperation.contains(opcode))
            bound.merge(declaredEffectsBuiltInPlace(context.program, opcode, operation));
        if (foundByCaller)
            bound.intersect(*foundByCaller);
        return bound;
    }
    if (foundByCaller && slotStoresThatMaySetMarkForEffects.contains(opcode) && foundByCaller->writes.contains(AbstractHeap::NamedProperties)) {
        Effects effects = effectsOf(context, operation, operandTypes, result, nullptr);
        effects.writes.add(AbstractHeap::NamedProperties, foundByCaller->writes.maskOf(AbstractHeap::NamedProperties));
        return effects;
    }

    if (opcodesUnderRuleOfOperators.contains(opcode))
        return effectsOfOperatorWithin(context, opcode, operandTypes.types, result, foundByCaller);

    Effects effects = opcodesWithEffectsFittedToOperands.contains(opcode) ? declaredEffectsWithOperands(context, opcode, operation, operandTypes) : declaredEffectsBuiltInPlace(context.program, opcode, operation);
    addCodeOfOperands(context, opcode, operation, operandTypes, effects, foundByCaller);
    if (foundByCaller && opcodesThatMakeThisForEffects.contains(opcode))
        effects.merge(effectsOfMakingThisOfConstruction);
    return effects;
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

template Effects effectsOf<Draft>(const TypingContext&, const Draft&, OperandTypes, TypeID, const Effects*);
template Effects effectsOf<Operation>(const TypingContext&, const Operation&, OperandTypes, TypeID, const Effects*);
template Effects effectsOf<BodyValue>(const TypingContext&, const BodyValue&, OperandTypes, TypeID, const Effects*);
template Effects effectsOfTerminator<TerminatorDraft>(const TypingContext&, const TerminatorDraft&, OperandTypes, const Effects*);
template Effects effectsOfTerminator<Terminator>(const TypingContext&, const Terminator&, OperandTypes, const Effects*);
template Effects effectsOfTerminator<BodyTerminator>(const TypingContext&, const BodyTerminator&, OperandTypes, const Effects*);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
