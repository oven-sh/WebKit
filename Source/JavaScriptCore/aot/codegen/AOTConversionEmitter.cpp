/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversionEmitter.h"

#if ENABLE(AOT)

#include "AOTConstantEmitter.h"
#include "AOTMachineFolding.h"
#include "AOTPureOperations.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTCompileUnit.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTThrowEmitter.h"
#include "AOTTypeArena.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <wtf/CheckedArithmetic.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t markerKinds[] = { Primitive::Undefined, Primitive::Null, Primitive::Absent };
static constexpr uint16_t markerPrimitives = Primitive::UndefinedOrNull | Primitive::Absent;

static_assert(integerMarkers.undefined < integerMarkers.null && integerMarkers.undefined < integerMarkers.absent);
static_assert(doubleMarkers.undefined < doubleMarkers.null && doubleMarkers.undefined < doubleMarkers.absent);
static_assert(static_cast<int64_t>(integerMarkers.absent) > 0 && static_cast<int64_t>(doubleMarkers.absent) > 0);
static_assert(PNaNAsBits < doubleMarkers.undefined);
static_assert(boxedMarkers.absent < boxedMarkers.undefined && boxedMarkers.null < boxedMarkers.undefined);

static constexpr std::optional<SlotMarkers> markersOf(Representation representation)
{
    switch (representation) {
    case Representation::OptionalInt64:
        return integerMarkers;
    case Representation::OptionalDouble:
        return doubleMarkers;
    case Representation::Cell32:
        return cell32Markers;
    case Representation::JSValue:
        return boxedMarkers;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::RawWord:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

static bool isValueWithoutBits(const Type& type)
{
    return type.isJavaScriptValue() && isLegalRepresentation(type, Representation::Void);
}

static constexpr bool isWithinInt32(Range range)
{
    return !range.isEmpty() && range.isSubsetOf(Range::int32());
}

static constexpr bool isNonNegative(Range range)
{
    return !range.isEmpty() && range.minimum >= 0;
}

static std::optional<Word32> convertedInt32Of(Number number)
{
    LValue value = number.value();
    if (value->opcode() != Machine::IToD || value->child(0)->type() != Machine::Int32)
        return std::nullopt;
    return Word32(value->child(0));
}

static LValue conversionInRegisters(Output& out, LType result, Number operand, const Machine::PureFunction& function, const auto& generator)
{
    LValue number = operand.value();
    if (LValue converted = out.findPurePatch(function, result, singleElementSpan(number)))
        return converted;
    Machine::PatchOperation* patch = out.patchpoint(result);
    patch->appendSomeRegister(number);
    patch->effects = Machine::Effects::none();
    patch->pureFunction = function;
    patch->setRunsWithoutFrame(true);
    patch->setClearsUpperHalf(result == Machine::Int32);
    patch->setGenerator(generator);
    out.recordPurePatch(patch);
    return patch;
}

Operand ConversionEmitter::convertMarkers(Operand source, const Type& type, Representation to, std::optional<Operand> result)
{
    bool sourceHasMarkers = markersOf(source.representation).has_value();
    for (uint16_t primitive : markerKinds) {
        if (!(type.primitives & primitive))
            continue;
        Operand marker = fromBits(integer(static_cast<int64_t>(markerBits(primitive, to))), to);
        if (!result) {
            result = marker;
            continue;
        }
        if (!sourceHasMarkers)
            continue;
        Condition holdsMarker = isMarker(source, markerBits(primitive, source.representation));
        result = Operand(to, m_out.select(holdsMarker.value(), marker.value, result->value));
    }
    return result ? *result : fromBits(integer(0), to);
}

Integer ConversionEmitter::integerPartOf(Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Int32:
        return widen(operand.as<Word32>(), type.integers);
    case Representation::Int64:
    case Representation::OptionalInt64:
        return Integer(operand.value);
    case Representation::Double:
    case Representation::OptionalDouble:
        return toInteger(Number(operand.value));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number ConversionEmitter::numberPartOf(Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Int32:
        return toNumber(operand.as<Word32>());
    case Representation::Int64:
        return toNumber(Integer(operand.value), type.integers);
    case Representation::OptionalInt64:
        return isWithinInt32(type.integers) ? toNumber(lowHalfOfOptionalInteger(operand.as<OptionalInteger>())) : toNumber(Integer(operand.value));
    case Representation::Double:
        return operand.as<Number>();
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static constexpr OpcodeSet boxings { Opcode::Box };

Boxed ConversionEmitter::box(const OperationContext& context, Operand operand, const Type& type)
{
    if (operand.representation == Representation::JSValue)
        return operand.as<Boxed>();
    if (operand.representation == Representation::Cell)
        return boxCell(operand.as<CellPointer>());
    BlockFrequency frequency = context.block() ? generator().code().at(context.block()).frequency : BlockFrequency::Normal;
    bool isOutsideLoops = context.block() && !generator().runsRepeatedly(context.block());
    SetForScope boxesByStub(m_boxesNumbersByStub, frequency != BlockFrequency::Normal || isOutsideLoops);
    SetForScope stateThatBoxesByStub(m_stateThatBoxesByStub, frequency != BlockFrequency::Normal ? StateOfKnowledge::Always : StateOfKnowledge::Number);
    SetForScope placeThatBoxesByStub(m_placeThatBoxesByStub, frequency == BlockFrequency::Rare ? Place::RareBlock : frequency == BlockFrequency::Unlikely ? Place::UnlikelyBlock : Place::Anywhere);
    switch (operand.representation) {
    case Representation::Void:
        if (!isValueWithoutBits(type))
            break;
        return convertMarkers(operand, type, Representation::JSValue).as<Boxed>();
    case Representation::Boolean:
        return boxBoolean(operand.as<Condition>());
    case Representation::Int32:
        return boxInt32(operand.as<Word32>());
    case Representation::Int64:
        return boxInteger(operand.as<Integer>(), type.integers);
    case Representation::Double:
        return boxNumber(operand);
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return boxOptional(operand, type);
    case Representation::Cell:
        return boxCell(operand.as<CellPointer>());
    case Representation::Cell32:
        return decode(operand.as<NarrowReference>(), type, Representation::JSValue).as<Boxed>();
    case Representation::JSValue:
        return operand.as<Boxed>();
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return undefined();
}

Boxed ConversionEmitter::boxByStub(BoxedNumberForm form, LValue number)
{
    CallOptions options { .pureFunction = pureFunction(PureOperation::BoxNumber, static_cast<uint32_t>(form), true) };
    return Boxed(emitter<RuntimeCallEmitter>().callStubWithoutSite(AOT::boxNumber(form), singleElementSpan(number), options).first);
}

Boxed ConversionEmitter::boxOptional(Operand operand, const Type& type)
{
    auto boxMarker = [&] {
        return convertMarkers(operand, type, Representation::JSValue).as<Boxed>();
    };
    auto boxUnmarked = [&] {
        return operand.is<OptionalInteger>() ? boxInteger(Integer(operand.value), type.integers) : boxNumber(Number(operand.value));
    };
    if (!(type.primitives & Primitive::Number))
        return boxMarker();
    if (!(type.primitives & markerPrimitives))
        return boxUnmarked();

    Condition holdsMarker = isMarker(operand);
    LBasicBlock numberCase = newBlock();
    LBasicBlock markerCase = newBlock();
    Join<Boxed> join(m_out);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holdsMarker.value(), unlikelyEdge(markerCase), normalEdge(numberCase));
    m_out.appendToArm(numberCase, fork);
    join.add(boxUnmarked());
    m_out.appendToArm(markerCase, fork);
    join.add(boxMarker());
    return join.finishBehind(fork);
}

void ConversionEmitter::stopUnlessBoxHolds(Boxed value, TypeID kinds)
{
    const OperationContext* context = generator().contextBeingGenerated();
    if (!context || !sourcesOfProofsCheckedUnder(build().options().testing))
        return;
    emitter<ThrowEmitter>().requireOrCrash(*context, hasType(value, typeTestPlanFor(TypeTable::anythingOrAbsent, Representation::JSValue, kinds)), value, ProofSource::OtherOperation);
}

void ConversionEmitter::stopUnlessToldRangeHolds(Condition holds)
{
    if (const OperationContext* context = generator().contextBeingGenerated())
        emitter<ThrowEmitter>().requireOrCrash(*context, holds, std::nullopt, ProofSource::OtherOperation);
}

Word32 ConversionEmitter::lowHalfOfIntegerToldInsideInt32(Integer wide)
{
    Word32 low = low32(wide);
    if (emittersCheckProofs())
        stopUnlessToldRangeHolds(equal(signExtended(low), wide));
    return low;
}

Word32 ConversionEmitter::lowHalfOfOptionalInteger(OptionalInteger optional)
{
    Integer wide(optional.value());
    Word32 low = low32(wide);
    if (emittersCheckProofs())
        stopUnlessToldRangeHolds(logicalOr(isMarker(Operand(Representation::OptionalInt64, optional.value())), equal(signExtended(low), wide)));
    return low;
}

Integer ConversionEmitter::integerKnownNotMarker(OptionalInteger optional)
{
    if (emittersCheckProofs())
        stopUnlessToldRangeHolds(logicalNot(isMarker(Operand(Representation::OptionalInt64, optional.value()))));
    return withoutMarkers(Operand(Representation::OptionalInt64, optional.value())).as<Integer>();
}

Number ConversionEmitter::unboxDoubleThatIsAsked(Boxed value)
{
    stopUnlessBoxHolds(value, TypeTable::number);
    return unboxDouble(value);
}

Operand ConversionEmitter::unbox(Boxed value, const Type& type, Representation to)
{
    switch (to) {
    case Representation::Void:
        return { };
    case Representation::Boolean:
        return unboxBoolean(value);
    case Representation::Int32:
        return either<Word32>(isInt32(value), [&] {
            return unboxInt32(value);
        }, [&] {
            return lowHalfOfIntegerToldInsideInt32(truncateUnchecked(unboxDoubleThatIsAsked(value)));
        });
    case Representation::Int64:
        return either<Integer>(isInt32(value), [&] {
            return signExtended(unboxInt32(value));
        }, [&] {
            return truncateUnchecked(unboxDoubleThatIsAsked(value));
        });
    case Representation::Double:
        if (!(type.primitives & Primitive::Integer))
            return unboxDoubleThatIsAsked(value);
        return selectByBranch<Number>(isNotInt32(value), [&] {
            return unboxDoubleThatIsAsked(value);
        }, [&] {
            return toNumber(unboxInt32(value));
        });
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return unboxOptional(value, type, to);
    case Representation::Cell:
        return unboxCell(value);
    case Representation::Cell32:
        return compress(value, type);
    case Representation::JSValue:
        return value;
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::unboxOptional(Boxed value, const Type& type, Representation to)
{
    auto unboxMarker = [&] {
        stopUnlessBoxHolds(value, TypeArena::fromPrimitives(type.primitives & markerPrimitives));
        return convertMarkers(value, type, to);
    };
    auto unboxNumber = [&] {
        return Operand(to, unbox(value, type, to == Representation::OptionalInt64 ? Representation::Int64 : Representation::Double).value);
    };
    if (!(type.primitives & Primitive::Number))
        return unboxMarker();
    if (!(type.primitives & markerPrimitives))
        return unboxNumber();

    if (type.primitives & Primitive::Integer) {
        LBasicBlock int32Case = newBlock();
        LBasicBlock otherCase = newBlock();
        OperandJoin join(m_out, to);
        ScopeFork fork = m_out.forkScope();
        m_out.branch(isNotInt32(value).value(), type.primitives & Primitive::Double ? normalEdge(otherCase) : unlikelyEdge(otherCase), normalEdge(int32Case));
        m_out.appendToArm(int32Case, fork);
        join.add(Operand(to, to == Representation::OptionalInt64 ? signExtended(unboxInt32(value)).value() : toNumber(unboxInt32(value)).value()));
        m_out.appendToArm(otherCase, fork);
        Type rest = type;
        rest.primitives = (type.primitives & ~Primitive::Integer) | Primitive::Double;
        join.add(unboxOptional(value, rest, to));
        return join.finishBehind(fork);
    }

    Condition holdsNumber = isNumber(value);
    LBasicBlock numberCase = newBlock();
    LBasicBlock markerCase = newBlock();
    OperandJoin join(m_out, to);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(holdsNumber.value(), normalEdge(numberCase), unlikelyEdge(markerCase));
    m_out.appendToArm(numberCase, fork);
    join.add(unboxNumber());
    m_out.appendToArm(markerCase, fork);
    join.add(unboxMarker());
    return join.finishBehind(fork);
}

Operand ConversionEmitter::toOptional(Operand operand, const Type& type, Representation to)
{
    RELEASE_ASSERT(to == Representation::OptionalInt64 || to == Representation::OptionalDouble);
    if (operand.representation == to)
        return operand;
    if (operand.is<Boxed>())
        return unbox(operand.as<Boxed>(), type, to);
    RELEASE_ASSERT(operand.representation != Representation::Void || isValueWithoutBits(type));

    std::optional<Operand> number;
    if (type.primitives & Primitive::Number)
        number = Operand(to, to == Representation::OptionalInt64 ? integerPartOf(operand, type).value() : numberPartOf(operand, type).value());
    Operand optional = convertMarkers(operand, type, to, number);
    if (operand.is<OptionalInteger>() && type.hasOnlyPrimitives(Primitive::Integer | Primitive::Undefined) && optional.value->opcode() == Machine::Select)
        m_optionalIntegersOfNumbers.add(optional.value, OptionalIntegerOfNumber { operand.value, type.integers });
    else if (operand.is<OptionalInteger>() && type.hasOnlyPrimitives(Primitive::Integer | Primitive::Null) && optional.value->opcode() == Machine::Select)
        m_integersOrNullOfNumbers.add(optional.value, OptionalIntegerOfNumber { operand.value, type.integers });
    return optional;
}

Operand ConversionEmitter::inRepresentation(const OperationContext& context, Operand produced, const Type& producedType, Representation wanted)
{
    RELEASE_ASSERT(wanted != Representation::Unchosen);
    if (produced.representation == wanted)
        return produced;
    if (produced.is<Boxed>() && wanted != Representation::RawWord)
        return unbox(produced.as<Boxed>(), producedType, wanted);
    if (numberRepresentations.contains(wanted) && producedType.isNumber()) {
        produced = withoutMarkers(produced);
        if (produced.representation == wanted)
            return produced;
    }

    bool isNumber = numberRepresentations.contains(produced.representation);
    auto producedInteger = [&] {
        if (produced.is<Word32>())
            return signExtended(produced.as<Word32>());
        return produced.is<Integer>() ? produced.as<Integer>() : truncateUnchecked(produced.as<Number>());
    };

    switch (wanted) {
    case Representation::Void:
        return { };
    case Representation::JSValue:
        return box(context, produced, producedType);
    case Representation::Int32:
        if (isNumber)
            return producedType.isInt32() ? lowHalfOfIntegerToldInsideInt32(producedInteger()) : narrow(producedInteger());
        break;
    case Representation::Int64:
        if (isNumber)
            return producedInteger();
        break;
    case Representation::Double:
        if (produced.is<Word32>())
            return toNumber(produced.as<Word32>());
        if (produced.is<Integer>())
            return toNumber(produced.as<Integer>(), producedType.integers);
        break;
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        if (isNumber || optionalRepresentations.contains(produced.representation))
            return toOptional(produced, producedType, wanted);
        break;
    case Representation::Cell32:
        if (produced.is<CellPointer>())
            return compress(produced, producedType);
        break;
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return emitter<ConstantEmitter>().zeroOf(wanted);
}

Boxed ConversionEmitter::boxNumber(Operand number)
{
    Number value(number.value);
    if (value.value()->hasDouble())
        return boxedConstant(jsNumber(AOT::withoutPayload(value.value()->asDouble())));
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return boxInt32(*converted);
    if (value.value()->opcode() == Machine::IToD && value.value()->child(0)->type() == Machine::Int64)
        return boxInteger(Integer(value.value()->child(0)), Range::exact());
    if (m_boxesNumbersByStub) {
        m_out.noteFormOf(boxings, Form::Closed, m_stateThatBoxesByStub, m_placeThatBoxesByStub);
        return boxNumberByStub(value);
    }

#if CPU(ARM64)
    if (convertsWithJavaScriptSemantics()) {
        Integer exact = exactInt32OrAllOnes(value);
        return selectByBranch<Boxed>(greaterThanOrEqual(exact, integer(0)), [&] {
            return boxInt32(low32(exact));
        }, [&] {
            return boxDouble(value);
        });
    }
#endif

    Word32 low = truncateToWord(value);
    Integer bitsOfLow = bitsOf(toNumber(low));
    return selectByBranch<Boxed>(notEqual(bitsOfLow, bitsOf(value)), [&] {
        return boxDouble(value);
    }, [&] {
        return boxInt32(low);
    });
}

Word32 ConversionEmitter::int32OrElse(Number value, LBasicBlock isNoInt32)
{
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return *converted;
#if CPU(ARM64)
    if (convertsWithJavaScriptSemantics()) {
        Integer exact = exactInt32OrAllOnes(value);
        branchUnless(greaterThanOrEqual(exact, integer(0)), isNoInt32);
        return low32(exact);
    }
#endif
    Word32 low = truncateToWord(value);
    branchUnless(equal(bitsOf(toNumber(low)), bitsOf(value)), isNoInt32);
    return low;
}

Word32 ConversionEmitter::int32OrElse(Integer value, Range range, LBasicBlock isNoInt32)
{
    if (isWithinInt32(range))
        return lowHalfOfIntegerToldInsideInt32(value);
    Word32 low = low32(value);
    branchUnless(isNonNegative(range) ? belowOrEqual(value, integer(INT32_MAX)) : equal(signExtended(low), value), isNoInt32);
    return low;
}

static bool isSumThatMayOverflowInt32(Integer value)
{
    LValue sum = value.value();
    auto isWidened = [](LValue operand) {
        return operand->opcode() == Machine::SExt32 || operand->hasInt64();
    };
    return (sum->opcode() == Machine::Add || sum->opcode() == Machine::Sub) && isWidened(sum->child(0)) && isWidened(sum->child(1));
}

Boxed ConversionEmitter::boxInteger(Integer value, Range range)
{
    if (isWithinInt32(range))
        return boxInt32(lowHalfOfIntegerToldInsideInt32(value));
    Word32 low = low32(value);
    if (m_boxesNumbersByStub && (m_stateThatBoxesByStub == StateOfKnowledge::Always || !isSumThatMayOverflowInt32(value))) {
        m_out.noteFormOf(boxings, Form::Closed, m_stateThatBoxesByStub, m_placeThatBoxesByStub);
        return boxByStub(BoxedNumberForm::Integer, value.value());
    }

    Condition fitsInt32 = isNonNegative(range) ? belowOrEqual(value, integer(INT32_MAX)) : equal(signExtended(low), value);
    LBasicBlock int32Case = newBlock();
    LBasicBlock doubleCase = newBlock();
    Join<Boxed> join(m_out);
    ScopeFork fork = m_out.forkScope();
    m_out.branch(fitsInt32.value(), normalEdge(int32Case), unlikelyEdge(doubleCase));
    m_out.appendToSuccessor(int32Case, fork);
    join.add(boxInt32(low32(value)));
    m_out.appendToArm(doubleCase, fork);
    join.add(boxDouble(toNumber(value)));
    return join.finishBehind(fork);
}

Number ConversionEmitter::toNumber(Integer value, Range range)
{
    if (isWithinInt32(range))
        return toNumber(lowHalfOfIntegerToldInsideInt32(value));
    return toNumber(value);
}

Integer ConversionEmitter::toInteger(Number value, Range range, bool standsInLoop)
{
    LValue number = value.value();
    static_assert(maxTrackedInteger < 1ll << 62, "a tracked integer is within int64");
    if (std::optional<Integer> int32 = range.isSubsetOf(Range::int32()) ? int32Behind(value) : std::nullopt)
        return *int32;
    if (number->opcode() != Machine::IToD || number->child(0)->type() != Machine::Int64 || range.isEmpty() || !range.isSubsetOf(Range::exact()))
        return truncateUnchecked(value);
    Integer converted(number->child(0));
    if (!standsInLoop && (range.maximum == maxExactInteger || range.minimum == -maxExactInteger)) {
        static_assert(maxExactInteger == 1ll << 53);
        return range.minimum >= 1 ? sub(converted, Integer(m_out.lShr(sub(converted, 1).value(), m_out.constInt32(53)))) : truncateUnchecked(value);
    }
    if (range.maximum == maxExactInteger)
        converted = select(greaterThan(converted, maxExactInteger), maxExactInteger, converted);
    if (range.minimum == -maxExactInteger)
        converted = select(lessThan(converted, -maxExactInteger), -maxExactInteger, converted);
    return converted;
}

std::optional<Integer> ConversionEmitter::findTruncation(Number value)
{
    if (value.value()->hasDouble() && std::abs(value.value()->asDouble()) < static_cast<double>(maxTrackedInteger))
        return integer(static_cast<int64_t>(value.value()->asDouble()));
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return signExtended(*converted);
    return std::nullopt;
}

Integer ConversionEmitter::toInteger(Number value)
{
    if (std::optional<Integer> found = findTruncation(value))
        return *found;
    return Integer(conversionInRegisters(m_out, Machine::Int64, value, truncationToInt64(0), [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
#if CPU(ARM64)
        jit.m_assembler.cmn<64>(params[0].gpr(), UInt12(1));
        jit.m_assembler.cinc<64>(params[0].gpr(), params[0].gpr(), ARM64Assembler::ConditionVS);
#endif
    }));
}

#if CPU(ARM64)
bool ConversionEmitter::convertsWithJavaScriptSemantics()
{
    return MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures());
}

Integer ConversionEmitter::exactInt32OrAllOnes(Number value)
{
    return Integer(conversionInRegisters(m_out, Machine::Int64, value, pureFunction(PureOperation::ExactInt32, 0, true), [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        jit.m_assembler.csinv<64>(params[0].gpr(), params[0].gpr(), ARM64Registers::zr, ARM64Assembler::ConditionEQ);
    }));
}
#endif

Word32 ConversionEmitter::truncateToWord(Number value)
{
    if (std::optional<Integer> found = findTruncation(value))
        return low32(*found);
    LValue number = value.value();
    if (LValue whole = m_out.findPurePatch(truncationToInt64(isARM64() ? 1 : 0), Machine::Int64, singleElementSpan(number)))
        return low32(Integer(whole));
    return Word32(conversionInRegisters(m_out, Machine::Int32, value, pureFunction(PureOperation::TruncateToInt32, 0, true), [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.truncateDoubleToInt32(params[1].fpr(), params[0].gpr());
    }));
}

Integer ConversionEmitter::truncateUnchecked(Number value)
{
#if CPU(ARM64)
    if (std::optional<Integer> found = findTruncation(value))
        return *found;
    return Integer(conversionInRegisters(m_out, Machine::Int64, value, truncationToInt64(1), [](CCallHelpers& jit, const Machine::PatchParameters& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    }));
#else
    return toInteger(value);
#endif
}

static constexpr Range integersNeverRounded { -(1ll << 51), 1ll << 51 };
static constexpr Range productsBehindNumbers { -(1ll << 62), 1ll << 62 };

static std::optional<Range> rangeOfProductBehindNumber(Range a, Range b)
{
    Range products = Range::empty();
    for (int64_t left : { a.minimum, a.maximum }) {
        for (int64_t right : { b.minimum, b.maximum }) {
            CheckedInt64 product = CheckedInt64(left) * right;
            if (product.hasOverflowed())
                return std::nullopt;
            products = products.unionWith(Range::one(product.value()));
        }
    }
    return products;
}

static int64_t largestMagnitudeBehindNumber(Range range)
{
    return std::max(std::abs(range.minimum), std::abs(range.maximum));
}

static bool isArithmeticThatMakesNumber(LValue number)
{
    switch (number->opcode()) {
    case Machine::Neg:
    case Machine::Add:
    case Machine::Sub:
    case Machine::Mul:
        return true;
    default:
        return false;
    }
}

template<typename Answer>
static void answerOperandsFirst(LValue number, UncheckedKeyHashMap<LValue, Answer>& answers, NOESCAPE const Invocable<Answer(LValue)> auto& answerFor)
{
    Vector<LValue, 16> pending { number };
    while (!pending.isEmpty()) {
        LValue value = pending.last();
        if (answers.contains(value)) {
            pending.removeLast();
            continue;
        }
        size_t sizeBefore = pending.size();
        if (isArithmeticThatMakesNumber(value)) {
            for (unsigned index = value->numChildren(); index--;) {
                if (!answers.contains(value->child(index)))
                    pending.append(value->child(index));
            }
        }
        if (pending.size() != sizeBefore)
            continue;
        pending.removeLast();
        answers.add(value, answerFor(value));
    }
}

static bool isNumberReadFromStorage(LValue number)
{
    return number->type() == Machine::Double && (number->opcode() == Machine::Load || number->opcode() == Machine::Phi || number->opcode() == Machine::ArgumentReg);
}

ValueID ConversionEmitter::parameterOfLoopHeadFedBy(ValueID start)
{
    if (m_parametersOfLoopHeadsFed.isEmpty()) {
        m_parametersOfLoopHeadsFed.fill(ValueID(), m_code.numberOfValues());
        m_valuesFollowed.ensureSize(m_code.numberOfValues());
    }

    Vector<ValueID, 16> followed;
    ValueID fed;
    for (ValueID value = start; value;) {
        if (opcodeOf(value) == Opcode::Parameter && m_code.at(m_code.blockOf(value)).isLoopHead) {
            fed = value;
            break;
        }
        if (m_valuesFollowed.quickSet(value.index())) {
            fed = m_parametersOfLoopHeadsFed[value.index()];
            break;
        }
        followed.append(value);
        ValueID parameter = m_code.parameterFedBy(value);
        value = parameter ? parameter : m_code.onlyUserOf(value);
    }
    for (ValueID value : followed)
        m_parametersOfLoopHeadsFed[value.index()] = fed;
    return fed;
}

bool ConversionEmitter::comesRoundToIntegerBehind(LValue number)
{
    if (!m_code.hasLoops())
        return false;
    ValueID carried = parameterOfLoopHeadFedBy(generator().operationBeingGenerated());
    if (!carried || !integerRepresentations.contains(representationOf(carried)))
        return false;

    LValue joined = generator().wordOf(carried);
    if (!joined)
        return false;
    if (std::exchange(m_integerThatComesRound, joined) != joined)
        m_holdsIntegerThatComesRound.clear();
    UncheckedKeyHashMap<LValue, bool>& holdsJoinedInteger = m_holdsIntegerThatComesRound;
    answerOperandsFirst<bool>(number, holdsJoinedInteger, [&](LValue value) {
        if (value->opcode() == Machine::IToD)
            return value->child(0) == joined;
        if (!isArithmeticThatMakesNumber(value))
            return false;
        return std::ranges::any_of(value->children(), [&](LValue operand) {
            return holdsJoinedInteger.get(operand);
        });
    });
    return holdsJoinedInteger.get(number);
}

std::optional<ConversionEmitter::IntegerBehindNumber> ConversionEmitter::describeIntegerBehind(LValue number, bool mayTestWhatWasRead, const IntegersBehindNumbers& operands)
{
    auto converted = [&](LValue source, bool mayBeTested) -> std::optional<IntegerBehindNumber> {
        Machine::IntegerRange known = Machine::rangeOf(source);
        Range range { known.min, known.max };
        if (range.isSubsetOf(integersNeverRounded))
            return IntegerBehindNumber { .range = range };
        if (!mayBeTested || source->type() != Machine::Int64)
            return std::nullopt;
        return IntegerBehindNumber { .range = Range::int32(), .isTested = true, .asksWhetherWideIntegerIsInt32 = true };
    };

    if (auto optional = m_optionalIntegersOfNumbers.getOptional(number))
        return optional->range.isSubsetOf(integersNeverRounded) ? std::optional(IntegerBehindNumber { .range = optional->range, .isTested = true }) : std::nullopt;

    switch (number->opcode()) {
    case Machine::ConstDouble: {
        double constant = number->asDouble();
        if (std::trunc(constant) != constant || std::abs(constant) > static_cast<double>(integersNeverRounded.maximum) || (!constant && std::signbit(constant)))
            return std::nullopt;
        return IntegerBehindNumber { .range = Range::one(static_cast<int64_t>(constant)) };
    }
    case Machine::IToD: {
        std::optional<IntegerBehindNumber> known = converted(number->child(0), true);
        if (known)
            known->holdsElementOfList = m_numbersOfInt32Elements.contains(number);
        return known;
    }
    case Machine::Phi: {
        LValue noted = m_integersNotedBehindNumbers.get(number);
        return noted ? converted(noted, false) : std::nullopt;
    }
    case Machine::Select:
        if (!m_int32ElementsBehindNumbersOrNaN.contains(number))
            return std::nullopt;
        return IntegerBehindNumber { .range = Range::int32(), .isTested = true, .holdsElementOfList = true };
    case Machine::Neg: {
        std::optional<IntegerBehindNumber> operand = operands.get(number->child(0));
        if (!operand || operand->mayBeRounded)
            return std::nullopt;
        return IntegerBehindNumber { .range = Range { -operand->range.maximum, -operand->range.minimum }, .isTested = operand->isTested, .holdsElementOfList = operand->holdsElementOfList, .asksWhetherWideIntegerIsInt32 = operand->asksWhetherWideIntegerIsInt32 };
    }
    case Machine::Mul: {
        std::optional<IntegerBehindNumber> left = operands.get(number->child(0));
        std::optional<IntegerBehindNumber> right = operands.get(number->child(1));
        if (!left || !right || left->mayBeRounded || right->mayBeRounded)
            return std::nullopt;
        std::optional<Range> range = rangeOfProductBehindNumber(left->range, right->range);
        if (!range || !range->isSubsetOf(productsBehindNumbers))
            return std::nullopt;
        bool mayBeRounded = !range->isSubsetOf(integersNeverRounded);
        if (mayBeRounded && left->holdsElementOfList != right->holdsElementOfList)
            return std::nullopt;
        return IntegerBehindNumber { .range = *range, .mayBeRounded = mayBeRounded, .isTested = left->isTested || right->isTested, .holdsElementOfList = left->holdsElementOfList || right->holdsElementOfList, .asksWhetherWideIntegerIsInt32 = left->asksWhetherWideIntegerIsInt32 || right->asksWhetherWideIntegerIsInt32 };
    }
    case Machine::Add:
    case Machine::Sub: {
        std::optional<IntegerBehindNumber> left = operands.get(number->child(0));
        std::optional<IntegerBehindNumber> right = operands.get(number->child(1));
        if (!left && !right)
            return std::nullopt;
        std::optional<IntegerBehindNumber>& missing = left ? right : left;
        if (!missing && mayTestWhatWasRead && !(left ? left : right)->holdsElementOfList && isNumberReadFromStorage(number->child(left ? 1 : 0)) && !m_doubleElements.contains(number->child(left ? 1 : 0)))
            missing = IntegerBehindNumber { .range = Range::int32(), .isTested = true };
        if (!left || !right || (left->mayBeRounded && right->mayBeRounded))
            return std::nullopt;
        if ((left->holdsElementOfList && right->asksWhetherWideIntegerIsInt32) || (right->holdsElementOfList && left->asksWhetherWideIntegerIsInt32))
            return std::nullopt;
        bool isSum = number->opcode() == Machine::Add;
        IntegerBehindNumber result {
            .range = isSum ? Range { left->range.minimum + right->range.minimum, left->range.maximum + right->range.maximum } : Range { left->range.minimum - right->range.maximum, left->range.maximum - right->range.minimum },
            .mayBeRounded = left->mayBeRounded || right->mayBeRounded,
            .isTested = left->isTested || right->isTested,
            .holdsElementOfList = left->holdsElementOfList || right->holdsElementOfList,
            .asksWhetherWideIntegerIsInt32 = left->asksWhetherWideIntegerIsInt32 || right->asksWhetherWideIntegerIsInt32,
        };
        if (!result.mayBeRounded)
            return result.range.isSubsetOf(integersNeverRounded) ? std::optional(result) : std::nullopt;
        const IntegerBehindNumber& rounded = left->mayBeRounded ? *left : *right;
        const IntegerBehindNumber& other = left->mayBeRounded ? *right : *left;
        if (other.holdsElementOfList && !rounded.holdsElementOfList)
            return std::nullopt;
        result.largestSumOfOtherTerms = rounded.largestSumOfOtherTerms + largestMagnitudeBehindNumber(other.range);
        if (result.largestSumOfOtherTerms > integersNeverRounded.maximum)
            return std::nullopt;
        return result;
    }
    default:
        return std::nullopt;
    }
}

ConversionEmitter::ExactInteger ConversionEmitter::computeIntegerBehind(LValue number, const IntegerBehindNumber& known, bool inWords, const ExactIntegers& operands)
{
    auto ofWord = [&](Word32 word) {
        return inWords ? word.value() : signExtended(word).value();
    };
    auto ofInteger = [&](Integer wide) {
        return inWords ? low32(wide).value() : wide.value();
    };
    auto wholeNumberReadFromStorage = [&](LValue read) {
        Word32 word = truncateToWord(Number(read));
        return ExactInteger { .value = ofWord(word), .holdsWhile = equal(toNumber(word), Number(read)) };
    };
    auto whileBoth = [&](Condition first, Condition second) {
        return first && second ? logicalAnd(first, second) : first ? first : second;
    };

    if (auto optional = m_optionalIntegersOfNumbers.getOptional(number)) {
        build().statistics().count(StatisticID::TruncatedOptionalIntegers);
        return ExactInteger { .value = ofInteger(Integer(optional->integer)), .holdsWhile = logicalNot(isMarker(Operand(Representation::OptionalInt64, optional->integer))) };
    }

    switch (number->opcode()) {
    case Machine::ConstDouble:
        return ExactInteger { ofInteger(integer(known.range.minimum)) };
    case Machine::IToD:
    case Machine::Phi: {
        LValue source = number->opcode() == Machine::IToD ? number->child(0) : m_integersNotedBehindNumbers.get(number);
        if (source->type() == Machine::Int32)
            return ExactInteger { ofWord(Word32(source)) };
        Integer wide(source);
        if (!known.isTested)
            return ExactInteger { inWords ? low32InWords(wide).value() : wide.value() };
        return ExactInteger { .value = ofInteger(wide), .holdsWhile = equal(signExtended(low32(wide)), wide) };
    }
    case Machine::Select: {
        Boxed element(m_int32ElementsBehindNumbersOrNaN.get(number));
        build().statistics().count(StatisticID::TruncatedElementsReadAsInt32);
        return ExactInteger { .value = ofWord(unboxInt32(element)), .holdsWhile = isNotEmpty(element) };
    }
    case Machine::Neg: {
        ExactInteger operand = *operands.get(number->child(0));
        return ExactInteger { .value = m_out.neg(operand.value), .holdsWhile = operand.holdsWhile };
    }
    case Machine::Mul: {
        ExactInteger left = *operands.get(number->child(0));
        ExactInteger right = *operands.get(number->child(1));
        return ExactInteger { .value = m_out.mul(left.value, right.value), .mayBeRounded = known.mayBeRounded, .holdsWhile = whileBoth(left.holdsWhile, right.holdsWhile) };
    }
    case Machine::Add:
    case Machine::Sub: {
        std::optional<ExactInteger> left = operands.get(number->child(0));
        std::optional<ExactInteger> right = operands.get(number->child(1));
        if (!left)
            left = wholeNumberReadFromStorage(number->child(0));
        else if (!right)
            right = wholeNumberReadFromStorage(number->child(1));
        return ExactInteger {
            .value = number->opcode() == Machine::Add ? m_out.add(left->value, right->value) : m_out.sub(left->value, right->value),
            .mayBeRounded = known.mayBeRounded,
            .holdsWhile = whileBoth(left->holdsWhile, right->holdsWhile),
        };
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<ConversionEmitter::ExactInteger> ConversionEmitter::findExactIntegerBehind(LValue number, ExactIntegerUse use)
{
    bool mayBeTested = use == ExactIntegerUse::Truncated;
    auto describe = [&](bool mayTestWhatWasRead) {
        IntegersBehindNumbers& known = m_integersBehindNumbers[mayTestWhatWasRead];
        answerOperandsFirst<std::optional<IntegerBehindNumber>>(number, known, [&](LValue value) {
            return describeIntegerBehind(value, mayTestWhatWasRead, known);
        });
        return known.get(number);
    };

    bool testsWhatWasRead = false;
    std::optional<IntegerBehindNumber> knownOfNumber = describe(false);
    if (!knownOfNumber && mayBeTested && isArithmeticThatMakesNumber(number) && comesRoundToIntegerBehind(number)) {
        testsWhatWasRead = true;
        knownOfNumber = describe(true);
    }
    if (!knownOfNumber || (knownOfNumber->isTested && !mayBeTested))
        return std::nullopt;

    bool inWords = use == ExactIntegerUse::Truncated && !knownOfNumber->mayBeRounded;
    const IntegersBehindNumbers& known = m_integersBehindNumbers[testsWhatWasRead];
    ExactIntegers computed;
    answerOperandsFirst<std::optional<ExactInteger>>(number, computed, [&](LValue value) -> std::optional<ExactInteger> {
        std::optional<IntegerBehindNumber> knownOfValue = known.get(value);
        if (!knownOfValue)
            return std::nullopt;
        return computeIntegerBehind(value, *knownOfValue, inWords, computed);
    });
    return computed.get(number);
}

void ConversionEmitter::noteIndexOfKey(Number key)
{
    if (indexBehind(key) || findExactIntegerBehind(key.value(), ExactIntegerUse::Whole))
        return;
    Integer truncated = truncateUnchecked(key);
    Condition isExact = isX86_64() ? equalOrUnordered(toNumber(truncated), key) : equal(toNumber(truncated), key);
    noteIndexOfNumber(key, select(isExact, truncated, integer(-1)));
}

std::optional<Integer> ConversionEmitter::exactIntegerBehind(Number number)
{
    std::optional<ExactInteger> exact = findExactIntegerBehind(number.value(), ExactIntegerUse::Whole);
    if (!exact)
        return std::nullopt;
    build().statistics().count(StatisticID::DoublesRecomputedAsIntegers);
    return Integer(exact->value);
}

Integer ConversionEmitter::widen(Word32 value, Range range)
{
    if (std::optional<Integer> extended = signExtendedIntegerOf(value))
        return *extended;
    if (!isNonNegative(range))
        return signExtended(value);
    if (emittersCheckProofs())
        stopUnlessToldRangeHolds(greaterThanOrEqual(value, word32(0)));
    return Integer(m_out.extendWordThatIsNotNegative(value.value()));
}

Word32 ConversionEmitter::toInt32(Number value, BitsOfInt32 bits)
{
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return *converted;
    if (LValue truncation = m_truncationsOfNumbers.get(value.value()))
        return Word32(truncation);
    static_assert(!static_cast<uint32_t>(JSValue::ValueEmpty));
    if (LValue element = m_int32ElementsBehindNumbersOrNaN.get(value.value()))
        return unboxInt32Element(Boxed(element));
    if (value.value()->opcode() == Machine::Neg)
        return neg(toInt32(Number(value.value()->child(0)), bits));
    if (bits == BitsOfInt32::All && m_lastTruncation.number == value.value() && m_lastTruncation.block == m_out.m_block)
        return Word32(m_lastTruncation.word);
    std::optional<ExactInteger> exact = findExactIntegerBehind(value.value(), ExactIntegerUse::Truncated);
    if (!exact) {
        Word32 word = toInt32OfAnyNumber(value, bits);
        if (bits == BitsOfInt32::All)
            m_lastTruncation = { value.value(), word.value(), m_out.m_block };
        return word;
    }

    build().statistics().count(StatisticID::DoublesRecomputedAsIntegers);
    Word32 word = exact->mayBeRounded ? low32(Integer(exact->value)) : Word32(exact->value);
    Condition isExact = exact->holdsWhile;
    if (exact->mayBeRounded) {
        Condition isInt32 = equal(signExtended(word), Integer(exact->value));
        isExact = isExact ? logicalAnd(isExact, isInt32) : isInt32;
    }
    if (!isExact)
        return word;
    return either<Word32>(isExact, [&] {
        return word;
    }, [&] {
        return exact->holdsWhile ? toInt32OfAnyNumber(value, bits) : low32(toInteger(value));
    });
}

Word32 ConversionEmitter::toInt32OfBoxedNumber(Boxed value)
{
    return either<Word32>(isInt32(value), [&] {
        return unboxInt32(value);
    }, [&] {
        return toInt32(unboxDouble(value));
    });
}

Word32 ConversionEmitter::truncateNullableNumber(Boxed value)
{
    return either<Word32>(isInt32(value), [&] {
        return unboxInt32(value);
    }, [&] {
        return either<Word32>(isNumber(value), [&] {
            return toInt32(unboxDouble(value));
        }, [&] {
            return word32(0);
        });
    });
}

Word32 ConversionEmitter::toInt32OfAnyNumber(Number value, BitsOfInt32 bits)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures())) {
        return Word32(conversionInRegisters(m_out, Machine::Int32, value, pureFunction(PureOperation::ToInt32, 0, true), [](CCallHelpers& jit, const Machine::PatchParameters& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        }));
    }
#endif
    if (bits == BitsOfInt32::Lowest)
        return low32(toInteger(value));
    Integer truncated = truncateUnchecked(value);
    Condition fits;
    if (isX86_64())
        fits = notEqual(truncated, std::numeric_limits<int64_t>::min());
    else {
        fits = greaterThanOrEqual(bitXor(truncated, Integer(m_out.shl(truncated.value(), 1))), 0);
    }
    return either<Word32>(fits, [&] {
        return low32(truncated);
    }, [&] {
        return low32(callLeaf<Integer>(RuntimeOperation::ToInt32, { value }));
    });
}

Number ConversionEmitter::purified(Number value)
{
    return either<Number>(equal(value, value), [&] {
        return value;
    }, [&] {
        return number(PNaN);
    });
}

std::optional<Number> ConversionEmitter::convertOptionalInteger(LValue optional)
{
    auto behind = m_optionalIntegersOfNumbers.getOptional(optional);
    if (!behind)
        return std::nullopt;
    Integer whole(behind->integer);
    Number made = select(isMarker(Operand(Representation::OptionalInt64, behind->integer)), number(PNaN), toNumber(whole));
    if (made.value()->opcode() == Machine::Select)
        m_optionalIntegersOfNumbers.add(made.value(), OptionalIntegerOfNumber { behind->integer, behind->range, true });
    return made;
}

std::optional<Number> ConversionEmitter::convertIntegerOrNull(LValue optional)
{
    auto behind = m_integersOrNullOfNumbers.getOptional(optional);
    if (!behind)
        return std::nullopt;
    Integer whole = select(isMarker(Operand(Representation::OptionalInt64, behind->integer)), integer(0), Integer(behind->integer));
    return toNumber(whole, Range { std::min<int64_t>(behind->range.minimum, 0), std::max<int64_t>(behind->range.maximum, 0) });
}

Number ConversionEmitter::withoutPayload(Number value)
{
    if (value.value()->opcode() == Machine::IToD || m_doubleElements.contains(value.value()))
        return value;
    return either<Number>(equal(value, value), [&] {
        return value;
    }, [&] {
        return Number(m_out.bitCast(quietNaNWithSignOf(bitsOf(value)).value(), Machine::Double));
    });
}

Operand ConversionEmitter::decode(NarrowReference word, const Type& type, Representation to)
{
    switch (to) {
    case Representation::Cell:
        return decode(word);
    case Representation::Cell32:
        return word;
    case Representation::JSValue: {
        if (!(type.primitives & markerPrimitives))
            return boxCell(decode(word));
        Boxed marker(m_out.zeroExt(word.value(), Machine::Int64));
        if (!type.hasCells())
            return marker;
        Condition isCell = isCellReference(word);
        return select(isCell, boxCell(decode(word)), marker);
    }
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NarrowReference ConversionEmitter::compress(Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Void:
        RELEASE_ASSERT(isValueWithoutBits(type));
        return convertMarkers(operand, type, Representation::Cell32).as<NarrowReference>();
    case Representation::Cell:
        return compress(operand.as<CellPointer>());
    case Representation::Cell32:
        return operand.as<NarrowReference>();
    case Representation::JSValue: {
        Boxed value = operand.as<Boxed>();
        if (!(type.primitives & markerPrimitives))
            return compress(unboxCell(value));
        NarrowReference marker(m_out.castToInt32(value.value()));
        if (!type.hasCells())
            return marker;
        Condition isCell = above(value, undefined());
        return select(isCell, compress(unboxCell(value)), marker);
    }
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Number ConversionEmitter::numberAsIndex(Number word)
{
    Integer index = *indexBehind(word);
    return select(greaterThanOrEqual(index, integer(0)), toNumber(index), word);
}

Integer ConversionEmitter::bitsOf(Operand operand)
{
    switch (operand.representation) {
    case Representation::Double:
    case Representation::OptionalDouble:
        return Integer(m_out.bitCast(operand.value, Machine::Int64));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::RawWord:
        return Integer(operand.value);
    case Representation::Cell32:
        return Integer(m_out.zeroExt(operand.value, Machine::Int64));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::fromBits(Integer bits, Representation representation)
{
    switch (representation) {
    case Representation::Double:
    case Representation::OptionalDouble:
        return Operand(representation, m_out.bitCast(bits.value(), Machine::Double));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::RawWord:
        return Operand(representation, bits.value());
    case Representation::Cell32:
        return Operand(representation, m_out.castToInt32(bits.value()));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

uint64_t ConversionEmitter::markerBits(uint16_t primitive, Representation representation)
{
    std::optional<SlotMarkers> markers = markersOf(representation);
    RELEASE_ASSERT(markers);
    switch (primitive) {
    case Primitive::Undefined:
        return markers->undefined;
    case Primitive::Null:
        return markers->null;
    case Primitive::Absent:
        return markers->absent;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Condition ConversionEmitter::isMarker(Operand operand)
{
    RELEASE_ASSERT(operand.is<OptionalInteger>() || operand.is<OptionalNumber>());
    if (std::optional<Word32> narrow = operand.is<OptionalInteger>() ? narrowWordBehind(operand.as<OptionalInteger>()) : std::nullopt)
        return lessThan(*narrow, minInlineInt32);
    if (operand.is<OptionalNumber>() && (operand.value->opcode() == Machine::IToD || m_doubleElements.contains(operand.value)))
        return boolean(false);
    return greaterThanOrEqual(bitsOf(operand), static_cast<int64_t>(markerBits(Primitive::Undefined, operand.representation)));
}

std::optional<ConversionEmitter::ComparedInteger> ConversionEmitter::findComparedInteger(Operand operand)
{
    LValue number = operand.value;
    if (!operand.is<Number>() && !operand.is<OptionalNumber>())
        return std::nullopt;
    if (auto behind = m_optionalIntegersOfNumbers.getOptional(number))
        return ComparedInteger { Integer(behind->integer), true, behind->markerIsNaN || operand.is<Number>() };
    if (number->opcode() == Machine::IToD && number->child(0)->type() == Machine::Int32)
        return ComparedInteger { signExtended(Word32(number->child(0))), false };
    static constexpr double largestComparedConstant = 0x1p52;
    if (number->hasDouble() && std::abs(number->asDouble()) <= largestComparedConstant && std::trunc(number->asDouble()) == number->asDouble())
        return ComparedInteger { integer(static_cast<int64_t>(number->asDouble())), false };
    return std::nullopt;
}

Condition ConversionEmitter::isNullOrAbsent(OptionalNumber operand)
{
    return greaterThanOrEqual(bitsOf(operand), static_cast<int64_t>(markerBits(Primitive::Undefined, Representation::OptionalDouble) + 1));
}

Condition ConversionEmitter::isAbsent(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::JSValue:
        return isMarker(operand, markerBits(Primitive::Absent, operand.representation));
    case Representation::Cell32:
        static_assert(!cell32Markers.absent);
        return isZero(Word32(operand.value));
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
        return boolean(false);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::withoutMarkers(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
        if (std::optional<Word32> narrow = narrowWordBehind(operand.as<OptionalInteger>()))
            return signExtended(*narrow);
        return Integer(operand.value);
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::RawWord:
        return operand;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
