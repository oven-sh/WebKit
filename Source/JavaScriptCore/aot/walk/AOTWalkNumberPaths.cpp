/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);
OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet operatorsOfNumberPaths = allSums | allBitwiseOperators | allRelations | OpcodeSet { Opcode::Mul, Opcode::Negate, Opcode::StrictEq };
static constexpr OpcodeSet operatorsThatGiveNumbersOnNumberPaths { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Negate, Opcode::Increment, Opcode::Decrement };
static constexpr OpcodeSet operatorsCheckedOnIntegersInNumberPaths { Opcode::Mul, Opcode::Negate };
static constexpr OpcodeSet operatorsBesideOneNumber { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
static constexpr OpcodeSet takersOfBoxesInNumberPaths { Opcode::PropSet, Opcode::PropDefine, Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::Strcat };
static constexpr OpcodeSet callsAmongTakersOfBoxes { Opcode::Call, Opcode::CallList, Opcode::Construct };

static constexpr uint32_t mostOperatorsOfNumberPath = 16;
static constexpr size_t mostNumbersTestedOnNumberPath = defaultLimitValue(LimitID::MaxNumberPathValues, hostArchitecture());
static constexpr unsigned mostCastsFollowedInNumberPaths = 16;
static constexpr uint32_t mostOperationsLookedAtBehindOperatorOfNumberPath = 8;

namespace {

enum class KindOnNumberPath : uint8_t {
    Foreign,
    Integer,
    Tagged,
    Waits,
};

struct OperandOnNumberPath {
    KindOnNumberPath kind { KindOnNumberPath::Foreign };
    ValueID tagged;
    bool isDoubleOnIntegersToo { false };
    uint32_t place { 0 };
};

struct NumberPathInBody {
    uint32_t begin { 0 };
    uint32_t end { 0 };
    uint32_t numberOfOperators { 0 };
    Vector<TestOfNumber, 8> tests;
};

} // anonymous namespace

bool isTaggedNumber(Walker& walker, Reference reference)
{
    if (!reference || !reference.isValue() || walker.representationOf(reference) != Representation::JSValue)
        return false;
    Type type = walker.types().at(walker.typeOf(reference));
    if (!type.isNumber() || type.isInteger() || !(type.primitives & Primitive::Integer) || type.integers.isEmpty())
        return false;
    return type.integers.minimum <= Range::int32().maximum && type.integers.maximum >= Range::int32().minimum;
}

static bool isTakenOnlyByOperatorsOfNumberPath(const BodyIndex& index, const BodyBlock& block, ValueID value)
{
    uint32_t numberOfMentions = index.useCount(value);
    if (!numberOfMentions || numberOfMentions == BodyIndex::maxCountedUses)
        return false;

    if (numberOfMentions == 1) {
        for (unsigned step = 0; step < mostCastsFollowedInNumberPaths; ++step) {
            ValueID userID = index.onlyUsingValue(value);
            if (!userID || !block.holds(userID))
                return false;
            BodyValue user = index.value(userID);
            if (operatorsOfNumberPaths.contains(user.opcode()))
                return true;
            if (!opcodesThatStateTypes.contains(user.opcode()) || user.operand(0) != value)
                return false;
            value = userID;
        }
        return false;
    }

    uint32_t end = std::min(block.firstValue().index() + block.numberOfValues(), value.index() + 1 + mostOperationsLookedAtBehindOperatorOfNumberPath);
    for (uint32_t number = value.index() + 1; number < end; ++number) {
        BodyValue user = index.value(ValueID(number));
        uint32_t numberOfMentionsByUser = 0;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            numberOfMentionsByUser += operand == value;
        });
        if (!numberOfMentionsByUser)
            continue;
        if (!operatorsOfNumberPaths.contains(user.opcode()))
            return false;
        numberOfMentions -= numberOfMentionsByUser;
        if (!numberOfMentions)
            return true;
    }
    return false;
}

static std::optional<NumberPathInBody> findNumberPathInBody(Walker& walker, const BodyValue& firstOperator)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstOperator.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t begin = firstOperator.id().index() - firstValue;
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    uint32_t first = firstValue + begin;

    Vector<OperandOnNumberPath, 32> given;
    Vector<uint32_t, 32> mentionsLeft;
    uint32_t mentionsLeftOfAll = 0;
    uint32_t numberOfOperators = 0;
    Vector<TestOfNumber, 8> tests;
    Vector<Reference, 8> tested;
    NumberPathInBody path { begin, begin, 0, { } };

    auto kindOf = [&](ValueID operand) -> OperandOnNumberPath {
        if (operand.index() >= first)
            return given[operand.index() - first];
        Reference reference = walker.referenceOf(operand);
        if (!reference || !reference.isRecord())
            return { };
        if (isTaggedNumber(walker, reference))
            return { KindOnNumberPath::Tagged, operand, 0 };
        return { walker.types().at(walker.typeOf(reference)).isInteger() ? KindOnNumberPath::Integer : KindOnNumberPath::Foreign, { }, 0 };
    };

    for (uint32_t place = begin; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        Opcode opcode = operation.opcode();
        OperandOnNumberPath result;
        uint32_t mentionsOfResult = 0;

        if (opcodesThatStateTypes.contains(opcode)) {
            result = kindOf(operation.operand(0));
            if (result.kind == KindOnNumberPath::Waits) {
                uint32_t uses = index.useCount(operation.id());
                if (uses == BodyIndex::maxCountedUses)
                    break;
                mentionsLeft[result.place] += uses;
                mentionsLeft[result.place] -= 1;
                mentionsLeftOfAll += uses;
                mentionsLeftOfAll -= 1;
            }
        } else if (opcode == Opcode::Integer)
            result.kind = KindOnNumberPath::Integer;
        else if (primitiveConstants.contains(opcode)) {
        } else {
            if (!operatorsOfNumberPaths.contains(opcode) || numberOfOperators == mostOperatorsOfNumberPath)
                break;
            bool takesOperatorThatWaits = false;
            bool takesTaggedNumber = false;
            bool takesForeignValue = false;
            bool takesDoubleOnIntegersToo = false;
            Vector<OperandOnNumberPath, 2> operands;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                operands.append(kindOf(operand));
                takesOperatorThatWaits |= operands.last().kind == KindOnNumberPath::Waits;
                takesTaggedNumber |= operands.last().kind == KindOnNumberPath::Tagged;
                takesForeignValue |= operands.last().kind == KindOnNumberPath::Foreign;
                takesDoubleOnIntegersToo |= operands.last().kind == KindOnNumberPath::Waits && operands.last().isDoubleOnIntegersToo;
            });
            if (takesForeignValue || (!takesOperatorThatWaits && !takesTaggedNumber))
                break;

            bool testsTooManyNumbers = false;
            for (const OperandOnNumberPath& operand : operands) {
                if (operand.kind == KindOnNumberPath::Waits) {
                    --mentionsLeft[operand.place];
                    --mentionsLeftOfAll;
                }
                if (operand.kind != KindOnNumberPath::Tagged || takesDoubleOnIntegersToo)
                    continue;
                Reference reference = walker.referenceOf(operand.tagged);
                if (tested.contains(reference))
                    continue;
                if (tested.size() == mostNumbersTestedOnNumberPath) {
                    testsTooManyNumbers = true;
                    break;
                }
                tested.append(reference);
                tests.append(TestOfNumber { operand.tagged, TypeTable::int32 });
            }
            if (testsTooManyNumbers)
                break;
            ++numberOfOperators;

            if (operatorsThatGiveNumbersOnNumberPaths.contains(opcode) && isTakenOnlyByOperatorsOfNumberPath(index, block, operation.id())) {
                result = { KindOnNumberPath::Waits, { }, takesDoubleOnIntegersToo || operatorsCheckedOnIntegersInNumberPaths.contains(opcode), place - begin };
                mentionsOfResult = index.useCount(operation.id());
                mentionsLeftOfAll += mentionsOfResult;
            } else {
                if (!takesOperatorThatWaits)
                    break;
                if (allBitwiseOperators.contains(opcode))
                    result.kind = KindOnNumberPath::Integer;
                if (!mentionsLeftOfAll) {
                    path.end = place + 1;
                    path.numberOfOperators = numberOfOperators;
                    path.tests = tests;
                }
            }
        }
        given.append(result);
        mentionsLeft.append(mentionsOfResult);
    }
    if (path.end == path.begin || path.tests.isEmpty())
        return std::nullopt;
    return path;
}

static bool buildNumberPath(Walker& walker, const BodyValue& firstOperator)
{
    if (!operatorsOfNumberPaths.contains(firstOperator.opcode()))
        return false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    bool takesTaggedNumber = false;
    firstOperator.forEachOperand([&](ValueID operand, uint32_t) {
        takesTaggedNumber |= isTaggedNumber(walker, walker.referenceOf(operand));
    });
    if (!takesTaggedNumber)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    if (LoopID loop = index.loopOf(index.blockOf(firstOperator.id()))) {
        ValueID taker = index.onlyUsingValue(firstOperator.id());
        while (taker && opcodesThatStateTypes.contains(index.opcodeOf(taker)))
            taker = index.onlyUsingValue(taker);
        bool givesSameInEveryTurn = taker && allBitwiseOperators.contains(index.opcodeOf(taker));
        firstOperator.forEachOperand([&](ValueID operand, uint32_t) {
            ValueID written = index.stripCasts(operand);
            givesSameInEveryTurn &= !index.isInLoop(index.blockOf(written), loop) || (!index.isParameter(written) && primitiveConstants.contains(index.opcodeOf(written)));
        });
        if (givesSameInEveryTurn)
            return false;
    }

    std::optional<NumberPathInBody> path = findNumberPathInBody(walker, firstOperator);
    if (!path)
        return false;
    if (!shouldOpenGuess(walker, walker.referenceOf(path->tests[0].input), TypeTable::int32, true, path->numberOfOperators))
        return false;
    BlockID block = walker.currentBodyIndex().blockOf(firstOperator.id());
    return walkStretchBehindTestsOfNumbers(walker, path->tests.span(), block, path->begin, path->end, BlockFrequency::Unlikely);
}

static_assert(opcodesThatBuildArmsInFrontOfTypedFormsTakes == (operatorsOfNumberPaths));

bool buildArmsInFrontOfTypedFormsOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    return buildNumberPath(walker, value);
}

static bool takerWantsBoxInNumberPaths(Walker& walker, const BodyValue& taker, ValueID taken, Reference standIn, TypeID typeOfSum)
{
    if (!takersOfBoxesInNumberPaths.contains(taker.opcode()))
        return false;

    Draft draft(taker.opcode(), walker.originOf(taker));
    draft.flags = static_cast<uint8_t>(taker.flags());
    bool lacksOperand = false;
    copyFields(taker, draft, [&](ValueID operand) {
        Reference reference = operand == taken ? standIn : walker.referenceOf(operand);
        lacksOperand |= !reference || !reference.isRecord();
        return reference;
    });
    if (lacksOperand)
        return false;

    if (callsAmongTakersOfBoxes.contains(taker.opcode())) {
        if (taker.calleeOperand() == taken || findOnlyCallee(walker, taker, walker.referenceOf(taker.calleeOperand())))
            return false;
        auto typeOfInput = [&](ValueID input) {
            return input == taken ? typeOfSum : walker.typeOf(walker.referenceOf(input));
        };
        if (walker.has(Mechanism::BuiltinCalls) && wouldCallBuiltin(walker, taker, TypesOfInputs(typeOfInput)) != BuiltinCallAskedAbout::None)
            return false;
    }

    walker.chooseConservativeFields(draft);
    WantedRepresentations wanted = wantedRepresentationsOf(walker, draft);
    bool takesBox = true;
    taker.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (operand == taken)
            takesBox &= operandIndex < wanted.size() && wanted[operandIndex] == Representation::JSValue;
    });
    return takesBox;
}

static bool onlyUserTakesBoxInNumberPaths(Walker& walker, const BodyValue& written, Reference standIn, TypeID typeOfSum, bool otherArmIsClosed)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BlockID blockOfSum = index.blockOf(written.id());
    BodyBlock block = index.block(blockOfSum);
    ValueID value = written.id();
    for (unsigned step = 0; step < mostCastsFollowedInNumberPaths; ++step) {
        std::optional<BodyUser> user = index.onlyUser(value);
        if (!user)
            return false;
        switch (user->kind) {
        case BodyUseKind::Operand: {
            if (!block.holds(user->value))
                return false;
            if (user->value.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked())
                return otherArmIsClosed;
            BodyValue taker = walker.asWalked(index.value(user->value));
            if (!opcodesThatStateTypes.contains(taker.opcode()))
                return takerWantsBoxInNumberPaths(walker, taker, value, standIn, typeOfSum);
            value = taker.id();
            break;
        }
        case BodyUseKind::TerminatorOperand: {
            if (user->block != blockOfSum || index.terminator(blockOfSum).kind() != TerminatorKind::Return || !walker.walksOwnBodyOfUnit())
                return false;
            const CallingConventions& conventions = walker.plan().conventions();
            return conventions.numberOfResults(walker.unit()) == 1 && conventions.resultRepresentation(walker.unit()) == Representation::JSValue;
        }
        case BodyUseKind::EdgeArgument: {
            bool goesRoundBoxed = false;
            for (BodyTarget target : index.terminator(user->block).targets()) {
                std::span<const ValueID> arguments = target.arguments();
                for (uint32_t position = 0; position < arguments.size(); ++position) {
                    if (arguments[position] != value)
                        continue;
                    bool isHeadAround = false;
                    for (LoopID loop = index.loopOf(blockOfSum); loop && !isHeadAround; loop = index.loop(loop).parent)
                        isHeadAround = index.loop(loop).head == target.block();
                    if (!isHeadAround)
                        return false;
                    Reference parameter = walker.referenceBoundTo(index.block(target.block()).parameter(position));
                    if (!parameter || !parameter.isRecord() || walker.representationOf(parameter) != Representation::JSValue)
                        return false;
                    goesRoundBoxed = true;
                }
            }
            return goesRoundBoxed;
        }
        case BodyUseKind::EdgeProof:
        case BodyUseKind::HandlerArgument:
        case BodyUseKind::BlockSubject:
            return false;
        }
    }
    return false;
}

bool sumStaysBoxed(Walker& walker, const BodyValue& written, const Draft& sum)
{
    if (!walker.has(Mechanism::UnboxedNumbers) || !allSums.contains(sum.opcode) || written.opcode() != sum.opcode)
        return false;
    if (sum.form() != OperatorForm::Numbers || sum.representation != Representation::Unchosen)
        return false;

    Reference boxedNumber;
    bool otherArmIsClosed = false;
    for (Reference operand : sum.operands()) {
        Type type = walker.types().at(walker.typeOf(operand));
        if (type.isInt32())
            continue;
        if (!type.isNumber() || !(type.primitives & Primitive::Integer) || walker.representationOf(operand) != Representation::JSValue)
            return false;
        boxedNumber = operand;
        if (const Operation& record = walker.code().at(operand.id()); sum.opcode == Opcode::Add && record.opcode == Opcode::Narrowed)
            otherArmIsClosed |= !walker.types().at(walker.typeOf(Reference::value(record.valueOperand()))).isNumber();
    }
    if (!boxedNumber)
        return false;
    return onlyUserTakesBoxInNumberPaths(walker, written, boxedNumber, walker.resultTypeOf(sum), otherArmIsClosed);
}

static_assert(opcodesThatBuildOperatorBesideOneNumberTakes == (operatorsBesideOneNumber));

static std::optional<uint32_t> findOperandThatIsNumberOrUndefinedBesideAnyValue(Walker& walker, const BodyValue& value)
{
    static constexpr OpcodeSet turnBothOperandsIntoNumerics { Opcode::Sub, Opcode::Mul, Opcode::Div };
    if (!turnBothOperandsIntoNumerics.contains(value.opcode()))
        return std::nullopt;
    auto typeAt = [&](uint32_t operandIndex) {
        return walker.types().at(walker.typeOf(walker.referenceOf(value.operand(operandIndex))));
    };
    for (uint32_t operandIndex = 0; operandIndex < 2; ++operandIndex) {
        Type type = typeAt(operandIndex);
        if (type.primitives & Primitive::Number && type.primitives & Primitive::Undefined && type.hasOnlyPrimitives(Primitive::Number | Primitive::Undefined) && !typeAt(1 - operandIndex).isNumberLike())
            return operandIndex;
    }
    return std::nullopt;
}

bool buildOperatorBesideOneNumberOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!operatorsBesideOneNumber.contains(value.opcode()) || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    OperatorForm form = operatorFormFor(walker, value.opcode(), value, InputsOfTypedOperation(walker, nullptr));
    std::optional<uint32_t> turnedInFront = form == OperatorForm::Generic && walker.has(Mechanism::UnboxedNumbers) ? findOperandThatIsNumberOrUndefinedBesideAnyValue(walker, value) : std::nullopt;
    bool takesNumberRaw = form == OperatorForm::NumberAndValue && walker.has(Mechanism::UnboxedNumbers);
    if (!takesNumberRaw && form != OperatorForm::MayBeString && !turnedInFront)
        return false;

    Draft draft = walker.draftOf(value);
    if (walker.hasFailed())
        return false;
    walker.chooseConservativeFields(draft);
    if (turnedInFront) {
        Draft conversion(Opcode::ToNumber, draft.origin());
        conversion.setValueOperand(draft.operand(*turnedInFront));
        walker.chooseConservativeFields(conversion);
        conversion.setForm(operatorFormFor(walker, conversion));
        draft.setOperand(*turnedInFront, walker.build(WTF::move(conversion)));
        form = operatorFormFor(walker, draft);
        if (walker.hasFailed())
            return false;
    }
    if (form == OperatorForm::NumberAndValue && tryGuardNumericOperand(walker, draft, value.id()))
        form = operatorFormFor(walker, draft);
    else if (Walker::NumberAskedBesideNumber asked = form == OperatorForm::NumberAndValue && !turnedInFront ? walker.askValueBesideNumberForNumber(value) : Walker::NumberAskedBesideNumber()) {
        draft.setOperand(asked.operandIndex, asked.proven);
        form = operatorFormFor(walker, draft);
    }
    draft.setForm(form);
    Origin origin = draft.origin();
    walker.bind(value.id(), boxNumberIfNeeded(walker, walker.build(WTF::move(draft)), origin));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
