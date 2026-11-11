/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet readsOfPlacesInNumberExpectations { Opcode::PropGet, Opcode::PrivateGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture };
static constexpr OpcodeSet callsWithResultsInNumberExpectations { Opcode::Call, Opcode::CallList };
static constexpr OpcodeSet sumsFollowedInNumberExpectations { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet operatorsWithZeroAsIdentityInNumberExpectations { Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
static constexpr OpcodeSet operatorsThatMakeFractions { Opcode::Div, Opcode::Pow };
static constexpr OpcodeSet accessesByKeyInNumberExpectations { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);

static constexpr unsigned mostNamesFollowedInNumberExpectations = 16;
static constexpr uint32_t mostOperationsSearchedForMentionsInNumberExpectations = 32;

namespace {

struct MentionInNumberExpectations {
    ValueID user;
    uint32_t operandIndex { 0 };
    ValueID handedOnAs;
};

using NamesInNumberExpectations = Vector<ValueID, mostNamesFollowedInNumberExpectations>;

} // anonymous namespace

template<typename TypeOfOperand>
static TypeID typeByRowInNumberExpectations(const TypingContext& typing, const BodyValue& operation, const TypeOfOperand& typeOfOperand)
{
    Opcode opcode = operation.opcode();
    if (resultRuleOf(opcode) == ResultRule::None || opcode == Opcode::ResultAt)
        return { };
    Draft draft(opcode);
    draft.flags = static_cast<uint8_t>(operation.flags());
    copyFields(operation, draft, [](ValueID operand) {
        return Reference::value(operand);
    });
    if (opcodesThatMayHaveSeveralResults.contains(opcode) && numberOfResultsOf(typing.program, draft))
        return { };

    Vector<TypeID, 8> operandTypes;
    operandTypes.fill(TypeID(), numberOfNamedOperandsOf(opcode) + operation.variadicOperands().size());
    operation.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        operandTypes[operandIndex] = typeOfOperand(operand, operandIndex);
    });
    return resultTypeOf(typing, draft, OperandTypes { operandTypes.span() });
}

static TypeID provenTypeInNumberExpectations(const Walker& walker, ValueID input)
{
    Reference built = input ? walker.referenceOf(input) : Reference();
    return built ? walker.typeOf(built) : TypeID();
}

static bool isUninformativeInNumberExpectations(const TypeArena& types, TypeID type)
{
    if (!type || isNeverType(type))
        return true;
    return !holdsOnlyPrimitives(type) && types.at(type).hasUnboundedUnknown();
}

TypeID typeHeldAtPositionOf(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const LinkTables& held = walker.facts().held();
    FunctionID function = walker.currentFunction();
    ValueID value = index.stripCasts(input);
    while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.stripCasts(index.value(value).operand(0));
    if (!value || function.index() >= held.functions.size())
        return { };

    BodyValue definition = walker.asWalked(index.value(value));
    if (definition.isParameter()) {
        if (index.blockOf(value) != entryBlock)
            return { };
        std::span<const LinkedParameter> parameters = held.parametersOf(function);
        return value.index() < parameters.size() ? parameters[value.index()].proven : TypeID();
    }

    Opcode opcode = definition.opcode();
    if (callsWithResultsInNumberExpectations.contains(opcode)) {
        FunctionID callee = held.calleeAt(function, definition.id());
        if (!callee || callee.index() >= held.functions.size() || walker.program().at(callee).isCoroutine())
            return { };
        return held.function(callee).result;
    }
    if (!readsOfPlacesInNumberExpectations.contains(opcode))
        return { };

    TypeArena& types = walker.types();
    std::optional<uint8_t> placeOfReceiver = receiverOf(opcode);
    TypeID receiver;
    if (placeOfReceiver) {
        ValueID object = definition.operand(*placeOfReceiver);
        receiver = provenTypeInNumberExpectations(walker, object);
        if (isUninformativeInNumberExpectations(types, receiver))
            receiver = object && object < value ? findExpectation(walker, object).type : TypeID();
        if (isUninformativeInNumberExpectations(types, receiver))
            return { };
        receiver = types.withoutNullish(receiver);
    }
    TypingContext byWhatIsHeld { walker.program(), types, walker.heldPositions(), TypeLevel::Proven };
    return typeByRowInNumberExpectations(byWhatIsHeld, definition, [&](ValueID operand, uint32_t operandIndex) -> TypeID {
        if (placeOfReceiver && *placeOfReceiver == operandIndex)
            return receiver;
        TypeID proven = provenTypeInNumberExpectations(walker, operand);
        return proven ? proven : TypeID(TypeTable::anything);
    });
}

template<typename Visit>
static uint32_t visitMentionsInOperationForNumberExpectations(const BodyValue& user, ValueID value, const Visit& visit)
{
    uint32_t found = 0;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operand != value)
            return;
        ++found;
        visit(MentionInNumberExpectations { user.id(), operandIndex, { } });
    });
    return found;
}

template<typename Visit>
static uint32_t visitMentionsAtEndOfBlockForNumberExpectations(const BodyIndex& index, BlockID from, ValueID value, const Visit& visit)
{
    BodyBlock block = index.block(from);
    uint32_t found = 0;
    auto visitMention = [&](ValueID handedOnAs) {
        ++found;
        visit(MentionInNumberExpectations { { }, 0, handedOnAs });
    };

    block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
        if (operand == value)
            visitMention({ });
    });
    block.forEachOutgoingEdge([&](BodyTarget target, bool) {
        BodyBlock taker = index.block(target.block());
        std::span<const ValueID> arguments = target.arguments();
        for (uint32_t position = 0; position < arguments.size(); ++position) {
            if (arguments[position] == value)
                visitMention(position < taker.numberOfParameters() ? taker.parameter(position) : ValueID());
        }
    });
    index.forEachEdgeResult(from, [&](const BodyEdgeResult& result) {
        if (result.subject == value)
            visitMention(result.parameter);
    });
    if (block.subject() == value)
        visitMention({ });
    return found;
}

template<typename Visit>
static bool visitMentionsForNumberExpectations(const BodyIndex& index, ValueID value, const Visit& visit)
{
    uint32_t numberOfMentions = index.useCount(value);
    if (!numberOfMentions)
        return true;
    if (std::optional<BodyUser> user = index.onlyUser(value)) {
        if (user->kind == BodyUseKind::Operand)
            return !!visitMentionsInOperationForNumberExpectations(index.value(user->value), value, visit);
        return !!visitMentionsAtEndOfBlockForNumberExpectations(index, user->block, value, visit);
    }

    BodyBlock block = index.block(index.blockOf(value));
    uint32_t endOfBlock = block.endOfValues().index();
    uint32_t first = std::max(value.index() + 1, block.firstValue().index());
    uint32_t end = std::min(endOfBlock, first + mostOperationsSearchedForMentionsInNumberExpectations);
    uint32_t found = 0;
    for (uint32_t number = first; number < end && found < numberOfMentions; ++number)
        found += visitMentionsInOperationForNumberExpectations(index.value(ValueID(number)), value, visit);
    if (found < numberOfMentions && end == endOfBlock)
        found += visitMentionsAtEndOfBlockForNumberExpectations(index, block.id(), value, visit);
    return found == numberOfMentions && numberOfMentions != BodyIndex::maxCountedUses;
}

static bool isWrittenZeroInNumberExpectations(const BodyIndex& index, ValueID value)
{
    return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::Integer && !index.value(value).as<Opcode::Integer>().integer();
}

static bool takesPositionOrCountInNumberExpectations(BuiltinConversion conversion)
{
    switch (conversion) {
    case BuiltinConversion::ToIntegerOrInfinity:
    case BuiltinConversion::ToLength:
    case BuiltinConversion::ToIndex:
    case BuiltinConversion::ToInt32:
    case BuiltinConversion::ToUint32:
        return true;
    case BuiltinConversion::None:
    case BuiltinConversion::ToBoolean:
    case BuiltinConversion::ToNumber:
    case BuiltinConversion::ToNumeric:
    case BuiltinConversion::ToString:
    case BuiltinConversion::ToPropertyKey:
    case BuiltinConversion::ToPrimitive:
    case BuiltinConversion::ToObject:
    case BuiltinConversion::ToBigInt:
    case BuiltinConversion::Dictionary:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool rowsTakeArgumentAsPositionInNumberExpectations(Walker& walker, const BodyValue& call, uint32_t operandIndex)
{
    if (call.opcode() != Opcode::Call || operandIndex < numberOfNamedOperandsOf(Opcode::Call))
        return false;
    TypeID typeOfCallee = provenTypeInNumberExpectations(walker, call.calleeOperand());
    if (isUninformativeInNumberExpectations(walker.types(), typeOfCallee)) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID read = index.stripCasts(call.calleeOperand());
        typeOfCallee = { };
        if (read && !index.isParameter(read) && index.opcodeOf(read) == Opcode::PropGet && !index.value(read).isFreeName()) {
            TypeID expected = findExpectation(walker, index.value(read).objectOperand()).type;
            if (expected && !isNeverType(walker.types().withoutNullish(expected))) {
                typeOfCallee = typeByRowInNumberExpectations(walker.typing(), index.value(read), [&](ValueID, uint32_t) {
                    return walker.types().withoutNullish(expected);
                });
            }
        }
    }
    if (!typeOfCallee)
        return false;
    TypeArena& types = walker.types();
    Type callee = types.at(typeOfCallee);
    BuiltinID builtin = callee.isObject() ? singleBuiltinOf(types, callee) : BuiltinID();
    if (!builtin)
        return false;

    const BuiltinTable& table = walker.program().builtins();
    unsigned argument = operandIndex - numberOfNamedOperandsOf(Opcode::Call);
    bool someRowTakesIt = false;
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(builtin))) {
        if (overload.mode != BuiltinCallMode::Call)
            continue;
        for (const BuiltinParameter& parameter : overload.parameters) {
            if (!parameter.operand.coversArgument(argument))
                continue;
            if (!takesPositionOrCountInNumberExpectations(parameter.conversion))
                return false;
            someRowTakesIt = true;
        }
    }
    return someRowTakesIt;
}

bool usesSpeakForInt32(Walker& walker, ValueID input)
{
    const BodyIndex& index = walker.currentBodyIndex();
    NamesInNumberExpectations names { input };
    Vector<bool, mostNamesFollowedInNumberExpectations> isSum { false };
    bool speaksForInt32 = false;
    bool speaksAgainstInt32 = false;
    for (size_t place = 0; place < names.size() && !speaksAgainstInt32; ++place) {
        ValueID name = names[place];
        bool nameIsSum = isSum[place];
        auto follow = [&](ValueID next, bool nextIsSum) {
            if (!next || names.contains(next) || names.size() == mostNamesFollowedInNumberExpectations)
                return;
            names.append(next);
            isSum.append(nextIsSum);
        };
        visitMentionsForNumberExpectations(index, name, [&](const MentionInNumberExpectations& mention) {
            if (!mention.user) {
                follow(mention.handedOnAs, nameIsSum);
                return;
            }
            BodyValue user = index.value(mention.user);
            Opcode opcode = user.opcode();
            if (allBitwiseOperators.contains(opcode)) {
                bool isTruncation = operatorsWithZeroAsIdentityInNumberExpectations.contains(opcode) && isWrittenZeroInNumberExpectations(index, user.operand(1 - mention.operandIndex));
                speaksForInt32 |= nameIsSum || !isTruncation;
            } else if (opcodesThatStateTypes.contains(opcode) && !mention.operandIndex)
                follow(mention.user, nameIsSum);
            else if (sumsFollowedInNumberExpectations.contains(opcode))
                follow(mention.user, true);
            else if (operatorsThatMakeFractions.contains(opcode))
                speaksAgainstInt32 = true;
            else if (accessesByKeyInNumberExpectations.contains(opcode))
                speaksForInt32 |= mention.operandIndex == OperandIndex::ElemGet::property;
            else if (rowsTakeArgumentAsPositionInNumberExpectations(walker, user, mention.operandIndex))
                speaksForInt32 = true;
        });
    }
    return speaksForInt32 && !speaksAgainstInt32;
}

static ValueID readOfEnvironmentFieldBehindNamesInNumberExpectations(const BodyIndex& index, ValueID value)
{
    while (value && !index.isParameter(value)) {
        value = index.stripCasts(value);
        if (index.isParameter(value) || index.opcodeOf(value) != Opcode::CheckInitialized)
            break;
        value = index.value(value).operand(0);
    }
    return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::GetEnvironmentField ? value : ValueID();
}

static bool turnsStepOrUseEnvironmentFieldAsInt32(Walker& walker, const BodyIndex& index, LoopID loop, LayoutID layout, uint32_t field)
{
    auto isReadOfField = [&](ValueID value) {
        ValueID read = readOfEnvironmentFieldBehindNamesInNumberExpectations(index, value);
        return read && index.value(read).layout() == layout && index.value(read).field() == field;
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::GetEnvironmentField && operation.layout() == layout && operation.field() == field) {
                ValueID name = operation.id();
                if (ValueID user = index.onlyUsingValue(name); user && index.opcodeOf(user) == Opcode::CheckInitialized)
                    name = user;
                if (usesSpeakForInt32(walker, name))
                    return true;
            }
            if (operation.opcode() != Opcode::PutEnvironmentField || operation.layout() != layout || operation.field() != field)
                continue;
            ValueID stored = index.stripCasts(operation.valueOperand());
            if (!stored || index.isParameter(stored))
                continue;
            BodyValue step = index.value(stored);
            if ((step.opcode() == Opcode::Increment || step.opcode() == Opcode::Decrement) && isReadOfField(step.operand(0)))
                return true;
            if ((step.opcode() == Opcode::Add || step.opcode() == Opcode::Sub) && (isReadOfField(step.operand(0)) || (step.opcode() == Opcode::Add && isReadOfField(step.operand(1)))))
                return true;
        }
    }
    return false;
}

static bool turnsTakeEnvironmentFieldForInt32(Walker& walker, LoopID loop, LayoutID layout, uint32_t field)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (turnsStepOrUseEnvironmentFieldAsInt32(walker, index, loop, layout, field))
        return true;
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (!allRelations.contains(operation.opcode()))
                continue;
            for (uint32_t side = 0; side < 2; ++side) {
                ValueID read = readOfEnvironmentFieldBehindNamesInNumberExpectations(index, operation.operand(side));
                ValueID other = readOfEnvironmentFieldBehindNamesInNumberExpectations(index, operation.operand(1 - side));
                if (!read || !other || index.value(read).layout() != layout || index.value(read).field() != field)
                    continue;
                if (turnsStepOrUseEnvironmentFieldAsInt32(walker, index, loop, index.value(other).layout(), index.value(other).field()))
                    return true;
            }
        }
    }
    return false;
}

static bool holdsNumbersBesideNullishAlone(const Type& type)
{
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent);
}

static TypeID withNumbersCutToInt32(TypeArena& types, const Type& type)
{
    Range insideInt32 = type.integers.intersectWith(Range::int32());
    if (!(type.primitives & Primitive::Integer) || insideInt32.isEmpty())
        return { };
    return types.withPrimitives(types.integer(insideInt32), type.primitives & ~Primitive::Number);
}

static bool holdsOnlyInt32InNumberExpectations(const Type& held)
{
    if (!held.isJavaScriptValue() || held.hasObjects() || !holdsNumbersBesideNullishAlone(held) || !(held.primitives & Primitive::Integer))
        return false;
    if (held.primitives & Primitive::FractionalNumber)
        return false;
    return held.integers == Range::full() || held.integers.isSubsetOf(Range::int32());
}

static bool isLeftAsFoundInNumberExpectations(const Walker::RememberedExpectation& remembered)
{
    return !remembered.isKnown && remembered.isHeld;
}

static ValueID readOfSlotBehindNamesInNumberExpectations(const BodyIndex& index, ValueID value)
{
    if (value)
        value = index.stripCasts(value);
    return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::PropGet && !index.value(value).isFreeName() ? value : ValueID();
}

static bool turnsTakeSlotForInt32(Walker& walker, LoopID loop, ValueID object, PropertyKeyID key)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto isReadOfSlot = [&](ValueID value) {
        ValueID read = readOfSlotBehindNamesInNumberExpectations(index, value);
        return read && index.value(read).key() == key && index.stripCasts(index.value(read).objectOperand()) == object;
    };
    auto placeOf = [](ValueID holder, PropertyKeyID name) {
        return static_cast<uint64_t>(holder.index()) << 32 | name.index();
    };
    Walker::AccessesOfSlotsInLoop& listed = walker.accessesOfSlotsInLoopListedLast();
    if (listed.body != &index || listed.loop != loop) {
        listed.body = &index;
        listed.loop = loop;
        listed.underObjectAndName.clear();
        listed.turnsTakeForInt32.clear();
        for (BlockID member : index.membersOf(loop)) {
            for (BodyValue operation : index.block(member).values()) {
                if ((operation.opcode() == Opcode::PropGet || operation.opcode() == Opcode::PropSet) && !operation.isFreeName())
                    listed.underObjectAndName.add(placeOf(index.stripCasts(operation.objectOperand()), operation.key()), Vector<ValueID, 2> { }).iterator->value.append(operation.id());
            }
        }
    }
    auto accesses = listed.underObjectAndName.find(placeOf(object, key));
    if (accesses == listed.underObjectAndName.end())
        return false;
    if (auto known = listed.turnsTakeForInt32.find(placeOf(object, key)); known != listed.turnsTakeForInt32.end())
        return known->value;
    auto keep = [&](bool takes) {
        if (listed.body == &index && listed.loop == loop)
            listed.turnsTakeForInt32.add(placeOf(object, key), takes);
        return takes;
    };
    for (ValueID access : accesses->value) {
        BodyValue operation = index.value(access);
        if (operation.opcode() == Opcode::PropGet) {
            if (usesSpeakForInt32(walker, access))
                return keep(true);
            continue;
        }
        ValueID stored = index.stripCasts(operation.valueOperand());
        if (!stored || index.isParameter(stored))
            continue;
        BodyValue step = index.value(stored);
        if ((step.opcode() == Opcode::Increment || step.opcode() == Opcode::Decrement) && isReadOfSlot(step.operand(0)))
            return keep(true);
    }
    return keep(false);
}

ExpectationOfNumber numbersExpectedOfSlotThatTurnsStep(Walker& walker, ValueID input, ExpectationOfNumber found)
{
    if (!found.type || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return found;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = readOfSlotBehindNamesInNumberExpectations(index, input);
    LoopID loop = read ? index.loopOf(index.blockOf(read)) : LoopID();
    if (!loop)
        return found;
    TypeArena& types = walker.types();
    Type expected = types.at(found.type);
    if (!holdsNumbersBesideNullishAlone(expected) || expected.isInt32OrNullish())
        return found;
    TypeID cut = withNumbersCutToInt32(types, expected);
    TypeID proven = provenTypeInNumberExpectations(walker, input);
    if (!cut || (proven && !types.overlap(proven, cut)))
        return found;
    if (!turnsTakeSlotForInt32(walker, loop, index.stripCasts(index.value(read).objectOperand()), index.value(read).key()))
        return found;
    return { cut, found.isDeclared, found.isHeld };
}

ExpectationOfNumber numberExpectedOfAddendOfPosition(Walker& walker, const BodyValue& operation, ValueID operand, ExpectationOfNumber found)
{
    if (!found.type || !sumsFollowedInNumberExpectations.contains(operation.opcode()) || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return found;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID written = index.stripCasts(operand);
    LoopID loop = index.loopOf(index.blockOf(operation.id()));
    if (!written || !loop || index.isInLoop(index.blockOf(written), loop))
        return found;
    TypeArena& types = walker.types();
    Type expected = types.at(found.type);
    if (!holdsNumbersBesideNullishAlone(expected) || expected.isInt32OrNullish())
        return found;
    TypeID cut = withNumbersCutToInt32(types, expected);
    TypeID proven = provenTypeInNumberExpectations(walker, operand);
    if (!cut || (proven && !types.overlap(proven, cut)) || !usesSpeakForInt32(walker, operation.id()))
        return found;
    return { cut, found.isDeclared, found.isHeld };
}

ExpectationOfNumber numbersExpectedOf(Walker& walker, ValueID input, ExpectationOfNumber found)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return found;
    if (isLeftAsFoundInNumberExpectations(walker.expectationRememberedOf(input)))
        return found;
    TypeArena& types = walker.types();
    TypeID proven = provenTypeInNumberExpectations(walker, input);
    if (found.type) {
        Type expected = types.at(found.type);
        if (!holdsNumbersBesideNullishAlone(expected) || expected.isInt32OrNullish())
            return found;
        TypeID cut = withNumbersCutToInt32(types, expected);
        if (!cut || (proven && !types.overlap(proven, cut)))
            return found;
        if (!(expected.primitives & Primitive::FractionalNumber))
            return { cut, found.isDeclared, found.isHeld };
        if (TypeID held = typeHeldAtPositionOf(walker, input); held && !isNeverType(held) && holdsOnlyInt32InNumberExpectations(types.at(held)))
            return { cut, found.isDeclared, true };
        if (ValueID read = readOfEnvironmentFieldBehindNamesInNumberExpectations(walker.currentBodyIndex(), input)) {
            const BodyIndex& index = walker.currentBodyIndex();
            LoopID loop = index.loopOf(index.blockOf(read));
            if (loop && turnsTakeEnvironmentFieldForInt32(walker, loop, index.value(read).layout(), index.value(read).field()))
                return { cut, found.isDeclared, found.isHeld };
        }
        if (usesSpeakForInt32(walker, input))
            return { cut, found.isDeclared, found.isHeld };
        return found;
    }

    if (!proven)
        return found;
    Type type = types.at(proven);
    if (holdsNumbersBesideNullishAlone(type)) {
        if (type.primitives & Primitive::FractionalNumber || type.isInt32OrNullish())
            return found;
        TypeID cut = withNumbersCutToInt32(types, type);
        return cut ? ExpectationOfNumber { cut, true, false } : found;
    }
    return found;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
