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
Reference checkIntegerBeforeLoop(Walker&, Reference value, TypeID tested, Origin, StatisticID counted);
bool isElementIndex(const BodyIndex&, ValueID);

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
static constexpr uint32_t maxUsersOfBoxedSum = 3;
static constexpr uint32_t maxOperationsToBoxedSumUsers = 16;

namespace {

enum class KindOnNumberPath : uint8_t {
    Foreign,
    Integer,
    Tagged,
    Read,
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
    Vector<ValueID, 8> reads;
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

static std::optional<NumberPathInBody> findNumberPathInBody(Walker& walker, ValueID firstOperation, ValueID readBefore = { })
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstOperation));
    uint32_t firstValue = block.firstValue().index();
    uint32_t begin = firstOperation.index() - firstValue;
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    uint32_t first = firstValue + begin;

    Vector<OperandOnNumberPath, 32> given;
    Vector<uint32_t, 32> mentionsLeft;
    uint32_t mentionsLeftOfAll = 0;
    uint32_t numberOfOperators = 0;
    Vector<TestOfNumber, 8> tests;
    Vector<Reference, 8> tested;
    Vector<ValueID, 8> reads;
    NumberPathInBody path { begin, begin, 0, { }, { } };

    auto kindOf = [&](ValueID operand) -> OperandOnNumberPath {
        if (operand.index() >= first)
            return given[operand.index() - first];
        if (operand == readBefore)
            return { KindOnNumberPath::Read, operand, 0 };
        Reference reference = walker.referenceOf(operand);
        if (!reference || !reference.isRecord())
            return { };
        if (isTaggedNumber(walker, reference))
            return { KindOnNumberPath::Tagged, operand, 0 };
        return { walker.types().at(walker.typeOf(reference)).isInteger() ? KindOnNumberPath::Integer : KindOnNumberPath::Foreign, { }, 0 };
    };
    auto typeOnPath = [&](ValueID input) -> TypeID {
        if (input.index() >= first)
            return input.index() - first < given.size() && given[input.index() - first].kind != KindOnNumberPath::Foreign ? TypeID(TypeTable::number) : TypeID();
        if (input == readBefore)
            return TypeTable::number;
        Reference reference = walker.referenceOf(input);
        return reference && reference.isRecord() ? walker.typeOf(reference) : TypeID();
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
        } else if (readBefore && opcode == Opcode::PropGet && operation.objectOperand().index() < first && readsNumberFromSite(walker, operation, TypesOfInputs(typeOnPath)))
            result = { KindOnNumberPath::Read, operation.id(), false, 0 };
        else {
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
                takesTaggedNumber |= operands.last().kind == KindOnNumberPath::Tagged || operands.last().kind == KindOnNumberPath::Read;
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
                if ((operand.kind != KindOnNumberPath::Tagged && operand.kind != KindOnNumberPath::Read) || takesDoubleOnIntegersToo)
                    continue;
                Reference reference = operand.kind == KindOnNumberPath::Read ? Reference() : walker.referenceOf(operand.tagged);
                if (operand.kind == KindOnNumberPath::Read ? reads.contains(operand.tagged) : tested.contains(reference))
                    continue;
                if (tested.size() + reads.size() == mostNumbersTestedOnNumberPath) {
                    testsTooManyNumbers = true;
                    break;
                }
                if (operand.kind == KindOnNumberPath::Read) {
                    reads.append(operand.tagged);
                    continue;
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
                    path.reads = reads;
                }
            }
        }
        given.append(result);
        mentionsLeft.append(mentionsOfResult);
    }
    if (path.end == path.begin || (path.tests.isEmpty() && path.reads.isEmpty()))
        return std::nullopt;
    return path;
}

static constexpr OpcodeSet binarySumsInNumberPaths { Opcode::Add, Opcode::Sub };

static std::optional<uint32_t> truncatedSumsEnd(Walker& walker, const BodyValue& firstOperator)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(firstOperator.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    Vector<ValueID, 16> sums { firstOperator.id() };
    uint32_t mentionsLeft = index.useCount(firstOperator.id());
    for (uint32_t place = firstOperator.id().index() - firstValue + 1; place < limit && mentionsLeft && mentionsLeft < BodyIndex::maxCountedUses; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        uint32_t mentions = 0;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += sums.contains(operand);
        });
        if (!mentions)
            continue;
        mentionsLeft -= mentions;
        if (binarySumsInNumberPaths.contains(operation.opcode()) || opcodesThatStateTypes.contains(operation.opcode())) {
            bool takesLaterElement = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) {
                ValueID made = index.stripCasts(operand);
                takesLaterElement |= !sums.contains(operand) && made.index() > firstOperator.id().index() && !index.isParameter(made) && index.opcodeOf(made) == Opcode::ElemGet;
            });
            if (takesLaterElement)
                return std::nullopt;
            sums.append(operation.id());
            mentionsLeft += index.useCount(operation.id());
        } else if (!allBitwiseOperators.contains(operation.opcode()))
            return std::nullopt;
        if (!mentionsLeft)
            return place + 1;
    }
    return std::nullopt;
}

static bool buildTruncatedSums(Walker& walker, const BodyValue& firstOperator)
{
    if (!binarySumsInNumberPaths.contains(firstOperator.opcode()) || !walker.isInLoop() || !walker.opensGuesses())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    for (uint32_t place : { 0u, 1u }) {
        ValueID written = firstOperator.operand(place);
        ValueID read = index.stripCasts(written);
        Reference term = walker.referenceOf(written);
        Reference other = walker.referenceOf(firstOperator.operand(1 - place));
        if (!term || !term.isValue() || !other || !other.isRecord() || walker.representationOf(term) != Representation::Double || !types.at(walker.typeOf(other)).isInteger())
            continue;
        Type type = types.at(walker.typeOf(term));
        if (!type.isNumber() || !(type.primitives & Primitive::FractionalNumber) || index.isParameter(read) || index.opcodeOf(read) != Opcode::ElemGet || findTruncatedUse(walker, read) != TruncatedUse::TermOfSums)
            continue;
        std::optional<uint32_t> end = truncatedSumsEnd(walker, firstOperator);
        if (!end)
            return false;
        BlockID block = index.blockOf(firstOperator.id());
        TestOfNumber test { written, types.integer(integersAddedWithoutRounding) };
        return walkStretchBehindTestsOfNumbers(walker, std::span<const TestOfNumber>(&test, 1), block, firstOperator.id().index() - index.block(block).firstValue().index(), *end, BlockFrequency::Rare);
    }
    return false;
}

static bool isUsedWithDouble(Walker& walker, ValueID value)
{
    static constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Mul };
    static constexpr unsigned maxSteps = 16;
    const BodyIndex* index = &walker.currentBodyIndex();
    size_t outward = 0;
    for (unsigned step = 0; step < maxSteps; ++step) {
        ValueID user = index->onlyUsingValue(value);
        if (!user) {
            BodyTerminator end = index->block(index->blockOf(value)).terminator();
            if (!index->hasOneUse(value) || end.kind() != TerminatorKind::Return || end.operand(0) != value)
                return false;
            std::optional<Walker::CallOfCopy> copied = walker.callOfCopy(outward);
            if (!copied)
                return false;
            ++outward;
            index = copied->index;
            value = copied->call;
            continue;
        }
        BodyValue operation = index->value(user);
        if (opcodesThatStateTypes.contains(operation.opcode())) {
            value = user;
            continue;
        }
        if (operation.opcode() == Opcode::Div)
            return true;
        if (!steps.contains(operation.opcode()))
            return false;
        ValueID other = operation.operand(operation.operand(0) == value ? 1 : 0);
        ValueID made = index->stripCasts(other);
        if (made && !index->isParameter(made) && index->opcodeOf(made) == Opcode::Double)
            return true;
        bool isInteger = other == value || (made && !index->isParameter(made) && index->opcodeOf(made) == Opcode::Integer);
        if (!isInteger) {
            Reference built = outward ? walker.referenceInCaller(other, outward - 1) : walker.referenceOf(other);
            return built && built.isValue() && walker.representationOf(built) == Representation::Double;
        }
        value = user;
    }
    return false;
}

static bool isInvariantPathOfDoubles(Walker& walker, const BodyBlock& block, const NumberPathInBody& path)
{
    std::optional<uint32_t> loop = walker.innermostOpenLoop();
    if (!loop)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t first = block.firstValue().index() + path.begin;
    uint32_t end = block.firstValue().index() + path.end;
    for (uint32_t number = first; number < end; ++number) {
        bool isInvariant = true;
        index.value(ValueID(number)).forEachOperand([&](ValueID operand, uint32_t) {
            Reference taken = operand.index() < first ? walker.referenceOf(operand) : Reference();
            isInvariant &= !taken || walker.standsInFrontOfOpenLoop(*loop, taken);
        });
        if (!isInvariant)
            return false;
    }
    return isUsedWithDouble(walker, ValueID(end - 1));
}

static bool hoistTestsOfIndex(Walker& walker, const NumberPathInBody& path, ValueID last, Origin origin)
{
    if (!walker.has(Mechanism::Guesses) || !walker.opensGuesses() || !isElementIndex(walker.currentBodyIndex(), last))
        return false;
    for (const TestOfNumber& test : path.tests) {
        Reference number = walker.referenceOf(test.input);
        if (test.tested != TypeTable::int32 || !number || !number.isValue() || !walker.isInvariantInSpecializedVersion(number, Walker::IterationPurity::KnownEffectsOnly))
            return false;
    }
    for (const TestOfNumber& test : path.tests) {
        Reference number = walker.referenceOf(test.input);
        if (walker.types().at(walker.typeOf(number)).isInteger())
            continue;
        Reference integer = checkIntegerBeforeLoop(walker, number, TypeTable::int32, origin, StatisticID::IndexNumberGuardsHoisted);
        if (!integer)
            return false;
        if (!walker.state().checkedNameOf(number))
            walker.state().noteCheckedName(number, integer);
    }
    return true;
}

static bool buildNumberPath(Walker& walker, const BodyValue& firstOperator)
{
    if (!operatorsOfNumberPaths.contains(firstOperator.opcode()))
        return false;
    if (!walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    if (buildTruncatedSums(walker, firstOperator))
        return true;
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

    std::optional<NumberPathInBody> path = findNumberPathInBody(walker, firstOperator.id());
    if (!path)
        return false;
    if (!shouldOpenGuess(walker, walker.referenceOf(path->tests[0].input), TypeTable::int32, true, path->numberOfOperators))
        return false;
    BlockID block = walker.currentBodyIndex().blockOf(firstOperator.id());
    ValueID last(walker.currentBodyIndex().block(block).firstValue().index() + path->end - 1);
    if (hoistTestsOfIndex(walker, *path, last, walker.originOf(firstOperator)) || walker.hasFailed())
        return false;
    if (isInvariantPathOfDoubles(walker, walker.currentBodyIndex().block(block), *path)) {
        walker.count(StatisticID::InvariantPathsOfDoubles);
        return false;
    }
    return walkStretchBehindTestsOfNumbers(walker, path->tests.span(), block, path->begin, path->end, BlockFrequency::Unlikely);
}

bool walkNumberPathAfterRead(Walker& walker, NumberReadBeforeArms&& firstRead)
{
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID read = firstRead.read;
    if (walker.isBuildingInPlace() || !walker.has(Mechanism::UnboxedNumbers) || !firstRead.found.isValue() || walker.inputBlockBeingWalked() != index.blockOf(read) || !index.onlyUsingValue(read))
        return false;
    BodyBlock block = index.block(index.blockOf(read));
    ValueID next(read.index() + 1);
    if (!block.holds(next))
        return false;
    std::optional<NumberPathInBody> path = findNumberPathInBody(walker, next, read);
    if (!path || !path->reads.contains(read) || !shouldOpenGuess(walker, firstRead.found, TypeTable::int32, true, path->numberOfOperators))
        return false;

    Vector<NumberReadBeforeArms, 8> reads;
    walker.bind(read, firstRead.found);
    reads.append(WTF::move(firstRead));
    for (ValueID later : path->reads) {
        if (later == read)
            continue;
        reads.append(readAtSite(walker, index.value(later)));
        if (!reads.last().found || !reads.last().found.isValue() || walker.hasFailed() || walker.isUnreachable())
            return true;
    }
    walker.count(StatisticID::NumberPathsAfterReads);
    walkStretchBehindTestsOfNumbers(walker, path->tests.span(), block.id(), path->begin, path->end, BlockFrequency::Unlikely, EndOfNumberArms::Meet, reads.span());
    walker.bind(read, Reference());
    return true;
}

static bool buildComparisonAfterUndefinedTests(Walker& walker, const BodyValue& comparison)
{
    bool takesOptionalInteger = false;
    comparison.forEachOperand([&](ValueID operand, uint32_t) {
        Reference value = walker.referenceOf(operand);
        takesOptionalInteger |= value && value.isValue() && walker.representationOf(value) == Representation::OptionalInt64;
    });
    if (!takesOptionalInteger || !walker.isInLoop() || !walker.opensGuesses() || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    TypeArena& types = walker.types();
    Vector<TestOfNumber, 2> tests;
    bool takesOtherValue = false;
    comparison.forEachOperand([&](ValueID operand, uint32_t) {
        Reference value = walker.referenceOf(operand);
        TypeID type = value && value.isRecord() ? walker.typeOf(value) : TypeID();
        if (!type || takesOtherValue) {
            takesOtherValue = true;
            return;
        }
        if (types.isSubset(type, TypeTable::int32))
            return;
        TypeID integers = types.withoutPrimitives(type, Primitive::Undefined);
        takesOtherValue = !value.isValue() || walker.representationOf(value) != Representation::OptionalInt64 || !types.isSubset(integers, TypeTable::int32) || isNeverType(integers);
        ValueID read = takesOtherValue ? ValueID() : value.id();
        while (read && valuePreservingOpcodes.contains(walker.code().at(read).opcode))
            read = walker.code().at(read).operand(0);
        takesOtherValue |= !read || walker.code().isConstant(read) || walker.code().at(read).opcode != Opcode::GetTypedArrayElement;
        if (!takesOtherValue)
            tests.append(TestOfNumber { operand, integers });
    });
    if (takesOtherValue || tests.isEmpty())
        return false;
    walker.noteComparisonOfElementsOfViews();
    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(comparison.id()));
    uint32_t begin = comparison.id().index() - block.firstValue().index();
    return walkStretchBehindTestsOfNumbers(walker, tests.span(), block.id(), begin, begin + 1, BlockFrequency::Rare, EndOfNumberArms::MayTakeBranch);
}

static bool buildBitsOfUnknownValues(Walker& walker, const BodyValue& firstOperator)
{
    static constexpr size_t mostValuesChecked = 4;
    static constexpr uint32_t fewestMentionsInGenericVersion = 4;
    if (!allBitwiseOperators.contains(firstOperator.opcode()) || !walker.isInLoop() || walker.canRestartIteration() || !walker.has(Mechanism::TypedOperations) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BodyBlock block = index.block(index.blockOf(firstOperator.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t begin = firstOperator.id().index() - firstValue;
    uint32_t limit = limitOfStretchOfNumbers(walker, block);
    uint32_t end = begin;
    uint32_t numberOfOperators = 0;
    uint32_t mentionsOfChecked = 0;
    Vector<Reference, mostValuesChecked> checked;
    Vector<TestOfNumber, mostValuesChecked> tests;
    auto takesNumber = [&](ValueID operand) {
        if (operand.index() >= firstOperator.id().index())
            return true;
        Reference value = walker.referenceOf(operand);
        if (!value || !value.isRecord())
            return false;
        TypeID type = walker.typeOf(value);
        if (types.at(type).isNumber())
            return true;
        if (!value.isValue() || !types.isUninformative(type) || walker.representationOf(value) != Representation::JSValue)
            return false;
        ++mentionsOfChecked;
        if (checked.contains(value))
            return true;
        if (checked.size() == mostValuesChecked)
            return false;
        checked.append(value);
        tests.append(TestOfNumber { operand, TypeTable::number });
        return true;
    };
    for (uint32_t place = begin; place < limit; ++place) {
        BodyValue operation = index.value(ValueID(firstValue + place));
        if (operation.opcode() == Opcode::Integer)
            continue;
        if (!allBitwiseOperators.contains(operation.opcode()))
            break;
        size_t checkedBefore = checked.size();
        uint32_t mentionsBefore = mentionsOfChecked;
        bool takesOnlyNumbers = true;
        operation.forEachOperand([&](ValueID operand, uint32_t) {
            takesOnlyNumbers &= takesNumber(operand);
        });
        if (!takesOnlyNumbers) {
            checked.shrink(checkedBefore);
            tests.shrink(checkedBefore);
            mentionsOfChecked = mentionsBefore;
            break;
        }
        ++numberOfOperators;
        end = place + 1;
        if (place == begin && checked.isEmpty())
            return false;
    }
    if (mentionsOfChecked <= tests.size() || (walker.opensGuesses() ? !shouldOpenGuess(walker, checked[0], TypeTable::number, true, numberOfOperators) : mentionsOfChecked < fewestMentionsInGenericVersion))
        return false;
    walker.count(StatisticID::BitsOfUnknownValuesChecked);
    SetForScope checksBits(walker.checksBitsOfUnknownValues(), true);
    return walkStretchBehindTestsOfNumbers(walker, tests.span(), block.id(), begin, end, BlockFrequency::Rare);
}

static_assert(opcodesThatBuildArmsInFrontOfTypedFormsTakes == (operatorsOfNumberPaths));

bool buildArmsInFrontOfTypedFormsOfItsOpcodes(Walker& walker, const BodyValue& value)
{
    if (!walker.checksBitsOfUnknownValues() && buildBitsOfUnknownValues(walker, value))
        return true;
    if (!walker.opensGuesses())
        return false;
    return buildNumberPath(walker, value) || (allRelations.contains(value.opcode()) && buildComparisonAfterUndefinedTests(walker, value));
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
            if ((otherArmIsClosed || !walker.isInLoop()) && user->value.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked())
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

static bool isStoredBoxedAndCompared(Walker& walker, const BodyValue& written, Reference standIn, TypeID typeOfSum)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const uint32_t usersLeft = index.useCount(written.id());
    if (usersLeft < 2 || usersLeft > maxUsersOfBoxedSum || !walker.runsInIterations())
        return false;
    BodyBlock block = index.block(index.blockOf(written.id()));
    const uint32_t end = std::min(block.firstValue().index() + walker.endOfStretchBeingWalked(), written.id().index() + 1 + maxOperationsToBoxedSumUsers);
    uint32_t users = 0;
    bool isStored = false;
    for (uint32_t place = written.id().index() + 1; place < end && users < usersLeft; ++place) {
        BodyValue user = walker.asWalked(index.value(ValueID(place)));
        uint32_t mentions = 0;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            mentions += operand == written.id();
        });
        if (!mentions)
            continue;
        users += mentions;
        if (allRelations.contains(user.opcode())) {
            if (mentions > 1)
                return false;
            continue;
        }
        if (mentions > 1 || callsAmongTakersOfBoxes.contains(user.opcode()) || !takerWantsBoxInNumberPaths(walker, user, written.id(), standIn, typeOfSum))
            return false;
        isStored = true;
    }
    if (users != usersLeft || !isStored)
        return false;
    walker.count(StatisticID::ComparedSumsStayBoxed);
    return true;
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
    TypeID typeOfSum = walker.resultTypeOf(sum);
    return onlyUserTakesBoxInNumberPaths(walker, written, boxedNumber, typeOfSum, otherArmIsClosed) || isStoredBoxedAndCompared(walker, written, boxedNumber, typeOfSum);
}

bool keepsDifferenceBoxed(Walker& walker, const BodyValue& written, const Draft& difference)
{
    if (written.opcode() != Opcode::Sub || walker.isInLoop() || !walker.has(Mechanism::UnboxedNumbers))
        return false;
    TypeArena& types = walker.types();
    uint32_t numberIndex = types.at(walker.typeOf(difference.operand(0))).isNumber() ? 0 : 1;
    Reference value = difference.operand(1 - numberIndex);
    if (!types.at(walker.typeOf(difference.operand(numberIndex))).isInt32() || !value.isValue() || walker.representationOf(value) != Representation::JSValue || !types.overlap(walker.typeOf(value), TypeTable::int32))
        return false;
    if (!onlyUserTakesBoxInNumberPaths(walker, written, value, walker.resultTypeOf(difference), false))
        return false;
    walker.count(StatisticID::BoxedDifferences);
    return true;
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
    if (form == OperatorForm::NumberAndValue && draft.opcode == Opcode::Sub && keepsDifferenceBoxed(walker, value, draft)) [[unlikely]]
        form = OperatorForm::Generic;
    draft.setForm(form);
    Origin origin = draft.origin();
    walker.bind(value.id(), boxNumberIfNeeded(walker, walker.build(WTF::move(draft)), origin));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
