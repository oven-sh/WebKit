/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOperatorForms.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTWalkNumberGuesses.h"
#include <limits>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
OperatorForm operatorFormFor(Walker&, Opcode, const BodyValue&, const InputsOfTypedOperation&);
Reference checkIntegerBeforeLoop(Walker&, Reference value, TypeID tested, Origin, StatisticID counted);

Representation representationOfNumber(const Walker& walker, TypeID type)
{
    if (!type || !walker.has(Mechanism::UnboxedNumbers))
        return Representation::JSValue;
    Representation natural = representationOf(TypeReader(walker.types()), type);
    return numberRepresentations.contains(natural) ? natural : Representation::JSValue;
}

Reference boxNumberIfNeeded(Walker& walker, Reference number, Origin origin)
{
    if (!number.isValue())
        return number;
    Representation held = walker.representationOf(number);
    TypeID type = walker.typeOf(number);
    if (!numberRepresentations.contains(held) || representationOfNumber(walker, type) != Representation::JSValue)
        return number;

    std::optional<ConversionPath> path = findConversion(walker.types(), type, held, Representation::JSValue);
    if (!path)
        return number;
    for (ConversionStep step : *path) {
        Draft conversion(step.opcode, origin);
        conversion.setOperand(0, number);
        conversion.representation = step.to;
        number = walker.build(WTF::move(conversion));
    }
    return number;
}

bool isTakenAsItIsByFormNumbers(const Type& type)
{
    return type.isNumberOrNullish() || type.isUndefinedOrNull();
}

static bool isClosedFormOfOperatorOnNumbers(Opcode opcode, OperatorForm form)
{
    bool takesAnyValue = form == OperatorForm::Generic || form == OperatorForm::NumberAndValue || form == OperatorForm::MayBeString;
    return takesAnyValue && !openUnderOf(opcode).contains(stateOf(form));
}

static Reference convertedForFormNumbers(Walker& walker, Reference operand, Origin origin)
{
    Draft conversion(Opcode::ToNumber, origin);
    conversion.setValueOperand(operand);
    walker.chooseConservativeFields(conversion);
    conversion.setForm(operatorFormFor(walker, conversion));
    return walker.build(WTF::move(conversion));
}

void prepareOperandsForFormNumbers(Walker& walker, Draft& draft)
{
    Origin origin = draft.origin();
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfOperands(); ++operandIndex) {
        Reference operand = draft.operand(operandIndex);
        const Type& type = walker.types().at(walker.typeOf(operand));
        bool isConvertedBeforeLoop = (type.primitives & Primitive::Number) && !type.isNumber() && operand.isValue() && walker.has(Mechanism::LoopInvariants) && walker.isInvariantInInnermostLoop(operand);
        if (isConvertedBeforeLoop || !isTakenAsItIsByFormNumbers(type))
            draft.setOperand(operandIndex, convertedForFormNumbers(walker, operand, origin));
    }
}

static Reference buildRemainderByReciprocalMadeInFrontOfLoop(Walker& walker, const Draft& remainder)
{
    constexpr Range dividendsOfReciprocal { 0, std::numeric_limits<uint32_t>::max() };
    constexpr Range divisorsOfReciprocal { 1, std::numeric_limits<int32_t>::max() };
    std::optional<uint32_t> loop = walker.has(Mechanism::LoopInvariants) ? walker.innermostOpenLoop() : std::nullopt;
    Reference dividend = remainder.operand(OperandIndex::Mod::left);
    Reference divisor = remainder.operand(OperandIndex::Mod::right);
    if (!loop || !divisor.isValue() || !walker.standsInFrontOfOpenLoop(*loop, divisor) || walker.standsInFrontOfOpenLoop(*loop, dividend))
        return { };
    Type dividends = walker.types().at(walker.typeOf(dividend));
    Type divisors = walker.types().at(walker.typeOf(divisor));
    if (!dividends.isInteger() || (!dividends.integers.isSubsetOf(dividendsOfReciprocal) && !walker.shouldAssumeUInt32Dividends()) || !divisors.isInteger() || !divisors.integers.isSubsetOf(divisorsOfReciprocal) || divisors.integers.isSingleValue())
        return { };
    static constexpr uint32_t maxDividingIterations = 4;
    if (std::optional<uint32_t> turns = walker.constantTripCountOf(*loop); turns && *turns <= maxDividingIterations)
        return { };

    Draft inFront(Opcode::ReciprocalOfDivisor, remainder.origin());
    walker.chooseConservativeFields(inFront);
    inFront.setDivisorOperand(divisor);
    Reference reciprocal = walker.build(WTF::move(inFront));
    if (!reciprocal || !walker.standsInFrontOfOpenLoop(*loop, reciprocal))
        return { };

    Draft byReciprocal(Opcode::RemainderByReciprocal, remainder.origin());
    walker.chooseConservativeFields(byReciprocal);
    byReciprocal.setDividendOperand(dividend);
    byReciprocal.setDivisorOperand(divisor);
    byReciprocal.setReciprocalOperand(reciprocal);
    Reference built = walker.build(WTF::move(byReciprocal));
    if (built)
        walker.count(StatisticID::RemaindersByHoistedReciprocal);
    return built;
}

static Reference buildRemainderByWholeDivisor(Walker& walker, const Draft& remainder)
{
    std::optional<uint32_t> loop = walker.has(Mechanism::LoopInvariants) ? walker.innermostOpenLoop() : std::nullopt;
    Reference dividend = remainder.operand(OperandIndex::Mod::left);
    Reference divisor = remainder.operand(OperandIndex::Mod::right);
    if (!loop || !divisor.isValue() || !walker.standsInFrontOfOpenLoop(*loop, divisor) || walker.standsInFrontOfOpenLoop(*loop, dividend))
        return { };
    const Type& typeOfDivisor = walker.types().at(walker.typeOf(divisor));
    if (!walker.types().at(walker.typeOf(dividend)).isNumber() || !typeOfDivisor.isNumber() || typeOfDivisor.isInteger())
        return { };

    Draft inFront(Opcode::WholeDivisor, remainder.origin());
    walker.chooseConservativeFields(inFront);
    inFront.setDivisorOperand(divisor);
    Reference wholeDivisor = walker.build(WTF::move(inFront));
    if (!wholeDivisor || !walker.standsInFrontOfOpenLoop(*loop, wholeDivisor))
        return { };

    Draft byWholeDivisor(Opcode::RemainderByWholeDivisor, remainder.origin());
    walker.chooseConservativeFields(byWholeDivisor);
    byWholeDivisor.setDividendOperand(dividend);
    byWholeDivisor.setDivisorOperand(divisor);
    byWholeDivisor.setWholeDivisorOperand(wholeDivisor);
    return walker.build(WTF::move(byWholeDivisor));
}

bool isElementIndex(const BodyIndex& index, ValueID value)
{
    ValueID used = value;
    for (ValueID user = index.onlyUsingValue(used); user; user = index.onlyUsingValue(used)) {
        BodyValue operation = index.value(user);
        if (operation.opcode() == Opcode::ElemGet || operation.opcode() == Opcode::ElemSet)
            return operation.propertyOperand() == used;
        if (!opcodesThatStateTypes.contains(operation.opcode()))
            return false;
        used = user;
    }
    return false;
}

static bool hoistOperandCheck(Walker& walker, Draft& draft, ValueID written)
{
    static constexpr OpcodeSet operators { Opcode::Add, Opcode::Sub, Opcode::Mul };
    if (!operators.contains(draft.opcode) || !walker.has(Mechanism::Guesses) || !walker.opensGuesses())
        return false;
    TypeArena& types = walker.types();
    for (uint32_t side : { 0u, 1u }) {
        Reference operand = draft.operand(side);
        Reference other = draft.operand(1 - side);
        if (!operand || !other || !operand.isValue() || !types.at(walker.typeOf(other)).isInteger())
            continue;
        Type type = types.at(walker.typeOf(operand));
        bool isWholeAddendOfIndex = draft.opcode != Opcode::Mul && type.isNumberWithoutFraction() && !type.isInteger() && walker.representationOf(operand) == Representation::Double && types.at(walker.typeOf(other)).isInt32() && isElementIndex(walker.currentBodyIndex(), written);
        if (!isWholeAddendOfIndex && (!(type.primitives & Primitive::Integer) || !type.hasOnlyPrimitives(Primitive::Integer | Primitive::Undefined | Primitive::Absent) || type.isInteger()))
            continue;
        if (!walker.isInvariantInSpecializedVersion(operand, Walker::IterationPurity::KnownEffectsOnly))
            return false;
        Reference integer = checkIntegerBeforeLoop(walker, operand, isWholeAddendOfIndex ? TypeID(TypeTable::int32) : types.integer(type.integers), draft.origin(), isWholeAddendOfIndex ? StatisticID::IndexAddendGuardsHoisted : StatisticID::InvariantNumberGuardsHoisted);
        if (!integer)
            return false;
        if (!walker.state().checkedNameOf(operand))
            walker.state().noteCheckedName(operand, integer);
        draft.setOperand(side, integer);
        return true;
    }
    return false;
}

static bool isPassedRoundOwnCycle(const Walker& walker, ValueID number)
{
    const LinkTables& linked = walker.facts().linked();
    FunctionID function = walker.currentFunction();
    if (!function || function.index() >= linked.functions.size())
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    uint32_t cycle = linked.function(function).cycle;
    return std::ranges::any_of(linked.callsOf(function), [&](const LinkedCall& call) {
        if (!call.isInTailPosition || call.callee.index() >= linked.functions.size() || linked.function(call.callee).cycle != cycle)
            return false;
        bool isPassed = false;
        index.value(call.site).forEachOperand([&](ValueID operand, uint32_t) {
            isPassed |= operand && index.stripCasts(operand) == number;
        });
        return isPassed;
    });
}

bool stepsParameterRoundCycle(Walker& walker)
{
    static constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    const BodyIndex& index = walker.currentBodyIndex();
    const LinkTables& linked = walker.facts().linked();
    FunctionID function = walker.currentFunction();
    if (!walker.has(Mechanism::UnboxedNumbers) || !index.holdsAnyOf(steps) || !function || function.index() >= linked.functions.size())
        return false;
    uint32_t cycle = linked.function(function).cycle;
    bool callsOwnCycle = std::ranges::any_of(linked.callsOf(function), [&](const LinkedCall& call) {
        return call.isInTailPosition && call.callee.index() < linked.functions.size() && linked.function(call.callee).cycle == cycle;
    });
    if (!callsOwnCycle)
        return false;
    TypeArena& types = walker.types();
    auto isExactParameter = [&](ValueID operand) {
        ValueID parameter = operand ? index.stripCasts(operand) : ValueID();
        if (!parameter || !index.isParameter(parameter) || index.blockOf(parameter) != entryBlock)
            return false;
        Reference arriving = walker.referenceBoundTo(parameter);
        if (!arriving || !arriving.isValue() || walker.code().isConstant(arriving.id()) || walker.representationOf(arriving) != Representation::Int64)
            return false;
        Type type = types.at(walker.typeOf(arriving));
        return type.isInteger() && type.integers.isSubsetOf(Range::exact()) && type.integers.minimum < 0;
    };
    for (uint32_t number = 0; number < index.numberOfBlocks(); ++number) {
        if (!index.isReachable(BlockID(number)))
            continue;
        for (BodyValue operation : index.block(BlockID(number)).values()) {
            if (!steps.contains(operation.opcode()))
                continue;
            bool stepsParameter = false;
            operation.forEachOperand([&](ValueID operand, uint32_t) { stepsParameter |= isExactParameter(operand); });
            if (stepsParameter && isPassedRoundOwnCycle(walker, operation.id()))
                return true;
        }
    }
    return false;
}

static Reference buildStepOfExactInteger(Walker& walker, const BodyValue& value, const Draft& draft)
{
    static constexpr OpcodeSet steps { Opcode::Add, Opcode::Sub, Opcode::Increment, Opcode::Decrement };
    if (!steps.contains(draft.opcode) || !walker.has(Mechanism::UnboxedNumbers) || walker.isUnreachable() || walker.code().at(walker.currentBlock()).frequency == BlockFrequency::Rare)
        return { };
    TypeArena& types = walker.types();
    auto stepIn = [&](Reference operand) -> std::optional<int64_t> {
        Type type = operand ? types.at(walker.typeOf(operand)) : Type();
        if (!operand || !type.isInteger() || !type.integers.isSingleValue() || std::abs(type.integers.minimum) != 1)
            return std::nullopt;
        return type.integers.minimum;
    };
    uint32_t side = 0;
    int64_t step = draft.opcode == Opcode::Decrement ? -1 : 1;
    if (draft.opcode == Opcode::Add || draft.opcode == Opcode::Sub) {
        std::optional<int64_t> written = stepIn(draft.operand(1));
        if (!written && draft.opcode == Opcode::Add) {
            side = 1;
            written = stepIn(draft.operand(0));
        }
        if (!written)
            return { };
        step = draft.opcode == Opcode::Sub ? -*written : *written;
    }
    Reference stepped = draft.operand(side);
    if (!stepped || !stepped.isValue() || walker.code().isConstant(stepped.id()) || walker.representationOf(stepped) != Representation::Int64)
        return { };
    Type type = types.at(walker.typeOf(stepped));
    if (!type.isInteger() || !type.integers.isSubsetOf(Range::exact()) || type.integers.minimum >= 0 || !types.overlap(walker.typeOf(stepped), types.integer(Range { -step, std::numeric_limits<int32_t>::max() - step })) || !isPassedRoundOwnCycle(walker, value.id()))
        return { };

    Origin origin = draft.origin();
    Draft onDoubles = draft;
    walker.chooseConservativeFields(onDoubles);
    onDoubles.setForm(OperatorForm::CheckedIntegers);
    TypeID resultType = walker.resultTypeOf(onDoubles);
    if (!resultType || !types.at(resultType).isInteger() || !types.at(resultType).integers.isSubsetOf(Range::exact()))
        return { };
    Draft test(Opcode::TypeTest, origin);
    walker.chooseConservativeFields(test);
    test.setValueOperand(stepped);
    test.setTestedType(types.integer(Range { -step, std::numeric_limits<int32_t>::max() - step }));
    Reference isFar = walker.build(WTF::move(test));
    if (!isFar || !isFar.isValue() || walker.isUnreachable())
        return { };
    BlockID passing = walker.addBlock();
    auto buildOnIntegers = [&]() -> Reference {
        walker.continueIn(passing);
        Draft narrowed(Opcode::Narrowed, origin);
        narrowed.setValueOperand(stepped);
        narrowed.setTestOperand(isFar);
        narrowed.setOperandIndex(OperandIndex::TypeTest::value);
        narrowed.setIsOnTakenEdge(true);
        Draft onIntegers = draft;
        onIntegers.setOperand(side, walker.build(WTF::move(narrowed)));
        if (!onIntegers.operand(side))
            return { };
        walker.chooseConservativeFields(onIntegers);
        onIntegers.setForm(operatorFormFor(walker, onIntegers));
        return walker.build(WTF::move(onIntegers));
    };
    if (walker.canGuardRecursion()) {
        walker.branch(isFar, passing, walker.elementGuardFallback());
        walker.recordSpeculation();
        return buildOnIntegers();
    }
    BlockID failing = walker.addBlock(BlockFrequency::Rare);
    BlockID meeting = walker.addBlock();
    Reference met = walker.addParameter(meeting, resultType, Representation::Int64);
    TypeID shown;
    walker.branch(isFar, passing, failing);
    if (Reference result = buildOnIntegers()) {
        shown = walker.typeShown(result);
        walker.jumpTo(meeting, singleElementSpan(result));
    }
    walker.continueIn(failing);
    if (Reference result = walker.isUnreachable() ? Reference() : walker.build(WTF::move(onDoubles))) {
        shown = shown ? types.unionOf(shown, walker.typeShown(result)) : walker.typeShown(result);
        walker.jumpTo(meeting, singleElementSpan(result));
    }
    walker.continueIn(meeting);
    if (walker.hasFailed() || walker.isUnreachable())
        return { };
    if (shown && types.isSubset(shown, types.integer(Range::exact())))
        walker.noteTypeShownOfJoin(met, shown);
    return met;
}

static bool stepsLoopCounter(Walker& walker, const BodyValue& step, Reference result)
{
    static constexpr OpcodeSet steps { Opcode::Increment, Opcode::Decrement };
    if (!steps.contains(step.opcode()) || !walker.isInLoop() || !result.isValue() || walker.representationOf(result) != Representation::Double)
        return false;
    const BodyIndex& index = walker.currentBodyIndex();
    ValueID stepped = index.stripCasts(step.operand(0));
    if (!index.isParameter(stepped) && index.opcodeOf(stepped) == Opcode::ToNumeric)
        stepped = index.stripCasts(index.value(stepped).operand(0));
    if (!index.isParameter(stepped) || !index.isLoopHead(index.blockOf(stepped)))
        return false;
    Reference arriving = walker.referenceBoundTo(stepped);
    if (!arriving || !arriving.isValue() || walker.representationOf(arriving) != Representation::Int64)
        return false;
    Type type = walker.types().at(walker.typeOf(result));
    return type.isInteger() && type.integers.isSubsetOf(Range::exact());
}

static Reference integerOfSteppedCounter(Walker& walker, Reference stepped, Origin origin)
{
    origin.isTailCall = false;
    Draft conversion(Opcode::NumberToInteger, origin);
    conversion.setOperand(0, stepped);
    conversion.representation = Representation::Int64;
    return walker.build(WTF::move(conversion));
}

bool buildOperatorOnNumbers(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    OperatorForm form = operatorFormFor(walker, value.opcode(), value, inputs);
    if (isClosedFormOfOperatorOnNumbers(value.opcode(), form))
        return false;
    if (inputs.areOnlyTypes())
        return true;

    Draft draft = walker.draftOf(value);
    if (form == OperatorForm::Numbers && (hoistOperandCheck(walker, draft, value.id()) || tryGuardAddendOfIndex(walker, draft, value.id())))
        form = operatorFormFor(walker, draft);
    if (walker.hasFailed())
        return true;
    if (form == OperatorForm::Numbers) {
        prepareOperandsForFormNumbers(walker, draft);
        form = operatorFormFor(walker, draft);
    }
    if (form == OperatorForm::CheckedIntegers)
        form = operatorFormFor(walker, draft);
    if (form == OperatorForm::CheckedIntegers) {
        Reference stepped = buildStepOfExactInteger(walker, value, draft);
        if (walker.hasFailed() || walker.isUnreachable())
            return true;
        if (stepped) {
            walker.bind(value.id(), stepped);
            return true;
        }
    }
    walker.chooseConservativeFields(draft);
    draft.setForm(form);
    if (sumStaysBoxed(walker, value, draft))
        draft.representation = Representation::JSValue;
    Origin origin = draft.origin();
    Reference remainder;
    if (value.opcode() == Opcode::Mod && form == OperatorForm::Integers)
        remainder = buildRemainderByReciprocalMadeInFrontOfLoop(walker, draft);
    else if (value.opcode() == Opcode::Mod && form == OperatorForm::Numbers)
        remainder = buildRemainderByWholeDivisor(walker, draft);
    Reference result = remainder ? remainder : walker.build(WTF::move(draft));
    if (result && form == OperatorForm::CheckedIntegers && stepsLoopCounter(walker, value, result))
        result = integerOfSteppedCounter(walker, result, origin);
    walker.bind(value.id(), boxNumberIfNeeded(walker, result, origin));
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
