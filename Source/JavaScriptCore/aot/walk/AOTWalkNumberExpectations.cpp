/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Guesses, ParameterGuards: what kind of number is expected of a value. The guard at an entry, every guess and the loops that are built twice ask one function, so each tests for int32 in the place of number at once.
// NOTHING HERE IS A PROOF: Facts::held(), what is written and what the body does with a value only say what a test is to ask. It builds nothing, so a suspension and a throw mean nothing to it.
// Withheld: nobody asks.

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet readsOfPlacesInNumberExpectations { Opcode::PropGet, Opcode::PrivateGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture };
static constexpr OpcodeSet callsWithResultsInNumberExpectations { Opcode::Call, Opcode::CallList };
static constexpr OpcodeSet testsThatCutNumberExpectations = allRelations | allEqualities | OpcodeSet { Opcode::TypeTest };
static constexpr OpcodeSet sumsFollowedInNumberExpectations { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet operatorsWithZeroAsIdentityInNumberExpectations { Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
static constexpr OpcodeSet operatorsThatMakeFractions { Opcode::Div, Opcode::Pow };
static constexpr OpcodeSet accessesByKeyInNumberExpectations { Opcode::ElemGet, Opcode::ElemSet, Opcode::ElemHas };
static_assert(OperandIndex::ElemGet::property == OperandIndex::ElemSet::property && OperandIndex::ElemGet::property == OperandIndex::ElemHas::property);

// How many names of a value are followed through the body as written. They may lead round a loop.
static constexpr unsigned mostNamesFollowedInNumberExpectations = 16;
// A value that is mentioned more than once has no only-user link: so many operations behind it in its block are looked at for its mentions.
static constexpr uint32_t mostOperationsSearchedForMentionsInNumberExpectations = 32;

namespace {

struct MentionInNumberExpectations {
    // The operation that takes the value, as its operand `operandIndex`. Null: the end of the block mentions the value.
    ValueID user;
    uint32_t operandIndex { 0 };
    // `const d = c ? a.w + a.h : 0`: the parameter that an edge or a test hands the value to. Null: it goes on under no other name.
    ValueID handedOnAs;
};

using NamesInNumberExpectations = Vector<ValueID, mostNamesFollowedInNumberExpectations>;

} // anonymous namespace

// ---- What is held

// The row's own rule over the types that `typeOfOperand` gives. Null: the row gives no result, or several.
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

static bool saysNothingInNumberExpectations(const TypeArena& types, TypeID type)
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
    ValueID value = index.valueBehindCasts(input);
    // `let pos: number` that closures share is read behind a look at its dead zone.
    while (value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::CheckInitialized)
        value = index.valueBehindCasts(index.value(value).operand(0));
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
        // What `return` hands over in a coroutine is not what its caller gets.
        if (!callee || callee.index() >= held.functions.size() || walker.program().at(callee).isCoroutine())
            return { };
        return held.function(callee).result;
    }
    if (!readsOfPlacesInNumberExpectations.contains(opcode))
        return { };

    // `a.b.n`: the field in what is proven of the receiver, else in what is expected of it. The receiver is defined in front of the read, so every road ends.
    TypeArena& types = walker.types();
    std::optional<uint8_t> placeOfReceiver = receiverOf(opcode);
    TypeID receiver;
    if (placeOfReceiver) {
        ValueID object = definition.operand(*placeOfReceiver);
        receiver = provenTypeInNumberExpectations(walker, object);
        if (saysNothingInNumberExpectations(types, receiver))
            receiver = object && object < value ? findExpectation(walker, object).type : TypeID();
        if (saysNothingInNumberExpectations(types, receiver))
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

// ---- The mentions of a value, by the body

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

// visit(mention) for each mention of the value that is found. A value that is mentioned once has its only-user link. The mentions of any other are looked for behind it in its block, and at the end of that block.
// False: some mention was not found.
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

// `s.slice(start)`, `xs.at(i)`, `s.padStart(n)`: what is called is proven one built-in, by binding or by a lock, and every row of its calls that has the argument takes it as a position or a count.
static bool rowsTakeArgumentAsPositionInNumberExpectations(Walker& walker, const BodyValue& call, uint32_t operandIndex)
{
    if (call.opcode() != Opcode::Call || operandIndex < numberOfNamedOperandsOf(Opcode::Call))
        return false;
    TypeID typeOfCallee = provenTypeInNumberExpectations(walker, call.calleeOperand());
    // `text.charCodeAt(pos)`, asked ahead of the walk: nothing is built for the read of the method yet. What is expected of `text` says which member the read would find. No proof, as all here.
    if (saysNothingInNumberExpectations(walker.types(), typeOfCallee)) {
        const BodyIndex& index = walker.currentBodyIndex();
        ValueID read = index.valueBehindCasts(call.calleeOperand());
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
    // The names from here on stand for sums that the value goes into.
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
                // `x | 0` is how a program truncates: it is written where x may be a fraction. `(a + b) | 0` is how it adds int32s.
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
        value = index.valueBehindCasts(value);
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
            ValueID stored = index.valueBehindCasts(operation.valueOperand());
            if (!stored || index.isParameter(stored))
                continue;
            BodyValue step = index.value(stored);
            if ((step.opcode() == Opcode::Increment || step.opcode() == Opcode::Decrement) && isReadOfField(step.operand(0)))
                return true;
            // `pos += charSize(ch)`
            if ((step.opcode() == Opcode::Add || step.opcode() == Opcode::Sub) && (isReadOfField(step.operand(0)) || (step.opcode() == Opcode::Add && isReadOfField(step.operand(1)))))
                return true;
        }
    }
    return false;
}

// By the body: what the turns of the loop do with the variable speaks for an int32: usesSpeakForInt32() of one of its reads, a step (`pos++`, `pos += size`), a relation with a variable that is so stepped.
static bool turnsTakeEnvironmentFieldForInt32(Walker& walker, LoopID loop, LayoutID layout, uint32_t field)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (turnsStepOrUseEnvironmentFieldAsInt32(walker, index, loop, layout, field))
        return true;
    // `pos < end`
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
// ---- Numbers among what is expected

static bool holdsNumbersBesideNullishAlone(const Type& type)
{
    return type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent);
}

// Null: none of its integers fits.
static TypeID withNumbersCutToInt32(TypeArena& types, const Type& type)
{
    Range insideInt32 = type.integers.intersectWith(Range::int32());
    if (!(type.primitives & Primitive::Integer) || insideInt32.isEmpty())
        return { };
    return types.withPrimitives(types.integer(insideInt32), type.primitives & ~Primitive::Number);
}

// Link gives integers without a bound to what arithmetic and rounding make, and whole numbers beside them for what outgrows an integer. `at(k + 1)`: such a result seldom outgrows 32 bits.
static bool holdsOnlyInt32InNumberExpectations(const Type& held)
{
    if (!held.isJavaScriptValue() || held.hasObjects() || !holdsNumbersBesideNullishAlone(held) || !(held.primitives & Primitive::Integer))
        return false;
    // NaN, -0 and what outgrows an integer come seldom beside integers: `isLetter(text.charCodeAt(i))`. A fraction says that fractions come.
    if (held.primitives & Primitive::FractionalNumber)
        return false;
    return held.integers == Range::full() || held.integers.isSubsetOf(Range::int32());
}

// ---- Expectations are cut by tests

// `for (let i = point[depth]; i > 0; i--)`: of a parameter of a block that takes a proof. The rule that cuts the proven type of the value in front of the test cuts what is expected of it.
static ExpectationOfNumber cutByTestInNumberExpectations(Walker& walker, ValueID input, ExpectationOfNumber found)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    if (!found.type || !index.isParameter(input) || !(types.at(found.type).primitives & Primitive::Number))
        return found;
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(input));
    if (!edge)
        return found;

    std::optional<BodyEdgeResult> proof;
    uint32_t numberOfProofs = static_cast<uint32_t>(edge->target.proofs().size());
    for (uint32_t proofIndex = 0; proofIndex < numberOfProofs && !proof; ++proofIndex) {
        std::optional<BodyEdgeResult> result = index.edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
        if (result && result->parameter == input)
            proof = result;
    }
    if (!proof || !proof->test || index.isParameter(proof->test))
        return found;
    BodyValue test = index.value(proof->test);
    if (!testsThatCutNumberExpectations.contains(test.opcode()))
        return found;

    Vector<TypeID, 4> operandTypes;
    operandTypes.fill(TypeID(), numberOfNamedOperandsOf(test.opcode()) + test.variadicOperands().size());
    test.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        TypeID proven = provenTypeInNumberExpectations(walker, operand);
        operandTypes[operandIndex] = proven ? proven : TypeID(TypeTable::anything);
    });
    if (proof->operandIndex >= operandTypes.size())
        return found;
    operandTypes[proof->operandIndex] = found.type;

    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };
    TypeID behind = typeOfEdgeResult(expecting, test, OperandTypes { operandTypes.span() }, proof->operandIndex, proof->isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
    if (!behind || isNeverType(behind) || behind == found.type)
        return found;
    return { behind, found.isDeclared, found.isHeld };
}

// ---- Expected is what is written united with what was seen

static uint16_t kindsOfPrimitivesAmongInNumberExpectations(uint16_t primitives)
{
    uint16_t kinds = 0;
    for (uint16_t kind : { Primitive::Number, Primitive::Boolean, Primitive::AnyString, Primitive::AnySymbol, Primitive::BigInt }) {
        if (primitives & kind)
            kinds |= kind;
    }
    return kinds;
}

static TypeID writtenTypeUnitedWithTypeSeen(TypeArena& types, TypeID written, TypeID seen)
{
    Type type = types.at(written);
    if (!type.isJavaScriptValue())
        return written;
    uint16_t leftToWrittenType = Primitive::UndefinedOrNull | Primitive::Absent | Primitive::Unspecified | kindsOfPrimitivesAmongInNumberExpectations(type.primitives);
    TypeParts added = types.at(types.withoutPrimitives(seen, leftToWrittenType));
    // An object says nothing of contents, and in a union that swallows what the other side says of them.
    if (added.unnamedClasses && type.unnamedClasses) {
        Vector<ClassPart, 8> classesNobodyWrote;
        for (const ClassPart& part : types.at(added.unnamedClasses)) {
            if (!findClassPart(types, type, part.builtinClass))
                classesNobodyWrote.append(part);
        }
        added.unnamedClasses = classesNobodyWrote.isEmpty() ? ClassPartsID() : types.internClassParts(classesNobodyWrote.span());
    }
    return types.unionOf(written, types.intern(added));
}

// `size([1, 2])` for `l: { length: number }`: the test lets through what the program does bring. Where no one test tells the union, what is written stands.
static ExpectationOfNumber unitedWithWhatWasSeenInNumberExpectations(Walker& walker, ValueID input, ExpectationOfNumber found)
{
    TypeArena& types = walker.types();
    if (!found.type || !found.isDeclared || found.isHeld)
        return found;
    Type written = types.at(found.type);
    if (!written.isJavaScriptValue() || !written.hasObjects() || written.hasUnboundedUnknown() || written.hasOnlyClosures())
        return found;
    TypeID seen = typeHeldAtPositionOf(walker, input);
    if (saysNothingInNumberExpectations(types, seen) || !types.at(seen).isJavaScriptValue() || types.isSubset(types.withOnlyObjects(seen), found.type))
        return found;

    // A test asks for shape numbers, which only a layout that holds compiled objects has.
    Type objectsSeen = types.at(types.describedByShape(types.withOnlyObjects(seen)));
    if (objectsSeen.hasUnboundedUnknown())
        return found;
    for (LayoutID layout : membersOf(types, objectsSeen)) {
        if (!walker.plan().isInUse(layout) || !walker.plan().layout(layout).holdsCompiledObjects())
            return found;
    }

    // Where no object is made as any layout that is written, the declaration says which members a value has and nothing of who made it: what was seen stands alone, as a weaker guess.
    bool someWrittenLayoutHasObjects = !written.hasOnlyLayouts();
    for (LayoutID layout : membersOf(types, written))
        someWrittenLayoutHasObjects |= walker.plan().isInUse(layout);
    TypeID writtenPart = someWrittenLayoutHasObjects ? found.type : types.withoutObjects(found.type);
    TypeID united = writtenTypeUnitedWithTypeSeen(types, writtenPart, types.describedByShape(seen));
    if (TypeID proven = provenTypeInNumberExpectations(walker, input)) {
        TypeID within = types.intersectionOf(proven, united);
        if (!isNeverType(within))
            united = within;
    }
    Type tested = types.at(types.testablePartOf(types.withLiteralStringsWidened(types.withoutNullish(widenedToFamily(types, walker.facts().families(), united)))));
    if (tested.isNever() || !tested.isJavaScriptValue() || tested.hasUnboundedUnknown() || (tested.hasObjects() && tested.primitives))
        return found;
    return { united, someWrittenLayoutHasObjects, true };
}

// ---- Whoever asks with no test behind the answer

// In the list of what is remembered: nothing is known of the value, and what is found of it is left as it is found. findExpectationForGuesses() writes what it finds over the mark.
static bool isLeftAsFoundInNumberExpectations(const Walker::RememberedExpectation& remembered)
{
    return !remembered.isKnown && remembered.isHeld;
}

// ---- A slot that the turns of a loop step

static ValueID readOfSlotBehindNamesInNumberExpectations(const BodyIndex& index, ValueID value)
{
    if (value)
        value = index.valueBehindCasts(value);
    return value && !index.isParameter(value) && index.opcodeOf(value) == Opcode::PropGet && !index.value(value).isFreeName() ? value : ValueID();
}

static bool turnsTakeSlotForInt32(Walker& walker, LoopID loop, ValueID object, PropertyKeyID key)
{
    const BodyIndex& index = walker.currentBodyIndex();
    auto isReadOfSlot = [&](ValueID value) {
        ValueID read = readOfSlotBehindNamesInNumberExpectations(index, value);
        return read && index.value(read).key() == key && index.valueBehindCasts(index.value(read).objectOperand()) == object;
    };
    for (BlockID member : index.membersOf(loop)) {
        for (BodyValue operation : index.block(member).values()) {
            if (operation.opcode() == Opcode::PropGet && isReadOfSlot(operation.id()) && usesSpeakForInt32(walker, operation.id()))
                return true;
            if (operation.opcode() != Opcode::PropSet || operation.isFreeName() || operation.key() != key || index.valueBehindCasts(operation.objectOperand()) != object)
                continue;
            ValueID stored = index.valueBehindCasts(operation.valueOperand());
            if (!stored || index.isParameter(stored))
                continue;
            BodyValue step = index.value(stored);
            if ((step.opcode() == Opcode::Increment || step.opcode() == Opcode::Decrement) && isReadOfSlot(step.operand(0)))
                return true;
        }
    }
    return false;
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
    if (!turnsTakeSlotForInt32(walker, loop, index.valueBehindCasts(index.value(read).objectOperand()), index.value(read).key()))
        return found;
    return { cut, found.isDeclared, found.isHeld };
}

// ---- The one function

ExpectationOfNumber numbersExpectedOf(Walker& walker, ValueID input, ExpectationOfNumber found)
{
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers))
        return found;
    if (isLeftAsFoundInNumberExpectations(walker.rememberedExpectationOf(input)))
        return found;
    TypeArena& types = walker.types();
    found = cutByTestInNumberExpectations(walker, input, found);
    found = unitedWithWhatWasSeenInNumberExpectations(walker, input, found);

    TypeID proven = provenTypeInNumberExpectations(walker, input);
    if (found.type) {
        Type expected = types.at(found.type);
        if (!holdsNumbersBesideNullishAlone(expected) || expected.isInt32OrNullish())
            return found;
        TypeID cut = withNumbersCutToInt32(types, expected);
        if (!cut || (proven && !types.overlap(proven, cut)))
            return found;
        // A counter seldom outgrows 32 bits.
        if (!(expected.primitives & Primitive::FractionalNumber))
            return { cut, found.isDeclared, found.isHeld };
        if (TypeID held = typeHeldAtPositionOf(walker, input); held && !isNeverType(held) && holdsOnlyInt32InNumberExpectations(types.at(held)))
            return { cut, found.isDeclared, true };
        // `pos < end`, `pos++`, `pos += size`: each mention of a variable that closures share is a read of its own, and none of them says much alone. What the turns of the loop do with the variable is asked of all of them.
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

    // Whoever plans the guards of entries has built nothing. What follows opens a guess in a loop and is no reason for a slow version.
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
