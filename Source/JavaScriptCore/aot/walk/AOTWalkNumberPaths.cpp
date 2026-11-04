/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// UnboxedNumbers, for numbers that arrive boxed: a chain of operators is walked on integers and on doubles behind one test of each tag, a sum that is taken boxed stays boxed, a truncation reads the tag,
// and arithmetic beside one proven number takes that number raw. Nothing here is a guess: every arm is right.
// At a suspension: no chain holds an operation that may suspend. At a throw: operators on numbers throw nothing, and the closed forms throw as ever.
// Withheld (UnboxedNumbers, TypedOperations; Guesses for the chains): each operator takes the form that the chain gives it.

#include "config.h"
#include "AOTWalkNumberGuesses.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTIRProgram.h"
#include "AOTLimits.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <algorithm>
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);
// AOTWalkNumbers.cpp
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);

static constexpr OpcodeSet operatorsOfNumberPaths = allSums | allBitwiseOperators | allRelations | OpcodeSet { Opcode::Mul, Opcode::Negate, Opcode::StrictEq };
// Of those, these give a number that may be no integer, so only their results gain by an integer arm. A bit operator gives an integer on both sides.
static constexpr OpcodeSet operatorsThatGiveNumbersOnNumberPaths { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Negate, Opcode::Increment, Opcode::Decrement };
// Of those, on two int32 these may give -0 or more than an integer holds: the checked form, whose answer is a Double.
static constexpr OpcodeSet operatorsCheckedOnIntegersInNumberPaths { Opcode::Mul, Opcode::Negate };
// Whether one of them pays by itself is what the tables of forms say: loneOperatorPaysOnIntegers().
static constexpr OpcodeSet operatorsThatMayStandAloneOnNumberPaths = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise });
static constexpr OpcodeSet operatorsBesideOneNumber { Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div };
static constexpr OpcodeSet operatorsWithZeroAsIdentityInNumberPaths { Opcode::BitOr, Opcode::BitXor, Opcode::LShift, Opcode::RShift, Opcode::URShift };
static constexpr OpcodeSet commutativeAmongOperatorsWithZeroAsIdentity { Opcode::BitOr, Opcode::BitXor };
// Closed, each takes what it stores, hands over or joins as a box.
static constexpr OpcodeSet takersOfBoxesInNumberPaths { Opcode::PropSet, Opcode::PropDefine, Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::Call, Opcode::CallList, Opcode::Construct, Opcode::Strcat };
static constexpr OpcodeSet callsAmongTakersOfBoxes { Opcode::Call, Opcode::CallList, Opcode::Construct };

// Both copies of a chain are code at the site.
static constexpr uint32_t mostOperatorsOfNumberPath = 16;
static constexpr size_t mostNumbersTestedOnNumberPath = defaultLimitValue(LimitID::MaxNumberPathValues, hostArchitecture());
static constexpr unsigned mostCastsFollowedInNumberPaths = 16;
// `d * d`: how many operations behind an operator are looked at for the users of a result that is mentioned more than once.
static constexpr uint32_t mostOperationsLookedAtBehindOperatorOfNumberPath = 8;

namespace {

enum class KindOnNumberPath : uint8_t {
    // It is no operand of an operator of a path.
    Foreign,
    Integer,
    // A number from in front of the chain that arrives boxed.
    Tagged,
    // What an operator of the chain gives, which only operators of the chain take.
    Waits,
};

struct OperandOnNumberPath {
    KindOnNumberPath kind { KindOnNumberPath::Foreign };
    // Tagged: the value of the body in front of the chain.
    ValueID tagged;
    // Waits: on int32 too the operator gives a Double: a product and a negation are checked, and so is whatever takes one. Whoever takes it works on doubles on both sides.
    bool isDoubleOnIntegersToo { false };
    // Waits: the place of the operator in the chain.
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

// ---- Number paths

// Every mention of the value is an operand of an operator of a path that stands in the block of the value, as it is or through casts.
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

// `i % n` on boxed numbers of which int32 is expected. In the form Numbers the operator unboxes both ways and calls the run time, on integers it does neither. Where the arms meet, an integer becomes a double again,
// so an operator that is open in both forms gains nothing by itself, and one whose code takes the box as it is asks the tag itself.
static bool loneOperatorPaysOnIntegers(Walker& walker, const BodyValue& operation, std::span<const TestOfNumber> tests)
{
    Opcode opcode = operation.opcode();
    TypeArena& types = walker.types();
    if (!hasOperatorForm(opcode) || !mayCallRuntime(opcode, OperatorForm::Numbers))
        return false;
    for (const TestOfNumber& test : tests) {
        ExpectationOfValue expectation = findExpectation(walker, test.input);
        if (!expectation.type || !types.at(types.withoutNullish(expectation.type)).isInt32())
            return false;
    }
    auto typeBehindTests = [&](ValueID input) {
        Reference reference = walker.referenceOf(input);
        TypeID proven = walker.typeOf(reference);
        return isTaggedNumber(walker, reference) ? types.intersectionOf(proven, TypeTable::int32) : proven;
    };
    TypesOfInputs behindTests(typeBehindTests);
    OperatorForm onIntegers = formAllowedForTypedOperator(walker, opcode, operation, InputsOfTypedOperation(walker, &behindTests));
    if (mayCallRuntime(opcode, onIntegers))
        return false;

    Draft onNumbers = walker.draftOf(operation);
    if (walker.hasFailed())
        return false;
    walker.chooseConservativeFields(onNumbers);
    onNumbers.setForm(OperatorForm::Numbers);
    WantedRepresentations wanted = wantedRepresentationsOf(walker, onNumbers);
    for (uint32_t operandIndex = 0; operandIndex < onNumbers.numberOfOperands(); ++operandIndex) {
        if (isTaggedNumber(walker, onNumbers.operand(operandIndex)) && !hasRegister(wanted[operandIndex]))
            return false;
    }
    return true;
}

// `(a + b) * c < d`, `(a + b) | 0` on values that are proven numbers and arrive boxed. The chain is read ahead in the body, from its first operator on:
// an operator waits where it gives a number that only operators of the chain take, and the chain ends behind an operator that takes one that waits and does not wait itself, once nothing waits.
// Between the operators stand casts, written constants and hints alone.
static std::optional<NumberPathInBody> findNumberPathInBody(Walker& walker, const BodyValue& firstOperator)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstOperator.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t begin = firstOperator.id().index() - firstValue;
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    uint32_t first = firstValue + begin;

    // By place in the chain.
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
            // A cast becomes its operand: whoever takes it takes that.
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
        else if (primitiveConstants.contains(opcode) || opcode == Opcode::ReleaseHint) {
            // Nothing of the chain takes it.
        } else {
            bool mayStandAlone = place == begin && operatorsThatMayStandAloneOnNumberPaths.contains(opcode);
            if ((!operatorsOfNumberPaths.contains(opcode) && !mayStandAlone) || numberOfOperators == mostOperatorsOfNumberPath)
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
                // `total + a * b`: int32 is asked of an operand only where its operator gets a form on integers by the answer.
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
                if (!takesOperatorThatWaits && !(mayStandAlone && loneOperatorPaysOnIntegers(walker, operation, tests.span())))
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
    if (!operatorsOfNumberPaths.contains(firstOperator.opcode()) && !operatorsThatMayStandAloneOnNumberPaths.contains(firstOperator.opcode()))
        return false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    bool takesTaggedNumber = false;
    firstOperator.forEachOperand([&](ValueID operand, uint32_t) {
        takesTaggedNumber |= isTaggedNumber(walker, walker.referenceOf(operand));
    });
    if (!takesTaggedNumber)
        return false;

    std::optional<NumberPathInBody> path = findNumberPathInBody(walker, firstOperator);
    if (!path)
        return false;
    // It guesses nothing, and it costs bytes.
    if (!shouldOpenGuess(walker, walker.referenceOf(path->tests[0].input), TypeTable::int32, true, path->numberOfOperators))
        return false;
    BlockID block = walker.currentBodyIndex().blockOf(firstOperator.id());
    return walkStretchBehindTestsOfNumbers(walker, path->tests.span(), block, path->begin, path->end, BlockFrequency::Unlikely);
}

static_assert(opcodesThatBuildArmsInFrontOfTypedFormsTakes == (operatorsOfNumberPaths | operatorsThatMayStandAloneOnNumberPaths));

bool buildArmsInFrontOfTypedFormsOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    return buildNumberPath(walker, value);
}

// ---- Sums of boxed numbers

// The taker as it is written, in its closed form, with `standIn` in the place of the sum: the rules of forms say how it takes that operand. It builds nothing.
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

    // A call that enters a function or a row of the table may take the number raw.
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

// By the body as written: the sum has one user, which stands in its block, behind casts.
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
            // `s += pair[0] + pair[1]`: the arms of the guess meet in front of whoever stands behind the stretch, and the closed arm brings a box.
            if (user->value.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked())
                return otherArmIsClosed;
            BodyValue taker = walker.asWalked(index.value(user->value));
            if (!opcodesThatStateTypes.contains(taker.opcode()))
                return takerWantsBoxInNumberPaths(walker, taker, value, standIn, typeOfSum);
            value = taker.id();
            break;
        }
        case BodyUseKind::TerminatorOperand: {
            // A copy hands what it returns to whoever goes on behind the call.
            if (user->block != blockOfSum || index.terminator(blockOfSum).kind() != TerminatorKind::Return || walker.depthOfCopies())
                return false;
            const CallingConventions& conventions = walker.plan().conventions();
            return conventions.numberOfResults(walker.unit()) == 1 && conventions.resultRepresentation(walker.unit()) == Representation::JSValue;
        }
        case BodyUseKind::EdgeArgument: {
            // `s += list[i]`, where `s` goes round the loop boxed, because for all that is proven a text may come to it. The head of a loop around the sum is built, and says how it takes what comes round.
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

// `o.total = a.x + b.y`, `pos += 2`: unboxed, each number is asked for its tag and converted, the doubles are added, and the sum is boxed again. Boxed, int32s are added as int32s.
bool sumStaysBoxed(Walker& walker, const BodyValue& written, const Draft& sum)
{
    if (!walker.has(Mechanism::UnboxedNumbers) || !allSums.contains(sum.opcode) || written.opcode() != sum.opcode)
        return false;
    if (sum.form() != OperatorForm::Numbers || sum.representation != Representation::Unchosen)
        return false;

    // As isSumOfBoxedNumbers() of the rules of forms asks, and the code of the sum takes numbers alone.
    Reference boxedNumber;
    // That an operand is a number is what a test has shown of a value that may be none: without the proof `+` is closed and may give a text. A difference beside one proven number gives a raw number on the other arm too.
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

// ---- Truncations

// `x | 0`, `x >>> 0` with x a proven number that lies boxed: the code of ToInt32 and ToUint32 looks at the tag itself, and a boxed int32 is its own answer.
bool buildTruncationOfBoxedNumber(Walker& walker, const BodyValue& value)
{
    Opcode opcode = value.opcode();
    if (!operatorsWithZeroAsIdentityInNumberPaths.contains(opcode) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    static_assert(!OperandIndex::BitOr::left && OperandIndex::BitOr::right == 1 && !OperandIndex::URShift::left && OperandIndex::URShift::right == 1);
    auto isZero = [&](Reference operand) {
        Type type = walker.types().at(walker.typeOf(operand));
        return type.isInteger() && type.integers == Range::one(0);
    };
    Reference left = walker.referenceOf(value.operand(0));
    Reference right = walker.referenceOf(value.operand(1));
    if (!left || !right)
        return false;
    Reference number;
    if (isZero(right))
        number = left;
    else if (commutativeAmongOperatorsWithZeroAsIdentity.contains(opcode) && isZero(left))
        number = right;
    if (!number || !isTaggedNumber(walker, number))
        return false;

    Draft written = walker.draftOf(value);
    if (walker.hasFailed())
        return false;
    Draft conversion(opcode == Opcode::URShift ? Opcode::ToUint32 : Opcode::ToInt32, written.origin());
    conversion.expected = written.expected;
    conversion.inputValueGiven = value.id();
    walker.chooseConservativeFields(conversion);
    conversion.setValueOperand(number);
    Origin origin = conversion.origin();
    walker.bind(value.id(), boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(conversion)), origin));
    return true;
}

// ---- Arithmetic beside one proven number

// `x * 2`, `x - 1`, `x / 2` with x unknown, where no guess was opened: the stub takes the number raw, asks the other side alone and hands back a raw number, so what follows stays unboxed.
// `"" + x`: a string is handed back as it is.
static_assert(opcodesThatBuildOperatorBesideOneNumberTakes == (operatorsBesideOneNumber));

bool buildOperatorBesideOneNumberOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!operatorsBesideOneNumber.contains(value.opcode()) || !walker.has(Mechanism::TypedOperations) || walker.isUnreachable())
        return false;
    OperatorForm form = formAllowedForTypedOperator(walker, value.opcode(), value, InputsOfTypedOperation(walker, nullptr));
    bool takesNumberRaw = form == OperatorForm::NumberAndValue && walker.has(Mechanism::UnboxedNumbers);
    if (!takesNumberRaw && form != OperatorForm::MayBeString)
        return false;

    Draft draft = walker.draftOf(value);
    if (walker.hasFailed())
        return false;
    walker.chooseConservativeFields(draft);
    draft.setForm(form);
    Origin origin = draft.origin();
    walker.bind(value.id(), boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(draft)), origin));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
