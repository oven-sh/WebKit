/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTConversionEmitter.h"

#if ENABLE(AOT)

#include "AOTCFG.h"
#include "AOTCheckedFrameCells.h"
#include "AOTConstantEmitter.h"
#include "AOTEdgeResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTNumberLiteralStrings.h"
#include "AOTPureOperations.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTSort.h"
#include "AOTStubsStringLiterals.h"
#include "B3BasicBlockInlines.h"
#include "B3BasicBlockUtils.h"
#include "B3Dominators.h"
#include "B3InsertionSetInlines.h"
#include "B3IntRange.h"
#include "B3ProcedureInlines.h"
#include "B3StackmapGenerationParams.h"
#include "JSCJSValueInlines.h"
#include "PureNaN.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <tuple>

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
    case Representation::LiteralString:
        return boxedMarkers;
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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
    if (value->opcode() != B3::IToD || value->child(0)->type() != B3::Int32)
        return std::nullopt;
    return Word32(value->child(0));
}

static B3::PatchpointValue* conversionInRegisters(Output& out, LType result, Number operand, B3::PureFunction function)
{
    B3::PatchpointValue* patchpoint = out.patchpoint(result);
    patchpoint->append(B3::ConstrainedValue(operand.value(), B3::ValueRep::SomeRegister));
    patchpoint->effects = B3::Effects::none();
    patchpoint->pureFunction = function;
    patchpoint->setRunsWithoutFrame(true);
    return patchpoint;
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalDouble:
        return toInteger(Number(operand.value));
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
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
    case Representation::OptionalInt64:
        return toNumber(Integer(operand.value), type.integers);
    case Representation::Double:
        return operand.as<Number>();
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return withoutBitsOfAnyNaN(operand);
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed ConversionEmitter::box(const OperationContext& context, Operand operand, const Type& type, bool isSeldomEntered)
{
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
        if (isSeldomEntered && !isWithinInt32(type.integers))
            return boxByStub(BoxedNumberForm::Integer, operand.value);
        return boxInteger(operand.as<Integer>(), type.integers);
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble: {
        Number number(operand.value);
        // The stub keeps the sign of a NaN and none of its other bits.
        if (isSeldomEntered && !number.value()->hasDouble() && !convertedInt32Of(number))
            return boxByStub(BoxedNumberForm::Double, (operand.is<ImpureNumber>() ? purified(number) : number).value());
        return boxNumber(operand);
    }
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return boxOptional(operand, type, isSeldomEntered);
    case Representation::Cell:
        return boxCell(operand.as<CellPointer>());
    case Representation::Cell32:
        return decode(operand.as<NarrowReference>(), type, Representation::JSValue).as<Boxed>();
    case Representation::JSValue:
        return operand.as<Boxed>();
    case Representation::LiteralString:
        return boxLiteralString(context, operand.as<LiteralStringWord>(), type);
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    fail(context, InternalErrorKind::UnsupportedConversion);
    return undefined();
}

bool ConversionEmitter::isSeldomEntered(const OperationContext& context) const
{
    if ((m_placeOnEdge ? m_placeOnEdge->frequency : m_function.at(context.block()).frequency).predicted() != BlockFrequency::Normal)
        return true;
    // The forms for such code call a leaf stub. Where that gives a function that calls nothing a frame, a stack check and a poll, only a coroutine body, which has them anyway, takes these forms in all but fast code.
    return (leavesCallerWithoutFrame(AOT::boxNumber(BoxedNumberForm::Double)) || m_function.role == FunctionRole::CoroutineBody) && shapeAt(context) != CodeShape::Fast;
}

Boxed ConversionEmitter::boxByStub(BoxedNumberForm form, LValue number)
{
    CallOptions options { .pureFunction = pureFunction(PureOperation::BoxNumber, static_cast<uint32_t>(form), true) };
    return Boxed(emitter<RuntimeCallEmitter>().callStubWithoutSite(AOT::boxNumber(form), singleElementSpan(number), options).first);
}

Boxed ConversionEmitter::boxOptional(Operand operand, const Type& type, bool isSeldomEntered)
{
    auto boxMarker = [&] {
        return convertMarkers(operand, type, Representation::JSValue).as<Boxed>();
    };
    auto boxUnmarked = [&] {
        return operand.is<OptionalInteger>() ? boxInteger(Integer(operand.value), type.integers) : boxNumber(Number(operand.value));
    };
    if (!(type.primitives & Primitive::Number))
        return boxMarker();
    if (isSeldomEntered)
        return boxByStub(operand.is<OptionalInteger>() ? BoxedNumberForm::OptionalInteger : BoxedNumberForm::OptionalDouble, operand.value);
    if (!(type.primitives & markerPrimitives))
        return boxUnmarked();

    LBasicBlock numberCase = newBlock();
    LBasicBlock markerCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(isMarker(operand).value(), unlikelyEdge(markerCase), normalEdge(numberCase));
    m_out.appendTo(numberCase);
    join.add(boxUnmarked());
    m_out.appendTo(markerCase);
    join.add(boxMarker());
    return join.finish();
}

Operand ConversionEmitter::unbox(Boxed value, const Type& type, Representation to, NumberUnboxing numberUnboxing)
{
    switch (to) {
    case Representation::Void:
        return { };
    case Representation::Boolean:
        return unboxBoolean(value);
    // The engine boxes an element of a double array as a double even when it is an int32, and a type test passes it as an integer.
    case Representation::Int32:
        return either<Word32>(isInt32(value), [&] {
            return unboxInt32(value);
        }, [&] {
            return low32(toInteger(unboxDouble(value)));
        });
    case Representation::Int64:
        return either<Integer>(isInt32(value), [&] {
            return signExtended(unboxInt32(value));
        }, [&] {
            return toInteger(unboxDouble(value));
        });
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
        return Operand(to, unbox(value, type, Representation::Double, numberUnboxing).value);
    case Representation::Double:
        if (!(type.primitives & Primitive::Integer))
            return unboxDouble(value);
        if (numberUnboxing == NumberUnboxing::ComputesBothSides) {
            build().statistics().count(StatisticID::NumbersUnboxedWithoutBranch);
            return numberToDouble(value);
        }
        return selectByBranch<Number>(isInt32(value), [&] {
            return toNumber(unboxInt32(value));
        }, [&] {
            return unboxDouble(value);
        });
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
        return unboxOptional(value, type, to, numberUnboxing);
    case Representation::Cell:
        return unboxCell(value);
    case Representation::Cell32:
        return compress(value, type);
    case Representation::JSValue:
        return value;
    case Representation::LiteralString:
        return toLiteralString(value, type);
    case Representation::Unchosen:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Operand ConversionEmitter::unboxOptional(Boxed value, const Type& type, Representation to, NumberUnboxing numberUnboxing)
{
    auto unboxMarker = [&] {
        return convertMarkers(value, type, to);
    };
    auto unboxNumber = [&] {
        return Operand(to, unbox(value, type, to == Representation::OptionalInt64 ? Representation::Int64 : Representation::Double, numberUnboxing).value);
    };
    if (!(type.primitives & Primitive::Number))
        return unboxMarker();
    if (!(type.primitives & markerPrimitives))
        return unboxNumber();

    LBasicBlock numberCase = newBlock();
    LBasicBlock markerCase = newBlock();
    OperandJoin join(m_out, to);
    m_out.branch(isNumber(value).value(), normalEdge(numberCase), unlikelyEdge(markerCase));
    m_out.appendTo(numberCase);
    join.add(unboxNumber());
    m_out.appendTo(markerCase);
    join.add(unboxMarker());
    return join.finish();
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
    return convertMarkers(operand, type, to, number);
}

Operand ConversionEmitter::inRepresentation(const OperationContext& context, Operand produced, const Type& producedType, Representation wanted)
{
    RELEASE_ASSERT(wanted != Representation::Unchosen);
    if (produced.representation == wanted)
        return produced;
    if (holdsAnyNaN(wanted)) {
        RELEASE_ASSERT(!produced.is<ImpureNumber>());
        return Operand(wanted, inRepresentation(context, produced, producedType, Representation::Double).value);
    }
    if (holdsAnyNaN(produced.representation) && wanted != Representation::JSValue)
        return inRepresentation(context, integerRepresentations.contains(wanted) ? Number(produced.value) : withoutBitsOfAnyNaN(produced), producedType, wanted);
    if (produced.is<Boxed>() && wanted != Representation::RawWord)
        return unbox(produced.as<Boxed>(), producedType, wanted);
    // A slot that holds integers or nothing is read where the type says no more than that a number comes.
    if (numberRepresentations.contains(wanted) && producedType.isNumber()) {
        produced = withoutMarkers(produced);
        if (produced.representation == wanted)
            return produced;
    }

    bool isNumber = numberRepresentations.contains(produced.representation);
    auto producedInteger = [&] {
        if (produced.is<Word32>())
            return signExtended(produced.as<Word32>());
        return produced.is<Integer>() ? produced.as<Integer>() : toInteger(produced.as<Number>());
    };

    switch (wanted) {
    case Representation::Void:
        return { };
    case Representation::JSValue:
        return box(context, produced, producedType);
    case Representation::Int32:
        if (isNumber)
            return narrow(producedInteger());
        break;
    case Representation::Int64:
        if (isNumber)
            return producedInteger();
        break;
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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
    case Representation::LiteralString:
        if (produced.is<CellPointer>())
            return toLiteralString(produced.as<CellPointer>());
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
        return boxedConstant(jsNumber(number.is<ImpureNumber>() ? purifyNaN(value.value()->asDouble()) : AOT::withoutPayload(value.value()->asDouble())));
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return boxInt32(*converted);

    Word32 low = low32(toInteger(value));
    Integer bitsOfLow = bitsOf(toNumber(low));
    return selectByBranch<Boxed>(equal(bitsOfLow, bitsOf(value)), [&] {
        return boxInt32(low);
    }, [&] {
        return boxDouble(number.is<Number>() ? value : withoutBitsOfAnyNaN(number));
    });
}

Boxed ConversionEmitter::boxInteger(Integer value, Range range)
{
    Word32 low = low32(value);
    if (isWithinInt32(range))
        return boxInt32(low);

    Condition fitsInt32 = isNonNegative(range) ? belowOrEqual(value, integer(INT32_MAX)) : equal(signExtended(low), value);
    LBasicBlock int32Case = newBlock();
    LBasicBlock doubleCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(fitsInt32.value(), normalEdge(int32Case), unlikelyEdge(doubleCase));
    m_out.appendTo(int32Case);
    join.add(boxInt32(low));
    m_out.appendTo(doubleCase);
    join.add(boxDouble(toNumber(value)));
    return join.finish();
}

Number ConversionEmitter::toNumber(Integer value, Range range)
{
    if (isWithinInt32(range))
        return toNumber(low32(value));
    return toNumber(value);
}

Integer ConversionEmitter::toInteger(Number value)
{
    if (value.value()->hasDouble() && std::abs(value.value()->asDouble()) < static_cast<double>(maxTrackedInteger))
        return integer(static_cast<int64_t>(value.value()->asDouble()));
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return signExtended(*converted);
    B3::PatchpointValue* patchpoint = conversionInRegisters(m_out, B3::Int64, value, truncationToInt64(0));
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
#if CPU(ARM64)
        // Beyond int64 x86-64 answers with the least int64, which made a number again is never the operand. ARM64 answers with the nearest int64, and the greatest one made a number again is 2^63.
        jit.m_assembler.cmn<64>(params[0].gpr(), UInt12(1));
        jit.m_assembler.cinc<64>(params[0].gpr(), params[0].gpr(), ARM64Assembler::ConditionVS);
#endif
    });
    return Integer(patchpoint);
}

#if CPU(ARM64)
Integer ConversionEmitter::toNearestInteger(Number value)
{
    B3::PatchpointValue* patchpoint = conversionInRegisters(m_out, B3::Int64, value, truncationToInt64(1));
    patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
        jit.truncateDoubleToInt64(params[1].fpr(), params[0].gpr());
    });
    return Integer(patchpoint);
}
#endif

// What is added to a product that may be rounded stays far below the 2^53 from which on it is.
static constexpr Range neverRounded { -(1ll << 51), 1ll << 51 };
// The product of two int32, with room for those terms within int64.
static constexpr Range productsBehindNumbers { -(1ll << 62), 1ll << 62 };

static std::optional<Range> rangeOfProduct(Range a, Range b)
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

static int64_t largestMagnitudeIn(Range range)
{
    return std::max(std::abs(range.minimum), std::abs(range.maximum));
}

static bool isArithmeticOnNumbers(LValue number)
{
    switch (number->opcode()) {
    case B3::Neg:
    case B3::Add:
    case B3::Sub:
    case B3::Mul:
        return true;
    default:
        return false;
    }
}

// Answers for every value of the arithmetic that makes a number, operands first, the left one before the right one.
// `a -= a - b` names a twice, and forty such lines lead to the first a in 2^40 ways: a value is asked once, however many ways lead to it.
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
        if (isArithmeticOnNumbers(value)) {
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

// What was read is likely a whole number if the program goes on to add it to an integer and truncate. What was computed, like `x + width * 0.5`, is as likely not, and a branch on it would be taken at random.
static bool isReadFromStorage(LValue number)
{
    return number->type() == B3::Double && (number->opcode() == B3::Load || number->opcode() == B3::Phi);
}

ConversionEmitter::ExactInteger ConversionEmitter::wholeNumberReadFromStorage(LValue number)
{
    Word32 word = low32(toInteger(Number(number)));
    return ExactInteger { .value = signExtended(word), .word = word, .holdsWhile = equal(toNumber(word), Number(number)) };
}

// One of the integers that the number is made of comes from a join, as a variable does that goes round a loop.
bool ConversionEmitter::holdsJoinedInteger(LValue number)
{
    answerOperandsFirst<bool>(number, m_holdsJoinedInteger, [&](LValue value) {
        if (value->opcode() == B3::IToD)
            return value->child(0)->opcode() == B3::Phi;
        if (!isArithmeticOnNumbers(value))
            return false;
        for (LValue operand : value->children()) {
            if (m_holdsJoinedInteger.get(operand))
                return true;
        }
        return false;
    });
    return m_holdsJoinedInteger.get(number);
}

// `(sum + row.id + name.length) | 0`: in integers two conversions go, and the test of `row.id` takes one.
bool ConversionEmitter::convertsSeveralIntegers(LValue number)
{
    answerOperandsFirst<ConvertedIntegers>(number, m_convertedIntegers, [&](LValue value) {
        if (value->opcode() == B3::IToD)
            return ConvertedIntegers { .first = value };
        ConvertedIntegers converted;
        if (!isArithmeticOnNumbers(value))
            return converted;
        for (LValue operand : value->children()) {
            ConvertedIntegers byOperand = m_convertedIntegers.get(operand);
            converted.areSeveral |= byOperand.areSeveral || (converted.first && byOperand.first && converted.first != byOperand.first);
            if (!converted.first)
                converted.first = byOperand.first;
        }
        return converted;
    });
    return m_convertedIntegers.get(number).areSeveral;
}

void ConversionEmitter::computeValueCycles()
{
    const CFG& cfg = analyses().cfg();
    unsigned numberOfValues = m_function.values.size();
    Vector<ValueID> parameterSources;
    Vector<std::pair<uint32_t, uint32_t>> parameterSourceRanges;
    parameterSourceRanges.fill({ 0, 0 }, numberOfValues);
    for (BlockID blockID : cfg.reversePostOrder()) {
        const auto& parameters = m_function.at(blockID).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index) {
            uint32_t start = parameterSources.size();
            for (Edge edge : cfg.incomingEdges(blockID)) {
                IncomingValue incoming = incomingValueOf(m_function, edge, index);
                if (incoming.kind == IncomingValue::Kind::Argument) {
                    parameterSources.append(incoming.argument);
                    continue;
                }
                EdgeResult result = edgeResultAt(m_function, incoming.result);
                switch (result.kind) {
                case EdgeResultKind::NarrowedOperand:
                    parameterSources.append(result.subject);
                    break;
                case EdgeResultKind::Guard:
                    break;
                case EdgeResultKind::DeclaredResult:
                    parameterSources.append(m_function.at(edge.from).terminator.operands());
                    break;
                }
            }
            parameterSourceRanges[parameters[index].index()] = { start, parameterSources.size() - start };
        }
    }

    struct Sources {
        size_t size() const { return values.size(); }
        unsigned operator[](size_t index) const { return values[index] ? values[index].index() : absentOperand; }

        std::span<const ValueID> values;
        unsigned absentOperand;
    };
    unsigned absentOperand = numberOfValues;
    m_valueCycles = computeStronglyConnectedComponents(numberOfValues + 1, [&](unsigned index) -> Sources {
        if (index == absentOperand)
            return { { }, absentOperand };
        const Value& value = m_function.at(ValueID(index));
        if (value.opcode == Opcode::Parameter)
            return { parameterSources.subspan(parameterSourceRanges[index].first, parameterSourceRanges[index].second), absentOperand };
        return { value.operands(), absentOperand };
    });
    for (unsigned index = 0; index < numberOfValues; ++index) {
        if (!m_valueCycles->isInCycle.get(index))
            continue;
        Representation representation = m_function.at(ValueID(index)).representation;
        if (representation == Representation::Int32 || representation == Representation::Int64)
            m_cyclesWithIntegers.set(m_valueCycles->componentOf[index]);
    }
}

// `sum = (sum + list[i]) | 0`: the sum is computed from an integer that the next iteration computes from the sum.
bool ConversionEmitter::isInLoopCarriedIntegerCycle(ValueID id)
{
    if (!analyses().cfg().hasCycle())
        return false;
    if (!m_valueCycles)
        computeValueCycles();
    return m_valueCycles->isInCycle.get(id.index()) && m_cyclesWithIntegers.get(m_valueCycles->componentOf[id.index()]);
}

// `sum = (sum + await next()) | 0`, `sum = (sum + parse(lines[i])) | 0`, `sum = (sum + totalOf(rows)) | 0` with the loops of totalOf() in line:
// an iteration that calls a function, suspends or runs a loop of its own takes longer than the three conversions, so the next one does not wait for them.
bool ConversionEmitter::isInLoopWithLongIterations(ValueID id)
{
    const NaturalLoops& loops = analyses().naturalLoops();
    BlockID blockOfValue = analyses().useLists().blockOf(id);
    const NaturalLoop* loop = blockOfValue ? loops.innerMostLoopOf(blockOfValue) : nullptr;
    if (!loop)
        return false;
    return m_loopsWithLongIterations.ensure(loop->header().index(), [&] {
        for (unsigned index = 0; index < loop->size(); ++index) {
            const Block& block = m_function.at(loop->at(index));
            if (block.frequency.predicted() != BlockFrequency::Normal)
                continue;
            if (loops.innerMostLoopOf(loop->at(index)) != loop)
                return true;
            for (ValueID value : block.values) {
                Opcode opcode = m_function.at(value).opcode;
                if (effectRuleOf(opcode) == EffectRule::Call || maySuspend(opcode))
                    return true;
            }
        }
        return false;
    }).iterator->value;
}

bool ConversionEmitter::isMadeOutsideLoopOf(LValue user, LValue number)
{
    ValueID userID = valueOfOrigin(user->origin());
    ValueID numberID = valueOfOrigin(number->origin());
    if (!userID || !numberID)
        return false;
    const UseLists& useLists = analyses().useLists();
    const NaturalLoops& loops = analyses().naturalLoops();
    BlockID blockOfUser = useLists.blockOf(userID);
    BlockID blockOfNumber = useLists.blockOf(numberID);
    const NaturalLoop* loop = blockOfUser ? loops.innerMostLoopOf(blockOfUser) : nullptr;
    return loop && blockOfNumber && !loops.belongsTo(blockOfNumber, *loop);
}

std::optional<ConversionEmitter::IntegerBehindNumber> ConversionEmitter::describeIntegerBehind(LValue number, bool mayTestWhatWasRead, const IntegersBehindNumbers& operands)
{
    auto converted = [&](LValue source, bool mayBeTested = false) -> std::optional<IntegerBehindNumber> {
        B3::IntRange known = B3::rangeFor(m_proc, source);
        Range range { known.min(), known.max() };
        if (range.isSubsetOf(neverRounded))
            return IntegerBehindNumber { .range = range };
        if (!mayBeTested || source->type() != B3::Int64)
            return std::nullopt;
        return IntegerBehindNumber { .range = Range::int32(), .isTested = true };
    };

    switch (number->opcode()) {
    case B3::ConstDouble: {
        double constant = number->asDouble();
        if (std::trunc(constant) != constant || std::abs(constant) > static_cast<double>(neverRounded.maximum) || (!constant && std::signbit(constant)))
            return std::nullopt;
        return IntegerBehindNumber { .range = Range::one(static_cast<int64_t>(constant)) };
    }
    case B3::IToD:
        return converted(number->child(0), true);
    case B3::Patchpoint: {
        const std::optional<B3::PureFunction>& function = number->as<B3::PatchpointValue>()->pureFunction;
        if (!function || !isRemainderThatIsNeverNaN(*function))
            return std::nullopt;
        return converted(number->child(0));
    }
    case B3::Neg: {
        std::optional<IntegerBehindNumber> operand = operands.get(number->child(0));
        if (!operand || operand->mayBeRounded)
            return std::nullopt;
        return IntegerBehindNumber { .range = Range { -operand->range.maximum, -operand->range.minimum }, .isTested = operand->isTested };
    }
    case B3::Mul: {
        std::optional<IntegerBehindNumber> left = operands.get(number->child(0));
        std::optional<IntegerBehindNumber> right = operands.get(number->child(1));
        if (!left || !right || left->mayBeRounded || right->mayBeRounded)
            return std::nullopt;
        std::optional<Range> range = rangeOfProduct(left->range, right->range);
        if (!range || !range->isSubsetOf(productsBehindNumbers))
            return std::nullopt;
        return IntegerBehindNumber { .range = *range, .mayBeRounded = !range->isSubsetOf(neverRounded), .isTested = left->isTested || right->isTested };
    }
    case B3::Add:
    case B3::Sub: {
        std::optional<IntegerBehindNumber> left = operands.get(number->child(0));
        std::optional<IntegerBehindNumber> right = operands.get(number->child(1));
        if (!left && !right)
            return std::nullopt;
        // `sum = (sum + list[i]) | 0` over a list held as numbers: in numbers each turn waits for the last through three conversions. A test of the element waits for the load alone.
        // `total += i & 255; return total & 1023` and `marks[k] = (marks[k] + i) & 1023`: nothing waits for the truncation, and the test would only come on top.
        std::optional<IntegerBehindNumber>& missing = left ? right : left;
        LValue operandWithoutInteger = number->child(left ? 1 : 0);
        if (!missing && isReadFromStorage(operandWithoutInteger) && (mayTestWhatWasRead || isMadeOutsideLoopOf(number, operandWithoutInteger)))
            missing = IntegerBehindNumber { .range = Range::int32(), .isTested = true };
        if (!left || !right || (left->mayBeRounded && right->mayBeRounded))
            return std::nullopt;
        bool isSum = number->opcode() == B3::Add;
        IntegerBehindNumber result {
            .range = isSum ? Range { left->range.minimum + right->range.minimum, left->range.maximum + right->range.maximum } : Range { left->range.minimum - right->range.maximum, left->range.maximum - right->range.minimum },
            .mayBeRounded = left->mayBeRounded || right->mayBeRounded,
            .isTested = left->isTested || right->isTested,
        };
        if (!result.mayBeRounded)
            return result.range.isSubsetOf(neverRounded) ? std::optional(result) : std::nullopt;
        const IntegerBehindNumber& rounded = left->mayBeRounded ? *left : *right;
        const IntegerBehindNumber& other = left->mayBeRounded ? *right : *left;
        result.largestSumOfOtherTerms = rounded.largestSumOfOtherTerms + largestMagnitudeIn(other.range);
        if (result.largestSumOfOtherTerms > neverRounded.maximum)
            return std::nullopt;
        return result;
    }
    default:
        return std::nullopt;
    }
}

ConversionEmitter::ExactInteger ConversionEmitter::computeIntegerBehind(LValue number, const IntegerBehindNumber& known, const ExactIntegers& operands)
{
    auto whileBoth = [&](Condition first, Condition second) {
        return first && second ? logicalAnd(first, second) : first ? first : second;
    };

    switch (number->opcode()) {
    case B3::ConstDouble:
        return ExactInteger { integer(known.range.minimum) };
    case B3::IToD:
    case B3::Patchpoint: {
        LValue source = number->child(0);
        if (known.isTested) {
            Integer wide(source);
            return ExactInteger { .value = wide, .holdsWhile = equal(signExtended(low32(wide)), wide) };
        }
        if (source->type() != B3::Int32)
            return ExactInteger { Integer(source) };
        return ExactInteger { .value = widen(Word32(source), known.range), .word = Word32(source) };
    }
    case B3::Neg: {
        ExactInteger operand = *operands.get(number->child(0));
        return ExactInteger { .value = sub(integer(0), operand.value), .holdsWhile = operand.holdsWhile };
    }
    case B3::Mul: {
        ExactInteger left = *operands.get(number->child(0));
        ExactInteger right = *operands.get(number->child(1));
        // B3 selects a multiplication that branches on overflow for the product of two sign-extended words.
        auto factor = [&](const ExactInteger& operand) {
            return operand.word ? signExtended(*operand.word) : operand.value;
        };
        Integer leftFactor = factor(left);
        return ExactInteger { .value = mul(leftFactor, factor(right)), .mayBeRounded = known.mayBeRounded, .holdsWhile = whileBoth(left.holdsWhile, right.holdsWhile) };
    }
    case B3::Add:
    case B3::Sub: {
        std::optional<ExactInteger> left = operands.get(number->child(0));
        std::optional<ExactInteger> right = operands.get(number->child(1));
        if (!left)
            left = wholeNumberReadFromStorage(number->child(0));
        else if (!right)
            right = wholeNumberReadFromStorage(number->child(1));
        return ExactInteger {
            .value = number->opcode() == B3::Add ? add(left->value, right->value) : sub(left->value, right->value),
            .mayBeRounded = known.mayBeRounded,
            .holdsWhile = whileBoth(left->holdsWhile, right->holdsWhile),
        };
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// No code is emitted until it is known that the whole of the arithmetic can be done in integers.
std::optional<ConversionEmitter::ExactInteger> ConversionEmitter::findExactIntegerBehind(LValue number, ValueID irValue, bool mayBeTested)
{
    bool mayTestWhatWasRead = mayBeTested && irValue && holdsJoinedInteger(number) && isInLoopCarriedIntegerCycle(irValue) && (convertsSeveralIntegers(number) || !isInLoopWithLongIterations(irValue));
    IntegersBehindNumbers& known = m_integersBehindNumbers[mayTestWhatWasRead];
    answerOperandsFirst<std::optional<IntegerBehindNumber>>(number, known, [&](LValue value) {
        return describeIntegerBehind(value, mayTestWhatWasRead, known);
    });
    std::optional<IntegerBehindNumber> knownOfNumber = known.get(number);
    if (!knownOfNumber || (knownOfNumber->isTested && !mayBeTested))
        return std::nullopt;

    ExactIntegers& computed = m_exactIntegers[mayTestWhatWasRead];
    answerOperandsFirst<std::optional<ExactInteger>>(number, computed, [&](LValue value) -> std::optional<ExactInteger> {
        std::optional<IntegerBehindNumber> knownOfValue = known.get(value);
        if (!knownOfValue)
            return std::nullopt;
        PlaceInBlock start = placeOfNextValue();
        ExactInteger exact = computeIntegerBehind(value, *knownOfValue, computed);
        rememberCodeOfExactInteger(value, start);
        return exact;
    });
    ExactInteger exact = *computed.get(number);
    useExactInteger(exact.value.value());
    if (exact.word)
        useExactInteger(exact.word->value());
    if (exact.holdsWhile)
        useExactInteger(exact.holdsWhile.value());
    return exact;
}

void ConversionEmitter::rememberCodeOfExactInteger(LValue number, PlaceInBlock start)
{
    RELEASE_ASSERT(m_out.m_block == start.block);
    for (unsigned index = start.index; index < start.block->size(); ++index) {
        LValue value = start.block->at(index);
        for (LValue operand : value->children())
            useExactInteger(operand);
        m_indexOfCodeOfExactInteger.add(value, static_cast<unsigned>(m_codeOfExactIntegers.size()));
        m_codeOfExactIntegers.append(CodeOfExactInteger { value, number, { start.block, index } });
    }
}

void ConversionEmitter::useExactInteger(LValue value)
{
    auto found = m_indexOfCodeOfExactInteger.find(value);
    if (found == m_indexOfCodeOfExactInteger.end() || m_codeOfExactIntegers[found->value].place.block == m_out.m_block)
        return;
    m_usesOfExactIntegersInOtherBlocks.append(UseOfExactInteger { found->value, placeOfNextValue() });
}

// `if (early) a = sum | 0; b = sum | 0;`: what was emitted in the arm moves in front of the branch.
void ConversionEmitter::placeExactIntegers()
{
    if (m_usesOfExactIntegersInOtherBlocks.isEmpty())
        return;

    LBasicBlock root = m_proc.at(0);
    for (LBasicBlock block : m_proc)
        block->predecessors().shrink(0);
    B3::updatePredecessorsAfter(root);
    B3::Dominators dominators(m_proc);
    auto isReached = [&](LBasicBlock block) {
        return block == root || block->numPredecessors();
    };

    Vector<std::optional<PlaceInBlock>> places;
    places.reserveInitialCapacity(m_codeOfExactIntegers.size());
    for (const CodeOfExactInteger& code : m_codeOfExactIntegers)
        places.append(isReached(code.place.block) ? std::optional(code.place) : std::nullopt);

    auto placeInFrontOf = [&](unsigned code, PlaceInBlock use) {
        std::optional<PlaceInBlock>& place = places[code];
        if (!place) {
            place = use;
            return;
        }
        if (place->block == use.block) {
            place->index = std::min(place->index, use.index);
            return;
        }
        if (dominators.strictlyDominates(place->block, use.block))
            return;
        if (dominators.strictlyDominates(use.block, place->block)) {
            place = use;
            return;
        }
        LBasicBlock blockOfNumber = m_codeOfExactIntegers[code].number->owner;
        if (!blockOfNumber)
            blockOfNumber = root;
        place = PlaceInBlock { blockOfNumber, static_cast<unsigned>(blockOfNumber->size() - 1) };
    };

    for (const UseOfExactInteger& use : m_usesOfExactIntegersInOtherBlocks) {
        if (isReached(use.place.block))
            placeInFrontOf(use.code, use.place);
    }
    for (unsigned code = m_codeOfExactIntegers.size(); code--;) {
        if (!places[code])
            continue;
        for (LValue operand : m_codeOfExactIntegers[code].value->children()) {
            auto found = m_indexOfCodeOfExactInteger.find(operand);
            if (found != m_indexOfCodeOfExactInteger.end())
                placeInFrontOf(found->value, *places[code]);
        }
    }

    Vector<unsigned> moved;
    for (unsigned code = 0; code < m_codeOfExactIntegers.size(); ++code) {
        const PlaceInBlock& emitted = m_codeOfExactIntegers[code].place;
        if (places[code] && (places[code]->block != emitted.block || places[code]->index != emitted.index))
            moved.append(code);
    }
    AOT::sort(moved, [&](unsigned a, unsigned b) {
        return std::tuple(places[a]->block->index(), places[a]->index, a) < std::tuple(places[b]->block->index(), places[b]->index, b);
    });
    for (unsigned code : moved) {
        const CodeOfExactInteger& emitted = m_codeOfExactIntegers[code];
        emitted.place.block->values()[emitted.place.index] = m_proc.add<B3::Value>(B3::Nop, B3::Void, emitted.value->origin());
    }
    B3::InsertionSet insertionSet(m_proc);
    for (size_t next = 0; next < moved.size();) {
        LBasicBlock block = places[moved[next]]->block;
        for (; next < moved.size() && places[moved[next]]->block == block; ++next)
            insertionSet.insertValue(places[moved[next]]->index, m_codeOfExactIntegers[moved[next]].value);
        insertionSet.execute(block);
    }
}

std::optional<Integer> ConversionEmitter::exactIntegerBehind(Number number)
{
    std::optional<ExactInteger> exact = findExactIntegerBehind(number.value(), { }, false);
    if (!exact)
        return std::nullopt;
    build().statistics().count(StatisticID::ExactIntegersBehindNumbers);
    return exact->value;
}

Integer ConversionEmitter::widen(Word32 value, Range range)
{
    return isNonNegative(range) ? zeroExtended(value) : signExtended(value);
}

Word32 ConversionEmitter::toInt32(Number value, BitsOfInt32 bits, ValueID irValue)
{
    if (std::optional<Word32> converted = convertedInt32Of(value))
        return *converted;
    std::optional<ExactInteger> exact = findExactIntegerBehind(value.value(), irValue);
    if (exact && !exact->holdsWhile) {
        build().statistics().count(StatisticID::ExactIntegersBehindNumbers);
        if (!exact->mayBeRounded)
            return low32(exact->value);
        // An exact result that is an int32 was not rounded. Any other lies within int64, and so does the number.
        return either<Word32>(equal(signExtended(low32(exact->value)), exact->value), [&] {
            return low32(exact->value);
        }, [&] {
            return low32(toInteger(value));
        });
    }
    if (exact) {
        build().statistics().count(StatisticID::ExactIntegersBehindNumbers);
        Condition isExact = exact->holdsWhile;
        if (exact->mayBeRounded)
            isExact = logicalAnd(isExact, equal(signExtended(low32(exact->value)), exact->value));
        return either<Word32>(isExact, [&] {
            return low32(exact->value);
        }, [&] {
            return toInt32OfAnyNumber(value, bits);
        });
    }
    return toInt32OfAnyNumber(value, bits);
}

Word32 ConversionEmitter::toInt32OfAnyNumber(Number value, BitsOfInt32 bits)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics(build().targetFeatures())) {
        B3::PatchpointValue* patchpoint = conversionInRegisters(m_out, B3::Int32, value, pureFunction(PureOperation::ToInt32, 0, true));
        patchpoint->setGenerator([](CCallHelpers& jit, const B3::StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
        });
        return Word32(patchpoint);
    }
#endif
    Integer truncated = toInteger(value);
    if (bits == BitsOfInt32::Lowest)
        return low32(truncated);
    // x86-64 truncates whatever is outside int64 to the least of them. ARM64 stops at either end.
    Condition fits;
    if (isX86_64())
        fits = notEqual(truncated, std::numeric_limits<int64_t>::min());
    else {
        Integer one = integer(1);
        fits = below(one, add(truncated, std::numeric_limits<int64_t>::min() + 1));
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

Number ConversionEmitter::withoutPayload(Number value)
{
    return either<Number>(equal(value, value), [&] {
        return value;
    }, [&] {
        return Number(m_out.bitCast(quietNaNWithSignOf(bitsOf(value)).value(), B3::Double));
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
        Boxed marker(m_out.zeroExt(word.value(), B3::Int64));
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

NarrowReference ConversionEmitter::compress(Operand operand, const Type& type)
{
    // A narrow reference never refers to a cell kept in a frame. No other value has the upper half of the addresses used by AOTCheckedFrameCells.h.
    auto checkIsNotInFrame = [&](LValue bits) {
        if (!generator().validatesOrChecksProofs())
            return;
        LBasicBlock liesInFrame = newBlock();
        LBasicBlock continuation = newBlock();
        LValue upperHalf = m_out.lShr(bits, m_out.constInt32(CheckedFrameCells::bitsOfOffsetInRange));
        m_out.branch(m_out.equal(upperHalf, m_out.constInt64(CheckedFrameCells::startOfRange >> CheckedFrameCells::bitsOfOffsetInRange)), rareEdge(liesInFrame), normalEdge(continuation));
        m_out.appendTo(liesInFrame);
        Operand arguments[] { Pointer(bits) };
        callWithoutSite(RuntimeOperation::ReportEscapedFrameCell, arguments, Representation::Void, { .isOnRarePath = true });
        m_out.unreachable();
        m_out.appendTo(continuation);
    };

    switch (operand.representation) {
    case Representation::Void:
        RELEASE_ASSERT(isValueWithoutBits(type));
        return convertMarkers(operand, type, Representation::Cell32).as<NarrowReference>();
    case Representation::Cell:
        checkIsNotInFrame(operand.value);
        return compress(operand.as<CellPointer>());
    case Representation::Cell32:
        return operand.as<NarrowReference>();
    case Representation::JSValue: {
        Boxed value = operand.as<Boxed>();
        if (type.hasCells())
            checkIsNotInFrame(operand.value);
        if (!(type.primitives & markerPrimitives))
            return compress(unboxCell(value));
        NarrowReference marker(low32(value).value());
        if (!type.hasCells())
            return marker;
        // The empty value passes isCell.
        Condition isCell = above(value, undefined());
        return select(isCell, compress(unboxCell(value)), marker);
    }
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::LiteralString:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Integer ConversionEmitter::bitsOf(Operand operand)
{
    switch (operand.representation) {
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalDouble:
        return Integer(m_out.bitCast(operand.value, B3::Int64));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return Integer(operand.value);
    case Representation::Cell32:
        return Integer(m_out.zeroExt(operand.value, B3::Int64));
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
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalDouble:
        return Operand(representation, m_out.bitCast(bits.value(), B3::Double));
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::LiteralString:
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
    return greaterThanOrEqual(bitsOf(operand), static_cast<int64_t>(markerBits(Primitive::Undefined, operand.representation)));
}

Condition ConversionEmitter::isAbsent(Operand operand)
{
    switch (operand.representation) {
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::JSValue:
    case Representation::LiteralString:
        return isMarker(operand, markerBits(Primitive::Absent, operand.representation));
    case Representation::Cell32:
        static_assert(!cell32Markers.absent);
        return isZero(Word32(operand.value));
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
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
        return Integer(operand.value);
    case Representation::OptionalDouble:
        return Number(operand.value);
    case Representation::Unchosen:
    case Representation::Void:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::Cell:
    case Representation::Cell32:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return operand;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LiteralStringWord ConversionEmitter::literalStringConstant(StringID string)
{
    return LiteralStringWord(m_out.constInt64(literalStringWord(inputs().literalStrings.numberOf(string))));
}

LiteralStringWord ConversionEmitter::toLiteralString(CellPointer string)
{
    return toLiteralString(Operand(string), m_program.at(TypeTable::string));
}

LiteralStringWord ConversionEmitter::toLiteralString(Operand operand, const Type& type)
{
    switch (operand.representation) {
    case Representation::Void:
        RELEASE_ASSERT(isValueWithoutBits(type));
        return convertMarkers(operand, type, Representation::LiteralString).as<LiteralStringWord>();
    case Representation::Cell:
    case Representation::JSValue: {
        if (!(type.primitives & Primitive::AnyString))
            return LiteralStringWord(operand.value);
        CallOptions options { .pureFunction = pureFunction(PureOperation::ToLiteralString, 0, true) };
        // The stub takes every cell for a string.
        if (type.hasOnlyPrimitives(Primitive::AnyString | markerPrimitives))
            return LiteralStringWord(emitter<RuntimeCallEmitter>().callStubWithoutSite(AOT::toLiteralString(type.isAnyString() ? ComparedOperand::String : ComparedOperand::Value), singleElementSpan(operand.value), options).first);
        return LiteralStringWord(callWithoutSite<Boxed>(RuntimeOperation::ToLiteralString, { Boxed(operand.value) }, options).value());
    }
    case Representation::LiteralString:
        return operand.as<LiteralStringWord>();
    case Representation::Unchosen:
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Int64:
    case Representation::Double:
    case Representation::DoubleWithPayload:
    case Representation::ImpureDouble:
    case Representation::OptionalInt64:
    case Representation::OptionalDouble:
    case Representation::Cell32:
    case Representation::RawWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Boxed ConversionEmitter::boxLiteralString(const OperationContext& context, LiteralStringWord word, const Type& type)
{
    const bool isNarrow = !m_options.recorded.useWideReferences();
    auto callConversionStub = [&](LiteralStringWord literal) {
        return boxCell(CellPointer(callStub(context, AOT::literalStringToString(), { literal.value() }, { .pureFunction = pureFunction(PureOperation::LiteralStringToString, 0, true) }).first));
    };
    const ptrdiff_t offsetOfCells = isNarrow ? AOT_INSTANCE_OFFSET(runtime.constants.literalStrings) : AOT_INSTANCE_OFFSET(runtime.constants.wideLiteralStrings);
    auto loadOrCreateCell = [&](LiteralStringWord literal, TypedPointer cellWord) {
        return boxCell(emitter<ConstantEmitter>().permanentCell(cellWord, isNarrow, [&] {
            return callOperation<CellPointer>(context, RuntimeOperation::LiteralStringToString, { literal }, { .pureFunction = pureFunction(PureOperation::LiteralStringToString, 0, true), .isOnRarePath = true });
        }));
    };

    std::optional<uint32_t> number;
    if (word.value()->hasInt64()) {
        uint64_t bits = word.value()->asInt64();
        if (!isLiteralStringWord(bits))
            return Boxed(word.value());
        number = literalStringIndex(static_cast<uint32_t>(bits));
    } else if (StringID literal = type.singleStringLiteral()) {
        // A literal without a number has no word, so the operand is its string cell.
        number = inputs().literalStrings.indexOf(literal);
        if (!number)
            return Boxed(word.value());
    }
    if (number) {
        RELEASE_ASSERT(*number < inputs().literalStrings.strings().size());
        if (!*number)
            return boxCell(emptyString());
        if (shapeAt(context) != CodeShape::Fast)
            return callConversionStub(LiteralStringWord(m_out.constInt64(literalStringWord(*number))));
        Pointer cells = loadImmutablePointer(offsetOfCells);
        ptrdiff_t sizeOfCellWord = isNarrow ? sizeof(uint32_t) : sizeof(JSCell*);
        TypedPointer cellWord = m_out.address(m_heaps.AOTInstance_stringCells, cells.value(), static_cast<ptrdiff_t>(*number) * sizeOfCellWord);
        return loadOrCreateCell(LiteralStringWord(m_out.constInt64(literalStringWord(*number))), cellWord);
    }

    if (shapeAt(context) != CodeShape::Fast)
        return callConversionStub(word);

    static_assert(1 << literalStringShift == sizeof(uint32_t) && 2 << literalStringShift == sizeof(JSCell*));
    Integer scaledNumber = sub(Integer(word.value()), integer(emptyLiteralStringWord));
    bool isAlwaysNumber = type.hasOnlyPrimitives(Primitive::LiteralString) && std::ranges::all_of(type.stringLiterals, [&](StringID literal) {
        return inputs().literalStrings.indexOf(literal).has_value();
    });
    if (isAlwaysNumber)
        return loadOrCreateCell(word, m_out.baseIndex(m_heaps.AOTInstance_stringCells, loadImmutablePointer(offsetOfCells).value(), scaledNumber.value(), isNarrow ? FTL::ScaleOne : FTL::ScaleTwo));
    LBasicBlock literalCase = newBlock();
    LBasicBlock boxedCase = newBlock();
    Join<Boxed> join(m_out);
    m_out.branch(below(scaledNumber, integer(minCell32 - emptyLiteralStringWord)).value(), normalEdge(literalCase), normalEdge(boxedCase));

    m_out.appendTo(boxedCase);
    join.add(Boxed(word.value()));

    m_out.appendTo(literalCase);
    Pointer cells = loadImmutablePointer(offsetOfCells);
    join.add(loadOrCreateCell(word, m_out.baseIndex(m_heaps.AOTInstance_stringCells, cells.value(), scaledNumber.value(), isNarrow ? FTL::ScaleOne : FTL::ScaleTwo)));
    return join.finish();
}

Condition ConversionEmitter::isStringCell(LiteralStringWord word)
{
    if (std::optional<NarrowReference> narrow = narrowWordBehind(word))
        return isCellReferenceAmongAllWords(*narrow);
    return aboveOrEqual(word, LiteralStringWord(m_out.constInt64(minCell32)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
