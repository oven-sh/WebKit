/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "B3StackmapGenerationParams.h"
#include "JSCInlines.h"
#include "JSGlobalObjectFunctions.h"
#include <wtf/Int128.h>

namespace JSC { namespace AOT { namespace Typed {

using namespace B3;

static bool isExact(Int128 minimum, Int128 maximum)
{
    return minimum >= -maxExactInteger && maximum <= maxExactInteger;
}

static bool isCounterStep(const Type& type)
{
    constexpr int64_t maxStep = 1024;
    return type.integers.minimum >= -maxStep && type.integers.maximum <= maxStep;
}

static bool isWithinInt32(const Type& type)
{
    return type.isInteger() && type.integers.isSubsetOf(Range::int32());
}

static bool isStringOrNullish(const Type& type)
{
    return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
}

static Type stubResultType(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Add:
        return Type::fromPrimitives(Primitive::Number | Primitive::BigInt | Primitive::String);
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::BitNot: {
        Type type = Type::integer(Range::int32());
        type.primitives |= Primitive::BigInt;
        return type;
    }
    case Opcode::URShift:
        return Type::integer(Range::uint32());
    case Opcode::ToNumber:
        return Type::fromPrimitives(Primitive::Number);
    default:
        return Type::fromPrimitives(Primitive::Number | Primitive::BigInt);
    }
}

static Type stubResultType(Opcode opcode, const Type& left, const Type& right)
{
    Type guarantee = stubResultType(opcode);
    switch (opcode) {
    case Opcode::Add:
        if (left.isString() || right.isString())
            return Type::fromPrimitives(Primitive::String);
        break;
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
        if (!left.isNumber() && !right.isNumber())
            break;
        guarantee.primitives &= ~Primitive::BigInt;
        if (opcode != Opcode::BitAnd)
            break;
        for (const Type* mask : { &left, &right }) {
            if (mask->isInteger() && mask->integers.minimum >= 0 && mask->integers.maximum <= guarantee.integers.maximum)
                guarantee.integers = Range { 0, mask->integers.maximum };
        }
        break;
    default:
        break;
    }
    return guarantee;
}

Number Lowering::asNumericOperand(ValueID id)
{
    auto& type = typeOf(id);
    if (type.isNumber())
        return in<Number>(id);
    OptionalNumber optional = in<OptionalNumber>(id);
    Number number(m_out.m_block->appendNew<B3::Value>(m_proc, B3::Identity, m_out.origin(), optional.value()));
    if (!(type.primitives & Primitive::Null))
        return number;
    return select(equal(bitsOf(optional), integer(markerBits(Primitive::Null, Rep::OptionalDouble))), this->number(0), number);
}

std::optional<Operand> Lowering::numberAndValueArithmeticWithStub(const Value& value, const Type& type, OperationEffects effects)
{
    TypeID wanted = m_function.at(m_currentBlock->values[m_currentIndexInBlock]).type;
    if (repOf(type) != Rep::Double || !type.isNumber() || !wanted || &m_program.at(wanted) != &type)
        return std::nullopt;
    if (m_useCastTest || handlerEnteredByBranch() || Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts())
        return std::nullopt;
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto isRawNumber = [&](ValueID id) {
        return get(id).rep == Rep::Double && typeOf(id).isNumber();
    };
    bool numberIsOnRight = isRawNumber(b) && get(a).rep == Rep::JSValue;
    if (!numberIsOnRight && !(isRawNumber(a) && get(b).rep == Rep::JSValue))
        return std::nullopt;
    Stub stub;
    switch (value.opcode) {
    case Opcode::Add:
        stub = Stub::AddDoubleAsDouble;
        break;
    case Opcode::Mul:
        stub = Stub::MulDoubleAsDouble;
        break;
    case Opcode::Sub:
        stub = numberIsOnRight ? Stub::SubDoubleOnRightAsDouble : Stub::SubDoubleAsDouble;
        break;
    case Opcode::Div:
        stub = numberIsOnRight ? Stub::DivDoubleOnRightAsDouble : Stub::DivDoubleAsDouble;
        break;
    default:
        return std::nullopt;
    }
    uint32_t typeAndWhetherNumberIsOnRight = wanted.index() | (numberIsOnRight ? 1u << 31 : 0);
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(Double);
    applyEffects(patchpoint, effects);
    patchpoint->append(ConstrainedValue(in<Number>(numberIsOnRight ? b : a).value(), ValueRep::reg(FPRInfo::fpRegT0)));
    patchpoint->append(ConstrainedValue(in<Boxed>(numberIsOnRight ? a : b).value(), ValueRep::reg(GPRInfo::argumentGPR1)));
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(RegisterSet { callMarkerGPR });
    patchpoint->clobberLate(RegisterSet::registersToSaveForCCall(RegisterSet::allScalarRegisters()));
#if CPU(X86_64)
    for (GPRReg reg : stubTemporaryGPRs)
        patchpoint->clobberLate(RegisterSet { reg });
#endif
    patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::fpRegT0) };
    CallSite site { nextSite() };
    patchpoint->setGenerator([stubCalls = &m_context.stubCalls, stub, typeAndWhetherNumberIsOnRight, site](CCallHelpers& jit, const StackmapGenerationParams&) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        jit.move(CCallHelpers::TrustedImm32(typeAndWhetherNumberIsOnRight), GPRInfo::argumentGPR2);
        stubCalls->call(jit, stub, site);
    });
    unwindToLandingPad(patchpoint, site);
    return Operand(Rep::Double, patchpoint);
}

Operand Lowering::lowerBinary(ValueID result, const Value& value)
{
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    auto& type = typeOf(result);
    if (isNumericOperand(left) && isNumericOperand(right))
        return lowerNumericBinary(value.opcode, a, b, type);

    OperationEffects effects { writesOfArithmetic(left, &right), true };
    if (value.opcode == Opcode::Add && !left.hasObjects() && !right.hasObjects() && !isCompact() && !prefersGenericPathInColdCode()) {
        bool isStaticallyCold = !m_isInLoop && !m_program.isHotOutsideLoops(m_id);
        if (left.isString() || right.isString()) {
            if (isStaticallyCold) {
                CellPointer first = toStringCell(a);
                return ensure(concatenateByStub(first, toStringCell(b)), m_program.at(m_program.types.string), type);
            }
            StringPart first = stringPartOf(a);
            return ensure(concatenate(first, stringPartOf(b)).string, m_program.at(m_program.types.string), type);
        }
        if (isStringOrNullish(left) && isStringOrNullish(right)) {
            Boxed x = in<Boxed>(a);
            Boxed y = in<Boxed>(b);
            bool areIntegral = !indexOf(x.value()) && !indexOf(y.value()) && !m_decoded.contains(x.value()) && !m_decoded.contains(y.value());
            Word32 areCells = areIntegral ? Word32(m_out.testIsZero64(m_out.bitOr(x.value(), y.value()), m_out.constInt64(JSValue::OtherTag))) : logicalAnd(isCell(x, left), isCell(y, right));
            return eitherOperand(areCells, joinRepFor(type), [&] {
                if (isStaticallyCold)
                    return ensure(concatenateByStub(asCell(x), asCell(y)), m_program.at(m_program.types.string), type);
                return ensure(concatenate(stringPartOf(asCell(x)), stringPartOf(asCell(y))).string, m_program.at(m_program.types.string), type);
            }, [&] {
                return ensure(emitGenericArithmetic(Stub::Add, x, y, effects), stubResultType(Opcode::Add), type);
            });
        }
        if (isStringOrNullish(left) || isStringOrNullish(right)) {
            bool isLeft = isStringOrNullish(left);
            Boxed x = in<Boxed>(a);
            Boxed y = in<Boxed>(b);
            return eitherOperand(isCell(isLeft ? x : y, isLeft ? left : right), joinRepFor(type), [&] {
                if (isStaticallyCold) {
                    CellPointer string = asCell(isLeft ? x : y);
                    CellPointer other = toStringCell(isLeft ? b : a);
                    return ensure(isLeft ? concatenateByStub(string, other) : concatenateByStub(other, string), m_program.at(m_program.types.string), type);
                }
                StringPart string = stringPartOf(asCell(isLeft ? x : y));
                StringPart other = stringPartOf(isLeft ? b : a);
                return ensure((isLeft ? concatenate(string, other) : concatenate(other, string)).string, m_program.at(m_program.types.string), type);
            }, [&] {
                return ensure(emitGenericArithmetic(Stub::Add, x, y, effects), stubResultType(Opcode::Add), type);
            });
        }
    }

    auto genericPath = [&] {
        if (auto raw = numberAndValueArithmeticWithStub(value, type, effects))
            return *raw;
        Boxed x = in<Boxed>(a);
        Boxed y = in<Boxed>(b);
        Boxed boxed;
        switch (value.opcode) {
#define AOT_TYPED_GENERIC_BINARY(opcode) \
        case Opcode::opcode: \
            boxed = emitGenericArithmetic(Stub::opcode, x, y, effects); \
            break;
        AOT_TYPED_GENERIC_BINARY(Add)
        AOT_TYPED_GENERIC_BINARY(Sub)
        AOT_TYPED_GENERIC_BINARY(Mul)
        AOT_TYPED_GENERIC_BINARY(Div)
        AOT_TYPED_GENERIC_BINARY(Mod)
        AOT_TYPED_GENERIC_BINARY(BitAnd)
        AOT_TYPED_GENERIC_BINARY(BitOr)
        AOT_TYPED_GENERIC_BINARY(BitXor)
        AOT_TYPED_GENERIC_BINARY(LShift)
        AOT_TYPED_GENERIC_BINARY(RShift)
        AOT_TYPED_GENERIC_BINARY(URShift)
#undef AOT_TYPED_GENERIC_BINARY
        case Opcode::Pow:
            boxed = AOT_TYPED_OPERATION_WITH_EFFECTS(Exp, effects, x, y);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (value.opcode == Opcode::URShift)
            return ensure(zeroExtended(low32(boxed)), stubResultType(value.opcode), type);
        Type guarantee = stubResultType(value.opcode, left, right);
        if (guarantee.isInteger())
            return ensure(signExtended(low32(boxed)), guarantee, type);
        return ensure(boxed, guarantee, type);
    };

    if (prefersGenericPathInColdCode())
        return genericPath();
    Rep rep = repOf(type);
    if (arePredictedNumbers({ a, b })) {
        bool hasCodeForInt32 = value.opcode != Opcode::Div && value.opcode != Opcode::Pow;
        bool isSum = value.opcode == Opcode::Add || value.opcode == Opcode::Sub;
        bool isOnBits = value.opcode == Opcode::BitAnd || value.opcode == Opcode::BitOr || value.opcode == Opcode::BitXor || value.opcode == Opcode::LShift || value.opcode == Opcode::RShift || value.opcode == Opcode::URShift;
        return ifPredictedNumbers({ a, b }, rep, hasCodeForInt32, [&](LBasicBlock otherwise) {
            if (isSum && !(isWithinInt32(typeOf(a)) && isWithinInt32(typeOf(b)))) {
                Integer x = get(a).as<Integer>();
                Integer y = get(b).as<Integer>();
                return int32ResultOrElse(value.opcode == Opcode::Add ? add(x, y) : sub(x, y), type, otherwise);
            }
            return lowerInt32Binary(value.opcode, a, b, type, otherwise);
        }, [&] {
            if ((value.opcode == Opcode::Div || value.opcode == Opcode::Mod || value.opcode == Opcode::Pow) && rep == Rep::JSValue) {
                auto& numberType = m_program.at(m_program.types.number);
                return ensure(boxNumber(lowerNumericBinary(value.opcode, a, b, numberType).as<Number>()), numberType, type);
            }
            return lowerNumericBinary(value.opcode, a, b, type);
        }, genericPath, isSum || isOnBits);
    }

    if (value.opcode == Opcode::Add && (predictionOf(a) || predictionOf(b))) {
        auto predictedOperandType = [&](ValueID id) -> const Type* {
            auto prediction = predictionOf(id);
            if (!prediction)
                return typeOf(id).isString() || typeOf(id).isNumber() ? &typeOf(id) : nullptr;
            if (prediction->isString())
                return &m_program.at(m_program.types.string);
            if (prediction->isNumber())
                return &m_program.at(m_program.types.int32);
            return nullptr;
        };
        const Type* first = predictedOperandType(a);
        const Type* second = predictedOperandType(b);
        if (first && second && (first->isString() || second->isString())) {
            return ifPredictionHolds({ { a, first }, { b, second } }, rep, [&](LBasicBlock) {
                return lowerBinary(result, value);
            }, genericPath);
        }
    }
    return genericPath();
}

Operand Lowering::int32ResultOrElse(Integer exact, const Type& type, LBasicBlock otherwise)
{
    if (repOf(type) != Rep::JSValue)
        return ensure(exact, Type::integer(Range::full()), type);
    orElse(equal(signExtended(low32(exact)), exact), otherwise);
    return ensure(exact, m_program.at(m_program.types.int32), type);
}

Word32 Lowering::int32OrElse(Opcode opcode, Word32 x, Word32 y, LBasicBlock otherwise, const char* file, unsigned line)
{
    if (dropsGuardAt(file, line)) [[unlikely]]
        return opcode == Opcode::Add ? add(x, y) : opcode == Opcode::Sub ? Word32(m_out.sub(x.value(), y.value())) : mul(x, y);
    if (!m_isValidatingFact && isForcedForTesting(file, line)) [[unlikely]]
        Emitter::orElse(m_out.constInt32(0), otherwise);
    if (m_isInLoop && opcode != Opcode::Mul) {
        Integer exact = opcode == Opcode::Add ? add(signExtended(x), signExtended(y)) : sub(signExtended(x), signExtended(y));
        Emitter::orElse(equal(signExtended(low32(exact)), exact).value(), otherwise);
        return low32(exact);
    }
    auto constantOf = [](LValue value) -> std::optional<int32_t> {
        if (value->hasInt32())
            return value->asInt32();
        if (value->opcode() == B3::Trunc && value->child(0)->hasInt64())
            return static_cast<int32_t>(value->child(0)->asInt64());
        return std::nullopt;
    };
    if (opcode == Opcode::Add && constantOf(x.value()))
        std::swap(x, y);
    std::optional<int32_t> constant = opcode == Opcode::Mul ? std::nullopt : constantOf(y.value());
    if (constant && (*constant <= -4096 || *constant >= 4096))
        constant = std::nullopt;
    LBasicBlock continuation = m_out.newBlock();
    PatchpointValue* patchpoint = m_out.patchpoint(Int32);
    patchpoint->effects = Effects::none();
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(x.value(), ValueRep::SomeRegister));
    if (!constant)
        patchpoint->append(ConstrainedValue(y.value(), opcode == Opcode::Sub ? ValueRep::SomeLateRegister : ValueRep::SomeRegister));
    m_out.appendSuccessor(FTL::rarely(otherwise));
    m_out.appendSuccessor(usually(continuation));
    patchpoint->setGenerator([opcode, constant](CCallHelpers& jit, const StackmapGenerationParams& params) {
        GPRReg result = params[0].gpr();
        GPRReg left = params[1].gpr();
        RELEASE_ASSERT(opcode != Opcode::Sub || constant || result != params[2].gpr());
        CCallHelpers::Jump notInt32;
        if (opcode == Opcode::Add)
            notInt32 = constant ? jit.branchAdd32(CCallHelpers::Overflow, left, CCallHelpers::TrustedImm32(*constant), result) : jit.branchAdd32(CCallHelpers::Overflow, left, params[2].gpr(), result);
        else if (opcode == Opcode::Sub)
            notInt32 = constant ? jit.branchSub32(CCallHelpers::Overflow, left, CCallHelpers::TrustedImm32(*constant), result) : jit.branchSub32(CCallHelpers::Overflow, left, params[2].gpr(), result);
        else
            notInt32 = jit.branchMul32(CCallHelpers::Overflow, left, params[2].gpr(), result);
        CCallHelpers::Jump isInt32;
        if (!params.fallsThroughToSuccessor(1))
            isInt32 = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            notInt32.linkTo(*labels[0], &jit);
            if (isInt32.isSet())
                isInt32.linkTo(*labels[1], &jit);
        });
    });
    m_out.appendTo(continuation);
    return Word32(patchpoint);
}

Operand Lowering::lowerInt32Binary(Opcode opcode, ValueID a, ValueID b, const Type& type, LBasicBlock otherwise)
{
    bool isBoxed = repOf(type) == Rep::JSValue;
    switch (opcode) {
    case Opcode::Add:
        if (isBoxed)
            return ensure(signExtended(int32OrElse(opcode, low32(get(a).as<Integer>()), low32(get(b).as<Integer>()), otherwise)), m_program.at(m_program.types.int32), type);
        return int32ResultOrElse(add(get(a).as<Integer>(), get(b).as<Integer>()), type, otherwise);
    case Opcode::Sub:
        if (isBoxed)
            return ensure(signExtended(int32OrElse(opcode, low32(get(a).as<Integer>()), low32(get(b).as<Integer>()), otherwise)), m_program.at(m_program.types.int32), type);
        return int32ResultOrElse(sub(get(a).as<Integer>(), get(b).as<Integer>()), type, otherwise);
    case Opcode::Mul: {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        if (isBoxed) {
            Word32 product = int32OrElse(opcode, low32(x), low32(y), otherwise);
            LBasicBlock isZeroCase = newUnlikelyBlock();
            LBasicBlock continuation = m_out.newBlock();
            m_out.branch(isZero(product).value(), rarely(isZeroCase), usually(continuation));
            m_out.appendTo(isZeroCase);
            orElse(greaterThanOrEqual(bitOr(low32(x), low32(y)), word32(0)), otherwise);
            m_out.jump(continuation);
            m_out.appendTo(continuation);
            return ensure(signExtended(product), m_program.at(m_program.types.int32), type);
        }
        Integer product = mul(x, y);
        orElse(logicalOr(isNotZero(product), greaterThanOrEqual(bitOr(x, y), integer(0))), otherwise);
        return int32ResultOrElse(product, type, otherwise);
    }
    case Opcode::Mod: {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        orElse(logicalAnd(isNotZero(y), greaterThanOrEqual(x, integer(0))), otherwise);
        return ensure(mod(x, y), m_program.at(m_program.types.int32), type);
    }
    default:
        return lowerNumericBinary(opcode, a, b, type);
    }
}

Integer Lowering::lowerBitwise(Opcode opcode, ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (isWithinInt32(left) && isWithinInt32(right)) {
        switch (opcode) {
        case Opcode::BitAnd:
            return bitAnd(get(a).as<Integer>(), get(b).as<Integer>());
        case Opcode::BitOr:
            return bitOr(get(a).as<Integer>(), get(b).as<Integer>());
        case Opcode::BitXor:
            return bitXor(get(a).as<Integer>(), get(b).as<Integer>());
        default:
            break;
        }
    }
    auto isMask = [](const Type& type) {
        return type.isInteger() && type.integers.isSubsetOf({ 0, INT32_MAX });
    };
    if (opcode == Opcode::BitAnd && left.isInteger() && right.isInteger() && (isMask(left) || isMask(right)))
        return bitAnd(get(a).as<Integer>(), get(b).as<Integer>());
    if (opcode == Opcode::RShift && isWithinInt32(left))
        return Integer(m_out.aShr(get(a).as<Integer>().value(), bitAnd(toInt32(b), word32(31)).value()));

    Word32 x = toInt32(a);
    Word32 y = toInt32(b);
    switch (opcode) {
    case Opcode::BitAnd:
        return signExtended(bitAnd(x, y));
    case Opcode::BitOr:
        return signExtended(bitOr(x, y));
    case Opcode::BitXor:
        return signExtended(bitXor(x, y));
    case Opcode::LShift:
        return signExtended(shiftLeft(x, y));
    case Opcode::RShift:
        return signExtended(shiftRight(x, y));
    case Opcode::URShift:
        return zeroExtended(shiftRightUnsigned(x, y));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::hasNumericType(ValueID id) const
{
    return isNumericOperand(m_program.at(m_function.at(id).type));
}

Operand Lowering::numericResult(Operand computed, const Type& guarantee, const Type& type, bool typeIsFact)
{
    if (!typeIsFact || !(type.primitives & Primitive::Number) || guarantee.isSubsetOf(type, m_program))
        return ensure(computed, guarantee, type);
    return convert(computed, type, repOf(type));
}

Operand Lowering::lowerNumericBinary(Opcode opcode, ValueID a, ValueID b, const Type& type)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    bool typeIsFact = hasNumericType(a) && hasNumericType(b);
    switch (opcode) {
    case Opcode::BitAnd: {
        Range range = Range::int32();
        for (const Type* operand : { &left, &right }) {
            if (operand->isInteger() && operand->integers.isSubsetOf({ 0, INT32_MAX }))
                range = { 0, std::min(range.maximum, operand->integers.maximum) };
        }
        return numericResult(lowerBitwise(opcode, a, b), Type::integer(range), type, typeIsFact);
    }
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
        return numericResult(lowerBitwise(opcode, a, b), m_program.at(m_program.types.int32), type, typeIsFact);
    case Opcode::URShift:
        return numericResult(lowerBitwise(opcode, a, b), Type::integer(Range::uint32()), type, typeIsFact);
    default:
        break;
    }

    if (left.isInteger() && right.isInteger()) {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        Int128 leftMin = left.integers.minimum;
        Int128 leftMax = left.integers.maximum;
        Int128 rightMin = right.integers.minimum;
        Int128 rightMax = right.integers.maximum;
        bool yieldsInteger = typeIsFact && type.isInteger();
        bool returnsExactOrLimit = yieldsInteger && isExact(type.integers.minimum, type.integers.maximum);
        bool returnsExact = returnsExactOrLimit && type.integers.minimum > -maxExactInteger && type.integers.maximum < maxExactInteger;
        auto withinLimits = [&](Integer sum) {
            if (type.integers.maximum == maxExactInteger)
                sum = clampToMaximum(sum, maxExactInteger);
            if (type.integers.minimum == -maxExactInteger)
                sum = clampToMinimum(sum, -maxExactInteger);
            return sum;
        };
        std::optional<Integer> exact;
        switch (opcode) {
        case Opcode::Add:
            if (isExact(leftMin + rightMin, leftMax + rightMax))
                exact = add(x, y);
            else if (returnsExactOrLimit)
                exact = isCounterStep(left) || isCounterStep(right) ? add(x, y) : withinLimits(add(x, y));
            break;
        case Opcode::Sub:
            if (isExact(leftMin - rightMax, leftMax - rightMin))
                exact = sub(x, y);
            else if (returnsExactOrLimit)
                exact = isCounterStep(right) ? sub(x, y) : withinLimits(sub(x, y));
            break;
        case Opcode::Mul:
            if (returnsExact || (leftMin >= 0 && rightMin >= 0 && isExact(0, leftMax * rightMax)))
                exact = mul(x, y);
            break;
        case Opcode::Mod: {
            auto remainder = [&]() -> Number {
                Integer whole = mod(x, y);
                if (leftMin >= 0 || yieldsInteger)
                    return convertToNumber(whole);
                return select(logicalAnd(isZero(whole), lessThan(x, integer(0))), number(-0.0), convertToNumber(whole));
            };
            if (right.integers.contains(0)) {
                return ensure(either<Number>(isNotZero(y), remainder, [&] {
                    return number(PNaN);
                }), m_program.at(m_program.types.number), type);
            }
            if (leftMin < 0 && !yieldsInteger)
                return ensure(remainder(), m_program.at(m_program.types.number), type);
            bool isMask = leftMin >= 0 && rightMin == rightMax && rightMin > 0 && hasOneBitSet(static_cast<uint64_t>(right.integers.minimum));
            exact = isMask ? bitAnd(x, integer(right.integers.minimum - 1)) : mod(x, y);
            break;
        }
        default:
            break;
        }
        if (exact)
            return convert(*exact, type, repOf(type));
    }

    Number x = asNumericOperand(a);
    Number y = asNumericOperand(b);
    Number computed;
    switch (opcode) {
    case Opcode::Add:
        computed = add(x, y);
        break;
    case Opcode::Sub:
        computed = sub(x, y);
        break;
    case Opcode::Mul:
        computed = mul(x, y);
        break;
    case Opcode::Div:
        computed = div(x, y);
        break;
    case Opcode::Mod:
        computed = remainder(a, b);
        break;
    case Opcode::Pow:
        if (auto power = powerWithConstantExponent(x, b))
            computed = *power;
        else if (auto power = powerWithConstantBase(a, b))
            computed = *power;
        else
            computed = AOT_TYPED_OPERATION(Pow, x, y);
        break;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
    return numericResult(computed, m_program.at(m_program.types.number), type, typeIsFact);
}

Word32 Lowering::nonNegativeInt32OrElse(Number number, LBasicBlock otherwise)
{
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics()) {
        PatchpointValue* patchpoint = m_out.patchpoint(Int32);
        patchpoint->append(ConstrainedValue(number.value(), ValueRep::SomeRegister));
        patchpoint->effects = Effects::none();
        patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::NonNegativeInt32);
        patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
            jit.convertDoubleToInt32UsingJavaScriptSemantics(params[1].fpr(), params[0].gpr());
            jit.m_assembler.csinv<32>(params[0].gpr(), params[0].gpr(), ARM64Registers::zr, ARM64Assembler::ConditionEQ);
        });
        Word32 converted(patchpoint);
        orElse(greaterThanOrEqual(converted, word32(0)), otherwise);
        return converted;
    }
#endif
    Word32 narrow = low32(truncateToInteger(number));
    orElse(equal(bitsOf(toNumber(narrow)), bitsOf(number)), otherwise);
    orElse(greaterThanOrEqual(narrow, word32(0)), otherwise);
    return narrow;
}

Number Lowering::fmodOnSlowPath(Number x, Number y)
{
#if CPU(ARM64)
    PatchpointValue* patchpoint = m_out.patchpoint(Double);
    patchpoint->append(ConstrainedValue(x.value(), ValueRep::reg(FPRInfo::argumentFPR0)));
    patchpoint->append(ConstrainedValue(y.value(), ValueRep::reg(FPRInfo::argumentFPR1)));
    patchpoint->resultConstraints = { ValueRep::reg(FPRInfo::returnValueFPR) };
    patchpoint->effects = Effects::none();
    patchpoint->pureFunctionOfChildrenKey = static_cast<int32_t>(PureFunctionOfChildren::Fmod);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->setGenerator([](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        Vector<Reg, 8> kept;
        RegisterSet::registersToSaveForCCall(params.usedRegisters()).forEach([&](Reg reg) {
            if (reg != Reg(FPRInfo::returnValueFPR))
                kept.append(reg);
        });
        unsigned size = WTF::roundUpToMultipleOf<stackAlignmentBytes()>(kept.size() * sizeof(Register));
        jit.pushPair(GPRInfo::callFrameRegister, CCallHelpers::linkRegister);
        if (size)
            jit.subPtr(CCallHelpers::TrustedImm32(size), CCallHelpers::stackPointerRegister);
        for (unsigned i = 0; i < kept.size(); ++i) {
            CCallHelpers::Address slot(CCallHelpers::stackPointerRegister, i * sizeof(Register));
            kept[i].isGPR() ? jit.store64(kept[i].gpr(), slot) : jit.storeDouble(kept[i].fpr(), slot);
        }
        GPRReg target = jit.scratchRegister();
        jit.loadPtr(CCallHelpers::Address(instanceGPR, Instance::offsetOfRuntimeTable()), target);
        jit.loadPtr(CCallHelpers::Address(target, static_cast<unsigned>(Entry::operationAOTTypedFmod) * sizeof(void*)), target);
        jit.call(target, OperationPtrTag);
        for (unsigned i = 0; i < kept.size(); ++i) {
            CCallHelpers::Address slot(CCallHelpers::stackPointerRegister, i * sizeof(Register));
            kept[i].isGPR() ? jit.load64(slot, kept[i].gpr()) : jit.loadDouble(slot, kept[i].fpr());
        }
        if (size)
            jit.addPtr(CCallHelpers::TrustedImm32(size), CCallHelpers::stackPointerRegister);
        jit.popPair(GPRInfo::callFrameRegister, CCallHelpers::linkRegister);
    });
    return Number(patchpoint);
#else
    return AOT_TYPED_OPERATION(Fmod, x, y);
#endif
}

Number Lowering::remainder(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    Number x = asNumericOperand(a);
    Number y = asNumericOperand(b);
    bool isConstant = right.isInteger() && right.integers.minimum == right.integers.maximum;
    if (isConstant && (right.integers.minimum == 1 || right.integers.minimum == -1)) {
        Number fraction = sub(x, trunc(x));
        return select(equal(fraction, number(0)), mul(x, number(0)), fraction);
    }
    LBasicBlock genericPath = isARM64() ? newColdBlock() : m_out.newBlock();
    Join join(m_out, Rep::Double, Double);
    auto nonNegativeOrElse = [&](ValueID id, const Type& given, Number asNumber) {
        if (!given.isInteger() || !given.integers.isSubsetOf(Range::int32()))
            return nonNegativeInt32OrElse(asNumber, genericPath);
        Word32 whole = low32(get(id).as<Integer>());
        if (given.integers.minimum < 0)
            orElse(greaterThanOrEqual(whole, word32(0)), genericPath);
        return whole;
    };
    Word32 dividend = nonNegativeOrElse(a, left, x);
    Word32 divisor = nonNegativeOrElse(b, right, y);
    if (!right.isInteger() || right.integers.contains(0))
        orElse(Word32(m_out.notZero32(divisor.value())), genericPath);
    bool isMask = isConstant && right.integers.isSubsetOf({ 1, INT32_MAX }) && hasOneBitSet(static_cast<uint64_t>(right.integers.minimum));
    auto nonNegativeRemainder = [&] {
        if (isMask)
            return bitAnd(dividend, word32(static_cast<int32_t>(right.integers.minimum - 1)));
        if (!isConstant)
            return Word32(m_out.mod(dividend.value(), divisor.value()));
        Word32 quotient(m_out.m_block->appendNew<B3::Value>(m_proc, B3::UDiv, m_out.origin(), dividend.value(), divisor.value()));
        return binary<Word32>(&Output::sub, dividend, mul(quotient, divisor));
    };
    join.add(toNumber(nonNegativeRemainder()));
    m_out.appendTo(genericPath);
    join.add(fmodOnSlowPath(x, y));
    return join.finish().as<Number>();
}

Operand Lowering::lowerUnary(ValueID result, const Value& value)
{
    ValueID a = value.child(0);
    auto& given = typeOf(a);
    auto& type = typeOf(result);
    auto& numberType = m_program.at(m_program.types.number);
    if (isNumericOperand(given)) {
        bool typeIsFact = hasNumericType(a);
        switch (value.opcode) {
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
            if (given.isNumber())
                return numericResult(get(a), given, type, typeIsFact);
            return numericResult(purified(asNumericOperand(a)), numberType, type, typeIsFact);
        case Opcode::Negate:
            if (given.isInteger() && (!given.integers.contains(0) || (typeIsFact && type.isInteger())))
                return convert(neg(get(a).as<Integer>()), type, repOf(type));
            return numericResult(neg(asNumericOperand(a)), numberType, type, typeIsFact);
        case Opcode::BitNot: {
            Integer inverted = isWithinInt32(given) ? bitXor(get(a).as<Integer>(), integer(-1)) : signExtended(bitXor(toInt32(a), word32(-1)));
            return numericResult(inverted, m_program.at(m_program.types.int32), type, typeIsFact);
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    Boxed operand = in<Boxed>(a);
    auto genericPath = [&] {
        OperationEffects effects { writesOfArithmetic(given, nullptr), true };
        Type guarantee = stubResultType(value.opcode);
        switch (value.opcode) {
        case Opcode::ToNumber:
            static_assert(static_cast<unsigned>(Stub::ToNumberAsDouble) + 1 == static_cast<unsigned>(Stub::LeafToNumberAsDouble));
            return ensure(Number(callAllocationStub(Stub::ToNumberAsDouble, { { operand.value(), GPRInfo::argumentGPR1 } }, effects, true, true, true)), guarantee, type);
        case Opcode::ToNumeric:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(ToNumeric, effects, operand), guarantee, type);
        case Opcode::Negate:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(Neg, effects, operand), guarantee, type);
        case Opcode::BitNot:
            return ensure(AOT_TYPED_OPERATION_WITH_EFFECTS(BitNot, effects, operand), guarantee, type);
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    };
    if (!arePredictedNumbers({ a }))
        return genericPath();
    Rep rep = repOf(type);
    if ((value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric) && rep == Rep::JSValue) {
        return eitherOperand(isNumber(operand), rep, [&] {
            return ensure(operand, numberType, type);
        }, genericPath);
    }
    return ifPredictedNumbers({ a }, rep, true, [&](LBasicBlock otherwise) {
        if (value.opcode != Opcode::Negate)
            return lowerUnary(result, value);
        Integer x = get(a).as<Integer>();
        if (rep == Rep::JSValue) {
            orElse(Word32(m_out.notZero32(bitAnd(low32(x), word32(INT32_MAX)).value())), otherwise);
            return ensure(signExtended(Word32(m_out.neg(low32(x).value()))), m_program.at(m_program.types.int32), type);
        }
        orElse(isNotZero(x), otherwise);
        return int32ResultOrElse(neg(x), type, otherwise);
    }, [&] {
        return lowerUnary(result, value);
    }, genericPath);
}

Operand Lowering::lowerStepByOne(InlineBuiltin which, ValueID result, const Value& value)
{
    auto& type = typeOf(result);
    ValueID a = value.arguments[0];
    bool isUp = which == InlineBuiltin::Increment;
    Rep rep = repOf(type);
    auto genericPath = [&]() -> Operand {
        Boxed operand = in<Boxed>(a);
        auto byOperation = [&] {
            return ensure(isUp ? AOT_TYPED_OPERATION(Increment, operand) : AOT_TYPED_OPERATION(Decrement, operand), stubResultType(Opcode::ToNumeric), type);
        };
        if (m_hasFailedInt32Test || rep != Rep::JSValue)
            return byOperation();
        LBasicBlock otherwise = m_out.newBlock();
        Join join(m_out, rep, b3TypeOf(rep));
        orElse(isInt32(operand), otherwise);
        join.add(ensure(signExtended(int32OrElse(isUp ? Opcode::Add : Opcode::Sub, low32(operand), word32(1), otherwise)), m_program.at(m_program.types.int32), type));
        m_out.appendTo(otherwise);
        join.add(byOperation());
        return join.finish();
    };
    if (!arePredictedNumbers({ a }))
        return genericPath();
    return ifPredictedNumbers({ a }, rep, true, [&](LBasicBlock otherwise) {
        Integer x = get(a).as<Integer>();
        if (rep == Rep::JSValue)
            return ensure(signExtended(int32OrElse(isUp ? Opcode::Add : Opcode::Sub, low32(x), word32(1), otherwise)), m_program.at(m_program.types.int32), type);
        return ensure(isUp ? add(x, integer(1)) : sub(x, integer(1)), Type::integer(Range::full()), type);
    }, [&] {
        return ensure(add(asNumericOperand(a), number(isUp ? 1 : -1)), m_program.at(m_program.types.number), type);
    }, genericPath);
}

void Lowering::branchOnRelational(const Value& value, LBasicBlock holds, LBasicBlock doesNotHold)
{
    SetForScope comparisonIndex(m_currentIndexInBlock, m_currentBlock->values.size() - 1);
    Word32 answer;
    if (auto fused = lowerFusedUse(m_currentIndexInBlock))
        answer = fused->as<Word32>();
    else {
        m_relationalSuccessors = std::pair { holds, doesNotHold };
        answer = lowerRelational(value);
    }
    if (answer.value())
        m_out.branch(answer.value(), unsure(holds), unsure(doesNotHold));
}

Word32 Lowering::lowerRelational(const Value& value)
{
    auto successors = std::exchange(m_relationalSuccessors, std::nullopt);
    if (auto answer = compareWithIntegerConstant(value))
        return *answer;
    ValueID a = value.child(0);
    ValueID b = value.child(1);
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (left.isInteger() && right.isInteger()) {
        Integer x = get(a).as<Integer>();
        Integer y = get(b).as<Integer>();
        if (left.integers.minimum >= 0 && right.integers.minimum >= 0) {
            switch (value.opcode) {
            case Opcode::Less:
                return below(x, y);
            case Opcode::Greater:
                return below(y, x);
            case Opcode::LessEq:
                return belowOrEqual(x, y);
            default:
                return belowOrEqual(y, x);
            }
        }
        switch (value.opcode) {
        case Opcode::Less:
            return lessThan(x, y);
        case Opcode::Greater:
            return greaterThan(x, y);
        case Opcode::LessEq:
            return lessThanOrEqual(x, y);
        default:
            return greaterThanOrEqual(x, y);
        }
    }
    if (auto answer = lowerRelationalWithLikelyInteger(value))
        return *answer;
    if (auto answer = lowerRelationalWithOptionalInteger(value))
        return *answer;
    if (auto answer = lowerRelationalWithIntegerBound(value))
        return *answer;
    if (isNumericOperand(left) && isNumericOperand(right)) {
        Number x = asNumericOperand(a);
        Number y = asNumericOperand(b);
        switch (value.opcode) {
        case Opcode::Less:
            return lessThan(x, y);
        case Opcode::Greater:
            return greaterThan(x, y);
        case Opcode::LessEq:
            return lessThanOrEqual(x, y);
        default:
            return greaterThanOrEqual(x, y);
        }
    }
    auto genericPath = [&] {
        Boxed x = in<Boxed>(a);
        Boxed y = in<Boxed>(b);
        OperationEffects effects { writesOfArithmetic(left, &right), true };
        switch (value.opcode) {
        case Opcode::Less:
            return emitGenericCompare(Stub::Less, x, y, effects);
        case Opcode::Greater:
            return emitGenericCompare(Stub::Greater, x, y, effects);
        case Opcode::LessEq:
            return emitGenericCompare(Stub::LessEq, x, y, effects);
        default:
            return emitGenericCompare(Stub::GreaterEq, x, y, effects);
        }
    };
    if (isCompact())
        return genericPath();
    for (unsigned i = 0; i < 2; ++i) {
        auto text = constantASCIIString(value.child(i));
        ValueID subject = value.child(1 - i);
        if (!text || text->size() != 1 || !(typeOf(subject).primitives & Primitive::String) || !holdsCells(m_function.at(subject).rep))
            continue;
        Opcode opcode = value.opcode;
        if (!i)
            opcode = opcode == Opcode::Less ? Opcode::Greater : opcode == Opcode::Greater ? Opcode::Less : opcode == Opcode::LessEq ? Opcode::GreaterEq : Opcode::LessEq;
        if (auto kept = stringCodeUnit(subject)) {
            static_assert(undefinedCodeUnit < emptyStringCodeUnit && emptyStringCodeUnit < 0);
            Integer literal = integer(static_cast<uint8_t>((*text)[0]));
            switch (opcode) {
            case Opcode::Less:
                return logicalAnd(lessThan(*kept, literal), notEqual(*kept, integer(undefinedCodeUnit)));
            case Opcode::LessEq:
                return logicalAnd(lessThanOrEqual(*kept, literal), notEqual(*kept, integer(undefinedCodeUnit)));
            case Opcode::Greater:
                return greaterThan(*kept, literal);
            default:
                return greaterThanOrEqual(*kept, literal);
            }
        }
        LBasicBlock otherwise = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        StringReceiver receiver = stringReceiver(subject);
        bool mayBeNonString = !typeOf(subject).hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
        if (mayBeNonString) {
            receiver.flags = 0;
            if (!receiver.isString && get(subject).rep != Rep::Cell)
                receiver.isString = isCell(get(subject).as<Boxed>(), typeOf(subject));
        }
        if (receiver.isString)
            orElse(receiver.isString, otherwise);
        if (mayBeNonString)
            orElse(equal(loadCellType(receiver.string), word32(StringType)), otherwise);
        Characters characters = charactersOf(receiver, otherwise);
        orElse(hasBits(characters.flags, StringImpl::flagIs8Bit()), otherwise);
        LBasicBlock isEmpty = m_out.newBlock();
        LBasicBlock hasCharacter = m_out.newBlock();
        m_out.branch(isZero(characters.length).value(), unsure(isEmpty), unsure(hasCharacter));
        m_out.appendTo(isEmpty);
        join.add(boolean(opcode == Opcode::Less || opcode == Opcode::LessEq));
        m_out.appendTo(hasCharacter);
        Word32 first(marked(m_out.load8ZeroExt32(m_out.address(m_heaps.characters8.atAnyIndex(), characters.data.value(), 0)), IsFixed));
        ensureStillAliveHere(characters.owner);
        Word32 character = word32((*text)[0]);
        Word32 isOneCharacterLong = equal(characters.length, integer(1));
        switch (opcode) {
        case Opcode::Less:
            join.add(below(first, character));
            break;
        case Opcode::GreaterEq:
            join.add(aboveOrEqual(first, character));
            break;
        case Opcode::LessEq:
            join.add(logicalOr(below(first, character), logicalAnd(equal(first, character), isOneCharacterLong)));
            break;
        default:
            join.add(logicalOr(below(character, first), logicalAnd(equal(first, character), logicalNot(isOneCharacterLong))));
            break;
        }
        m_out.appendTo(otherwise);
        join.add(genericPath());
        return join.finish().as<Word32>();
    }
    if (!arePredictedNumbers({ a, b }))
        return genericPath();
    Operand answer = ifPredictedNumbers({ a, b }, Rep::Int32, true, [&](LBasicBlock) {
        return lowerRelational(value);
    }, [&] {
        return lowerRelational(value);
    }, genericPath, true, successors);
    return successors ? Word32() : answer.as<Word32>();
}

unsigned Lowering::integerComparisonCount(ValueID id)
{
    if (m_integerComparisonCounts.isEmpty()) {
        m_integerComparisonCounts.fill(0, m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                auto& value = m_function.at(user);
                if (value.opcode != Opcode::StrictEq && value.opcode != Opcode::Eq)
                    continue;
                for (unsigned i = 0; i < 2; ++i) {
                    auto& count = m_integerComparisonCounts[value.child(i).index()];
                    Rep rep = m_function.at(value.child(i)).rep;
                    if ((rep == Rep::Double || rep == Rep::OptionalDouble) && m_function.at(value.child(1 - i)).opcode == Opcode::Integer && count < UINT8_MAX)
                        ++count;
                }
            }
        }
    }
    return m_integerComparisonCounts[id.index()];
}

ValueID Lowering::currentValue() const
{
    if (!m_currentBlock || m_currentIndexInBlock >= m_currentBlock->values.size())
        return { };
    return m_currentBlock->values[m_currentIndexInBlock];
}

bool Lowering::isUsedAsInt32(ValueID id)
{
    if (!m_didFindInt32Uses) {
        m_didFindInt32Uses = true;
        m_isUsedAsInt32.ensureSize(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                auto& value = m_function.at(user);
                switch (value.opcode) {
                case Opcode::BitAnd:
                case Opcode::BitOr:
                case Opcode::BitXor:
                case Opcode::LShift:
                case Opcode::RShift:
                case Opcode::URShift:
                    m_isUsedAsInt32.quickSet(value.child(1).index());
                    [[fallthrough]];
                case Opcode::BitNot:
                    m_isUsedAsInt32.quickSet(value.child(0).index());
                    break;
                default:
                    break;
                }
            }
        }
    }
    return m_isUsedAsInt32.quickGet(id.index());
}

Word32 Lowering::toInt32OnPath(Number value, bool isRare)
{
    if (value.value()->hasDouble())
        return word32(JSC::toInt32(value.value()->asDouble()));
#if CPU(ARM64)
    if (MacroAssembler::supportsDoubleToInt32ConversionUsingJavaScriptSemantics())
        return toInt32(value);
#endif
    if (isRare)
        return AOT_TYPED_OPERATION(ToInt32, value);
    return toInt32(value);
}

Integer Lowering::exactIntegerOnPath(Number value)
{
    if (value.value()->hasDouble()) {
        double constant = value.value()->asDouble();
        bool isExact = constant >= -9007199254740992.0 && constant <= 9007199254740992.0 && static_cast<double>(static_cast<int64_t>(constant)) == constant;
        return integer(isExact ? static_cast<int64_t>(constant) : INT64_MIN);
    }
    Integer truncated = truncateToInteger(value);
    return select(equal(convertToNumber(truncated), value), truncated, integer(INT64_MIN));
}

void Lowering::didDefineNumber(ValueID id, const Value& value, Number number)
{
    if (auto found = m_numberToExactInteger.find(number.value()); found != m_numberToExactInteger.end()) {
        m_numbersAsIntegers.add(id.index(), NumberAsInteger { number.value(), found->value });
        return;
    }
    if (value.rep == Rep::Double && value.opcode == Opcode::Cast && m_function.at(value.child(0)).opcode != Opcode::Cast && m_function.at(value.child(0)).opcode != Opcode::GetById) {
        Operand from = m_values[value.child(0).index()];
        auto minimum = minIfIntegerOrNaN(id);
        if ((from.rep == Rep::Int64 || from.rep == Rep::OptionalInt64) && minimum) {
            Integer kept(from.value);
            if (from.rep == Rep::OptionalInt64) {
                auto known = m_numbersAsIntegers.find(value.child(0).index());
                kept = known != m_numbersAsIntegers.end() && known->value.minimum && known->value.number == from.value ? Integer(known->value.integer) : select(isMarker(from), integer(INT64_MIN), kept);
            }
            m_numbersAsIntegers.add(id.index(), NumberAsInteger { number.value(), kept.value(), minimum });
            return;
        }
    }
    static constexpr unsigned worthConverting = 3;
    if (integerComparisonCount(id) < worthConverting)
        return;
    Integer truncated = truncateToInteger(number);
    Integer exact = select(equal(convertToNumber(truncated), number), truncated, integer(INT64_MIN));
    m_numbersAsIntegers.add(id.index(), NumberAsInteger { number.value(), exact.value() });
}

std::optional<Integer> Lowering::knownIntegerFor(ValueID id)
{
    auto iterator = m_numbersAsIntegers.find(id.index());
    if (iterator == m_numbersAsIntegers.end() || get(id).value != iterator->value.number)
        return std::nullopt;
    return Integer(iterator->value.integer);
}

std::optional<Word32> Lowering::areLiteralStringsEqual(ValueID a, ValueID b)
{
    auto withoutWidening = [&](ValueID id) {
        ValueID origin = withoutUpcasts(id);
        return m_function.at(origin).rep == Rep::LiteralString ? origin : id;
    };
    a = withoutWidening(a);
    b = withoutWidening(b);
    if (m_function.at(a).rep != Rep::LiteralString && m_function.at(b).rep != Rep::LiteralString)
        return std::nullopt;
    auto wordOf = [&](ValueID id) -> LiteralStringWord {
        if (!typeOf(id).hasObjects() && !(typeOf(id).primitives & ~(Primitive::LiteralString | Primitive::UndefinedOrNull)))
            return in<LiteralStringWord>(id);
        ValueID constant = m_program.constantOf(m_function, id);
        if (!constant || m_function.at(constant).opcode != Opcode::String)
            return { };
        auto number = m_program.literalStringIndex(m_function.at(constant).string());
        return number ? LiteralStringWord(m_out.constInt64(literalStringWord(*number))) : LiteralStringWord();
    };
    LiteralStringWord x = wordOf(a);
    LiteralStringWord y = x ? wordOf(b) : LiteralStringWord();
    if (!x || !y)
        return std::nullopt;
    if (isNeverStringCell(x) || isNeverStringCell(y) || !typeOf(a).hasLiteralString() || !typeOf(b).hasLiteralString())
        return Word32(m_out.equal(rawWordOf(x), rawWordOf(y)));
    LBasicBlock differ = m_out.newBlock();
    LBasicBlock areCells = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    LBasicBlock areSame = m_out.newBlock();
    m_out.branch(m_out.equal(x.value(), y.value()), unsure(areSame), unsure(differ));
    m_out.appendTo(areSame);
    join.add(boolean(true));
    m_out.appendTo(differ);
    LBasicBlock areNot = m_out.newBlock();
    m_out.branch(logicalAnd(isStringCell(x), isStringCell(y)).value(), rarely(areCells), usually(areNot));
    m_out.appendTo(areNot);
    join.add(boolean(false));
    m_out.appendTo(areCells);
    join.add(emitGenericCompare(Stub::StrictEqual, Boxed(x.value()), Boxed(y.value()), OperationEffects { Writes::RuntimeState, true }));
    return join.finish().as<Word32>();
}

template<typename Qualifies>
void Lowering::findRepeatedComparisons(const Vector<BlockID>& order, FirstRepeatedComparisonMap& firstRepeatedComparisons, unsigned minCount, const Qualifies& qualifies)
{
    struct Comparison {
        ValueID id;
        BlockID block;
    };
    UncheckedKeyHashMap<uint32_t, Vector<Comparison, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> comparisonCounts;
    for (BlockID block : order) {
        for (ValueID id : m_function.at(block).values) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::StrictEq)
                continue;
            for (unsigned i = 0; i < 2; ++i) {
                ValueID operand = value.child(i);
                if (!qualifies(block, value.child(1 - i)) || m_function.at(operand).rep != Rep::JSValue || isLazilyFetched(operand))
                    continue;
                comparisonCounts.add(operand.index(), Vector<Comparison, 4>()).iterator->value.append(Comparison { id, block });
                break;
            }
        }
    }
    constexpr unsigned maxFirstComparisons = 8;
    for (auto& comparisons : comparisonCounts.values()) {
        if (comparisons.size() < minCount)
            continue;
        Vector<Comparison, maxFirstComparisons> firsts;
        for (auto& comparison : comparisons) {
            const Comparison* first = nullptr;
            for (auto& candidate : firsts) {
                if (dominates(candidate.block, comparison.block))
                    first = &candidate;
            }
            if (first) {
                firstRepeatedComparisons.add(first->id.index(), first->id.index());
                firstRepeatedComparisons.add(comparison.id.index(), first->id.index());
            } else if (firsts.size() < maxFirstComparisons)
                firsts.append(comparison);
        }
    }
}

void Lowering::findRepeatedIntegerComparisons(const Vector<BlockID>& order)
{
    if (Options::auditAOTTypedAssertions())
        return;
    findRepeatedComparisons(order, m_firstRepeatedComparisons, 2, [&](BlockID, ValueID constant) {
        return m_function.at(constant).opcode == Opcode::Integer;
    });
}

void Lowering::findRepeatedShortStringComparisons(const Vector<BlockID>& order)
{
    if (Options::auditAOTTypedAssertions() || Options::validateAOTTypedFacts() || (!m_isCompact && m_program.isHotOutsideLoops(m_id)))
        return;
    findRepeatedComparisons(order, m_firstRepeatedShortStringComparisons, m_isCompact ? 2 : 3, [&](BlockID block, ValueID constant) {
        auto text = m_loopBlocks.get(block.index()) ? std::nullopt : constantASCIIString(constant);
        return text && text->size() <= maxShortStringLength;
    });
}

Integer Lowering::integerEqualTo(Boxed value)
{
    LBasicBlock isWhole = m_out.newBlock();
    LBasicBlock isOther = newColdBlock();
    Join join(m_out, Rep::Int64, Int64);
    m_out.branch(isInt32(value).value(), usually(isWhole), rarely(isOther));
    m_out.appendTo(isWhole);
    join.add(signExtended(low32(value)));
    m_out.appendTo(isOther);
    join.add(Integer(AOT_TYPED_OPERATION(IntegerEqualTo, value).value()));
    return join.finish().as<Integer>();
}

std::optional<Word32> Lowering::compareWithCachedInteger(ValueID a, ValueID b)
{
    if (m_firstRepeatedComparisons.isEmpty() || m_currentIndexInBlock >= m_currentBlock->values.size() || m_out.m_block != m_unconditionalBlock)
        return std::nullopt;
    ValueID comparison = m_currentBlock->values[m_currentIndexInBlock];
    auto& compared = m_function.at(comparison);
    if (compared.opcode != Opcode::StrictEq || compared.child(0) != a || compared.child(1) != b)
        return std::nullopt;
    auto first = m_firstRepeatedComparisons.find(comparison.index());
    if (first == m_firstRepeatedComparisons.end())
        return std::nullopt;
    auto [operand, constant] = m_function.at(b).opcode == Opcode::Integer ? std::pair { a, b } : std::pair { b, a };
    Operand value = get(operand);
    if (value.rep != Rep::JSValue)
        return std::nullopt;
    Integer wanted = integer(m_function.at(constant).integer());
    if (first->value == comparison.index()) {
        Integer found = integerEqualTo(value.as<Boxed>());
        m_cachedComparisonIntegers.add(comparison.index(), std::pair { value.value, found.value() });
        return equal(found, wanted);
    }
    auto found = m_cachedComparisonIntegers.find(first->value);
    if (found == m_cachedComparisonIntegers.end() || found->value.first != value.value)
        return std::nullopt;
    return equal(Integer(found->value.second), wanted);
}

std::optional<Word32> Lowering::compareWithCachedShortString(ValueID a, ValueID b)
{
    if (m_firstRepeatedShortStringComparisons.isEmpty() || m_currentIndexInBlock >= m_currentBlock->values.size() || m_out.m_block != m_unconditionalBlock)
        return std::nullopt;
    ValueID comparison = m_currentBlock->values[m_currentIndexInBlock];
    auto& compared = m_function.at(comparison);
    if (compared.opcode != Opcode::StrictEq || compared.child(0) != a || compared.child(1) != b)
        return std::nullopt;
    auto first = m_firstRepeatedShortStringComparisons.find(comparison.index());
    if (first == m_firstRepeatedShortStringComparisons.end())
        return std::nullopt;
    auto [operand, constant] = stringConstantOf(b) ? std::pair { a, b } : std::pair { b, a };
    Operand value = get(operand);
    if (value.rep != Rep::JSValue)
        return std::nullopt;
    Integer wanted = integer(shortStringWord(*constantASCIIString(constant)));
    if (first->value == comparison.index()) {
        LValue found = callAllocationStub(Stub::ShortStringOfValue, { { value.value, stubTemporaryGPRs[2] } }, { Writes::Pure, false }, false);
        m_cachedShortStrings.add(comparison.index(), std::pair { value.value, found });
        return equal(Integer(found), wanted);
    }
    auto found = m_cachedShortStrings.find(first->value);
    if (found == m_cachedShortStrings.end() || found->value.first != value.value)
        return std::nullopt;
    return equal(Integer(found->value.second), wanted);
}

Word32 Lowering::lowerStrictEq(ValueID a, ValueID b)
{
    for (auto [read, constant] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!encodesUndefinedAsZero(read))
            continue;
        Boxed encoded(m_values[read.index()].value);
        switch (m_function.at(constant).opcode) {
        case Opcode::Undefined:
            return Word32(m_out.isZero64(encoded.value()));
        case Opcode::Null:
            return haveEqualBits(encoded, boxedConstant(jsNull()));
        case Opcode::Boolean:
            return haveEqualBits(encoded, boxedConstant(jsBoolean(m_function.at(constant).boolean())));
        default:
            break;
        }
    }
    if (!m_pinnedReads.isEmpty()) {
        for (auto [read, text] : { std::pair { a, b }, std::pair { b, a } }) {
            if (auto found = m_pinnedReads.find(read.index()); found != m_pinnedReads.end())
                return isPinnedTo(found->value, text);
        }
    }
    if (!m_elementsReadAsNumbers.isEmpty()) {
        for (auto [element, number] : { std::pair { a, b }, std::pair { b, a } }) {
            if (auto found = m_elementsReadAsNumbers.find(element.index()); found != m_elementsReadAsNumbers.end())
                return equal(Number(found->value), in<Number>(number));
        }
    }
    if (auto areEqual = areTrackedCodeUnitsEqual(a, b))
        return *areEqual;
    if (auto areEqual = areLiteralStringsEqual(a, b))
        return *areEqual;
    if (typeOf(a).hasLiteralString() || typeOf(b).hasLiteralString()) {
        size_t numberOfAssumptions = m_assumptions.size();
        widenLiteralString(a);
        widenLiteralString(b);
        Word32 areEqual = lowerStrictEq(a, b);
        m_assumptions.shrink(numberOfAssumptions);
        return areEqual;
    }
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (left.isInteger() && right.isInteger())
        return equal(get(a).as<Integer>(), get(b).as<Integer>());
    if (left.isBoolean() && right.isBoolean())
        return equal(get(a).as<Word32>(), get(b).as<Word32>());
    for (auto [number, constant] : { std::pair { a, b }, std::pair { b, a } }) {
        if (m_function.at(constant).opcode != Opcode::Integer)
            continue;
        if (auto asInteger = knownIntegerFor(number))
            return equal(*asInteger, integer(m_function.at(constant).integer()));
    }
    for (auto [number, other] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!typeOf(other).isInteger())
            continue;
        if (auto asInteger = knownIntegerFor(number))
            return equal(*asInteger, get(other).as<Integer>());
    }
    if (auto isEqual = compareWithCachedInteger(a, b))
        return *isEqual;
    if (auto isEqual = compareWithCachedShortString(a, b))
        return *isEqual;
    auto kept = [&](ValueID id) -> const NumberAsInteger* {
        auto found = m_numbersAsIntegers.find(id.index());
        return found != m_numbersAsIntegers.end() && get(id).value == found->value.number ? &found->value : nullptr;
    };
    if (auto* x = kept(a), *y = kept(b); x && y && left.isNumber() && right.isNumber()) {
        auto* integerOrNaN = x->minimum ? x : y->minimum ? y : nullptr;
        LBasicBlock isSame = newUnlikelyBlock();
        LBasicBlock isOther = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(equal(Integer(x->integer), Integer(y->integer)).value(), unsure(isSame), unsure(isOther));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        m_out.appendTo(isSame);
        if (integerOrNaN)
            join.add(*integerOrNaN->minimum >= 0 ? greaterThanOrEqual(Integer(integerOrNaN->integer), integer(0)) : notEqual(Integer(integerOrNaN->integer), integer(INT64_MIN)));
        else {
            join.add(either<Word32>(notEqual(Integer(x->integer), integer(INT64_MIN)), [&] {
                return boolean(true);
            }, [&] {
                return equal(in<Number>(a), in<Number>(b));
            }));
        }
        return join.finish().as<Word32>();
    }
    if (left.isNumber() && right.isNumber())
        return equal(in<Number>(a), in<Number>(b));
    uint16_t either = left.primitives | right.primitives;
    uint16_t both = left.primitives & right.primitives;
    if (isNumericOperand(left) && isNumericOperand(right) && either & Primitive::Number) {
        if (!(either & Primitive::Double))
            return equal(in<OptionalInteger>(a), in<OptionalInteger>(b));
        OptionalNumber x = in<OptionalNumber>(a);
        OptionalNumber y = in<OptionalNumber>(b);
        if (!(either & Primitive::Null) && (left.isUndefinedOrNull() || right.isUndefinedOrNull()))
            return isMarker(left.isUndefinedOrNull() ? y : x);
        Word32 hasSameBits = equal(bitsOf(x), bitsOf(y));
        if (!(both & Primitive::Number))
            return hasSameBits;
        Word32 isEqual = equal(assumedNumber(x), assumedNumber(y));
        if (!(both & Primitive::UndefinedOrNull))
            return isEqual;
        return logicalOr(isEqual, logicalAnd(hasSameBits, isMarker(x)));
    }
    if (!(both & (Primitive::Number | Primitive::String | Primitive::BigInt)))
        return haveEqualBits(in<Boxed>(a), in<Boxed>(b));
    if (isCompact()) {
        Boxed first = in<Boxed>(a);
        return emitGenericCompare(Stub::StrictEqual, first, in<Boxed>(b), OperationEffects { Writes::RuntimeState, true });
    }
    if (isStringOrNullish(left) && isStringOrNullish(right))
        return areStringsEqual(a, b);
    for (auto [string, other] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!typeOf(string).isString())
            continue;
        if (auto text = constantASCIIString(string); text && !text->empty() && text->size() <= maxInlineStringCompareLength && !m_isInLoop && holdsCells(get(other).rep)) {
            LBasicBlock isText = m_out.newBlock();
            LBasicBlock isOther = m_out.newBlock();
            Join join(m_out, Rep::Int32, Int32);
            branchOnLiteralByOneCall(other, string, *text, isText, isOther);
            m_out.appendTo(isText);
            join.add(boolean(true));
            m_out.appendTo(isOther);
            join.add(boolean(false));
            return join.finish().as<Word32>();
        }
        return ifPredictionHolds({ { other, &m_program.at(m_program.types.string) } }, Rep::Int32, [&](LBasicBlock) {
            return lowerStrictEq(a, b);
        }, [&] {
            return boolean(false);
        }).as<Word32>();
    }
    for (auto [integer, other] : { std::pair { a, b }, std::pair { b, a } }) {
        if (!typeOf(integer).isInteger() || !typeOf(integer).integers.isSubsetOf(Range::int32()) || get(other).rep != Rep::JSValue)
            continue;
        Integer wanted = get(integer).as<Integer>();
        Boxed value = in<Boxed>(other);
        LBasicBlock hasSameBits = newUnlikelyBlock();
        LBasicBlock hasOtherBits = m_out.newBlock();
        LBasicBlock isOtherInt32 = m_out.newBlock();
        LBasicBlock notInt32 = newColdBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(equal(value, boxInt32(low32(wanted))).value(), unsure(hasSameBits), unsure(hasOtherBits));
        m_out.appendTo(hasSameBits);
        join.add(boolean(true));
        m_out.appendTo(hasOtherBits);
        m_out.branch(isInt32(value).value(), usually(isOtherInt32), rarely(notInt32));
        m_out.appendTo(isOtherInt32);
        join.add(boolean(false));
        m_out.appendTo(notInt32);
        auto& range = typeOf(integer).integers;
        if (range.minimum == range.maximum && range.minimum) {
            join.add(equal(value, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(range.minimum)))));
            return join.finish().as<Word32>();
        }
        LBasicBlock isDouble = newColdBlock();
        LBasicBlock notNumber = newColdBlock();
        m_out.branch(isNumber(value).value(), unsure(isDouble), unsure(notNumber));
        m_out.appendTo(isDouble);
        join.add(equal(unboxedDouble(value), convertToNumber(wanted)));
        m_out.appendTo(notNumber);
        join.add(boolean(false));
        return join.finish().as<Word32>();
    }
    Boxed x = in<Boxed>(a);
    Boxed y = in<Boxed>(b);
    auto genericPath = [&] {
        return emitGenericCompare(Stub::StrictEqual, x, y, OperationEffects { Writes::RuntimeState, true });
    };
    if (prefersGenericPathInColdCode())
        return genericPath();
    if (a == b) {
        LBasicBlock isSomeNumber = m_out.newBlock();
        LBasicBlock isDouble = newUnlikelyBlock();
        LBasicBlock isSelf = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        m_out.branch(isNumber(x).value(), unsure(isSomeNumber), unsure(isSelf));
        m_out.appendTo(isSomeNumber);
        m_out.branch(isInt32(x).value(), usually(isSelf), rarely(isDouble));
        m_out.appendTo(isDouble);
        Number number = unboxedDouble(x);
        join.add(equal(number, number));
        m_out.appendTo(isSelf);
        join.add(boolean(true));
        return join.finish().as<Word32>();
    }
    auto inlinePath = [&](LBasicBlock) {
        return lowerStrictEq(a, b);
    };
    if (arePredictedNumbers({ a, b })) {
        return ifPredictedNumbers({ a, b }, Rep::Int32, true, inlinePath, [&] {
            return lowerStrictEq(a, b);
        }, genericPath, true).as<Word32>();
    }
    if (predictedTypeOf(a).isString() && predictedTypeOf(b).isString()) {
        auto& stringType = m_program.at(m_program.types.string);
        return ifPredictionHolds({ { a, &stringType }, { b, &stringType } }, Rep::Int32, inlinePath, genericPath).as<Word32>();
    }
    for (ValueID id : { a, b }) {
        auto prediction = predictionOf(id);
        if (!prediction || !(prediction->isObject() || prediction->isBoolean()))
            continue;
        auto& kind = m_program.at(prediction->isObject() ? m_program.types.object : m_program.types.boolean);
        return ifPredictionHolds({ { id, &kind } }, Rep::Int32, inlinePath, genericPath).as<Word32>();
    }
    LBasicBlock hasSameBits = newUnlikelyBlock();
    LBasicBlock isSameNumber = m_out.newBlock();
    LBasicBlock hasOtherBits = m_out.newBlock();
    LBasicBlock areCells = m_out.newBlock();
    LBasicBlock isSame = m_out.newBlock();
    LBasicBlock isOther = m_out.newBlock();
    LBasicBlock callsStub = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    m_out.branch(equal(x, y).value(), unsure(hasSameBits), unsure(hasOtherBits));
    m_out.appendTo(hasSameBits);
    m_out.branch(isNumber(x).value(), unsure(isSameNumber), unsure(isSame));
    m_out.appendTo(isSameNumber);
    m_out.branch(isInt32(x).value(), usually(isSame), rarely(callsStub));
    m_out.appendTo(hasOtherBits);
    m_out.branch(isCell(Boxed(m_out.bitOr(x.value(), y.value()))).value(), usually(areCells), rarely(callsStub));
    m_out.appendTo(areCells);
    m_out.branch(aboveOrEqual(loadCellType(asCell(x)), word32(ObjectType)).value(), usually(isOther), rarely(callsStub));
    m_out.appendTo(isSame);
    join.add(boolean(true));
    m_out.appendTo(isOther);
    join.add(boolean(false));
    m_out.appendTo(callsStub);
    join.add(genericPath());
    return join.finish().as<Word32>();
}

std::optional<uint32_t> Lowering::nonNumericStringConstant(ValueID id) const
{
    auto text = constantASCIIString(id);
    if (!text || text->empty() || !std::isnan(jsToNumber(StringView(byteCast<Latin1Character>(*text)))))
        return std::nullopt;
    return m_function.at(stringConstantOf(id)).string().index();
}

Word32 Lowering::lowerLooseEq(ValueID a, ValueID b)
{
    auto& left = typeOf(a);
    auto& right = typeOf(b);
    if (a == b || (left.isNumber() && right.isNumber()) || (left.isString() && right.isString()) || (left.isBoolean() && right.isBoolean()) || (left.isObject() && right.isObject()))
        return lowerStrictEq(a, b);
    auto genericPath = [&] {
        for (auto [text, other] : { std::pair { a, b }, std::pair { b, a } }) {
            if (auto number = nonNumericStringConstant(text))
                return Word32(callICStub(Stub::LooseEqualsLiteral, Int32, { { in<Boxed>(other).value(), firstStubOperandGPR } }, std::pair { GPRInfo::argumentGPR1, *number }, OperationEffects { writesOfArithmetic(left, &right), true }));
        }
        Boxed x = in<Boxed>(a);
        return emitGenericCompare(Stub::LooseEqual, x, in<Boxed>(b), OperationEffects { writesOfArithmetic(left, &right), true });
    };
    auto inlinePath = [&](LBasicBlock) {
        return lowerStrictEq(a, b);
    };
    if (arePredictedNumbers({ a, b }) && predictedTypeOf(a).isNumber() && predictedTypeOf(b).isNumber()) {
        return ifPredictedNumbers({ a, b }, Rep::Int32, true, inlinePath, [&] {
            return lowerStrictEq(a, b);
        }, genericPath, true).as<Word32>();
    }
    if (!predictionOf(a) && !predictionOf(b))
        return genericPath();
    Type first = predictedTypeOf(a);
    Type second = predictedTypeOf(b);
    for (TypeID kind : { m_program.types.string, m_program.types.boolean, m_program.types.object }) {
        auto& type = m_program.at(kind);
        if (first.isSubsetOf(type, m_program) && second.isSubsetOf(type, m_program))
            return ifPredictionHolds({ { a, &type }, { b, &type } }, Rep::Int32, inlinePath, genericPath).as<Word32>();
    }
    return genericPath();
}

void Lowering::branchOnTruthinessWithStub(Boxed value, FTL::WeightedTarget isTruthy, FTL::WeightedTarget isFalsy)
{
    m_context.emitsCalls = true;
    PatchpointValue* patchpoint = m_out.patchpoint(Void);
    applyEffects(patchpoint, { Writes::RuntimeState, false });
    patchpoint->effects.terminal = true;
    patchpoint->append(ConstrainedValue(value.value(), ValueRep::reg(firstStubOperandGPR)));
    RegisterSet changed = *registersClobberedBy(Stub::ValueIsTruthy);
    changed.add(GPRInfo::returnValueGPR, IgnoreVectors);
    changed.add(firstStubOperandGPR, IgnoreVectors);
    patchpoint->clobber(RegisterSet::macroClobberedGPRs());
    patchpoint->clobberLate(changed);
    m_out.appendSuccessor(isFalsy);
    m_out.appendSuccessor(isTruthy);
    patchpoint->setGenerator([context = &m_context](CCallHelpers& jit, const StackmapGenerationParams& params) {
        AllowMacroScratchRegisterUsage allowScratch(jit);
        context->stubCalls.call(jit, Stub::ValueIsTruthy, CallSite { });
        bool leavesIfTruthy = params.fallsThroughToSuccessor(0);
#if CPU(X86_64)
        CCallHelpers::Jump leaves = CCallHelpers::Jump(jit.m_assembler.jCC(leavesIfTruthy ? X86Assembler::ConditionNE : X86Assembler::ConditionE));
#else
        CCallHelpers::Jump leaves = jit.branchOnFlags(leavesIfTruthy ? CCallHelpers::NotEqual : CCallHelpers::Equal);
#endif
        CCallHelpers::Jump isTruthy;
        if (!leavesIfTruthy && !params.fallsThroughToSuccessor(1))
            isTruthy = jit.jump();
        Vector<Box<CCallHelpers::Label>> labels = params.successorLabels();
        params.addLatePath([=](CCallHelpers& jit) {
            leaves.linkTo(*labels[leavesIfTruthy], &jit);
            if (isTruthy.isSet())
                isTruthy.linkTo(*labels[1], &jit);
        });
    });
}

Word32 Lowering::genericToBoolean(ValueID id)
{
    auto successors = std::exchange(m_truthinessSuccessors, std::nullopt);
    Boxed value = in<Boxed>(id);
    auto callStubPath = [&] {
        if (successors) {
            branchOnTruthinessWithStub(value, successors->first, successors->second);
            return Word32();
        }
        return Word32(callICStub(Stub::ToBoolean, Int32, { { value.value(), firstStubOperandGPR } }, std::nullopt, { Writes::RuntimeState, false }));
    };
    if (prefersGenericPathInColdCode())
        return callStubPath();
    if (isCompact())
        return callStubPath();
    auto expected = predictionOf(id, 0);
    if (!expected) {
        auto& type = typeOf(id);
        Type usual = Type::fromPrimitives(type.primitives & (Primitive::UndefinedOrNull | Primitive::Boolean));
        usual.anyObject = type.hasObjects();
        if (usual.isNever())
            return callStubPath();
        expected = usual;
    }

    LBasicBlock otherwise = newColdBlock();
    Join join(m_out, Rep::Int32, Int32);
    auto when = [&](Word32 condition, const auto& answer) {
        LBasicBlock holds = m_out.newBlock();
        LBasicBlock next = m_out.newBlock();
        m_out.branch(condition.value(), unsure(holds), unsure(next));
        m_out.appendTo(holds);
        join.add(answer());
        m_out.appendTo(next);
    };
    if (expected->hasCells()) {
        LBasicBlock isCellCase = m_out.newBlock();
        LBasicBlock isOtherCase = m_out.newBlock();
        m_out.branch(isCell(value).value(), unsure(isCellCase), unsure(isOtherCase));
        m_out.appendTo(isCellCase);
        CellPointer cell = asCell(value);
        Word32 cellType = loadCellType(cell);
        if (expected->hasObjects()) {
            Word32 isTruthyObject = aboveOrEqual(cellType, word32(ObjectType));
            if (m_program.mayEncounterMasqueradesAsUndefined)
                isTruthyObject = logicalAnd(isTruthyObject, hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), MasqueradesAsUndefined));
            when(isTruthyObject, [&] {
                if (!m_program.mayEncounterMasqueradesAsUndefined && Options::validateAOTTypedFacts()) [[unlikely]] {
                    LBasicBlock masquerades = newColdBlock();
                    LBasicBlock continuation = m_out.newBlock();
                    m_out.branch(hasNoBits(loadByte(cell, m_heaps.JSCell_typeInfoFlags), MasqueradesAsUndefined).value(), usually(continuation), rarely(masquerades));
                    m_out.appendTo(masquerades);
                    AOT_TYPED_OPERATION(FactViolated, value, word32(m_id.index()), word32(m_currentBlock->values[m_currentIndexInBlock].index()));
                    m_out.jump(continuation);
                    m_out.appendTo(continuation);
                }
                return boolean(true);
            });
        }
        if (expected->primitives & Primitive::String) {
            when(equal(cellType, word32(StringType)), [&] {
                return notEqual(cell, CellPointer(emptyString()));
            });
        }
        m_out.jump(otherwise);
        m_out.appendTo(isOtherCase);
    }
    if (expected->primitives & (Primitive::UndefinedOrNull | Primitive::Boolean)) {
        static_assert(JSValue::ValueNull < JSValue::ValueUndefined && JSValue::ValueFalse < JSValue::ValueUndefined && JSValue::ValueTrue < JSValue::ValueUndefined && JSValue::ValueUndefined < JSValue::DoubleEncodeOffset);
        when(equal(value, boxedConstant(jsBoolean(true))), [&] {
            return boolean(true);
        });
        when(belowOrEqual(value, undefined()), [&] {
            return boolean(false);
        });
    }
    if (expected->primitives & Primitive::Number) {
        when(isInt32(value), [&] {
            return notEqual(low32(value), word32(0));
        });
        when(isNumber(value), [&] {
            return isNeitherZeroNorNaN(unboxedDouble(value));
        });
    }
    m_out.jump(otherwise);
    m_out.appendTo(otherwise);
    if (Word32 answer = callStubPath(); answer.value())
        join.add(answer);
    return join.finish().as<Word32>();
}

CellPointer Lowering::toStringCell(ValueID id)
{
    auto& type = typeOf(id);
    Operand operand = get(id);
    if (type.isString())
        return operand.as<CellPointer>();
    bool createsStringGenerically = prefersGenericPathInColdCode();
    if (operand.rep == Rep::Int64 && createsStringGenerically)
        return AOT_TYPED_OPERATION(DoubleToString, convertToNumber(operand.as<Integer>()));
    if (operand.rep == Rep::Double && createsStringGenerically)
        return AOT_TYPED_OPERATION(DoubleToString, operand.as<Number>());
    if (operand.rep == Rep::Int64)
        return integerToString(operand.as<Integer>(), type);
    if (operand.rep == Rep::Double) {
        LBasicBlock isOther = newColdBlock();
        Join join(m_out, Rep::Cell, Int64);
        auto& int32Type = m_program.at(m_program.types.int32);
        if (auto exact = knownIntegerFor(id)) {
            orElse(isSubsetOf(*exact, Range::int32()), isOther);
            join.add(integerToString(*exact, int32Type));
        } else
            join.add(integerToString(integralOrElse(operand.as<Number>(), Range::int32(), isOther), int32Type));
        m_out.appendTo(isOther);
        join.add(AOT_TYPED_OPERATION(DoubleToString, operand.as<Number>()));
        return join.finish().as<CellPointer>();
    }
    Boxed boxed = convertTo<Boxed>(operand, type);
    auto genericPath = [&] {
        return AOT_TYPED_OPERATION_WITH_EFFECTS(ToString, (OperationEffects { type.hasObjects() ? Writes::Heap : Writes::RuntimeState, true }), boxed);
    };
    if (type.primitives & Primitive::String && !(type.primitives & Primitive::Absent) && !isStringOrNullish(type) && !predictionOf(id)) {
        bool everyCellIsString = !type.hasObjects() && !(type.primitives & (Primitive::Symbol | Primitive::BigInt));
        bool mayBeNumberByType = !type.hasObjects() && type.primitives & Primitive::Number;
        LBasicBlock isAnyCell = m_out.newBlock();
        LBasicBlock isString = m_out.newBlock();
        LBasicBlock isNotCell = m_out.newBlock();
        LBasicBlock isAnythingElse = m_out.newBlock();
        Join join(m_out, Rep::Cell, Int64);
        m_out.branch(isCell(boxed).value(), unsure(isAnyCell), unsure(isNotCell));
        m_out.appendTo(isAnyCell);
        if (everyCellIsString)
            m_out.jump(isString);
        else
            m_out.branch(equal(loadCellType(asCell(boxed)), word32(StringType)).value(), unsure(isString), unsure(isAnythingElse));
        m_out.appendTo(isString);
        join.add(asCell(boxed));
        m_out.appendTo(isNotCell);
        if (mayBeNumberByType) {
            LBasicBlock isWhole = m_out.newBlock();
            m_out.branch(isInt32(boxed).value(), unsure(isWhole), unsure(isAnythingElse));
            m_out.appendTo(isWhole);
            join.add(integerToString(signExtended(low32(boxed)), m_program.at(m_program.types.int32)));
        } else
            m_out.jump(isAnythingElse);
        m_out.appendTo(isAnythingElse);
        join.add(genericPath());
        return join.finish().as<CellPointer>();
    }
    if (isStringOrNullish(type)) {
        constexpr bool mayThrow = OperationSignature<std::remove_pointer_t<decltype(operationAOTTypedToString)>>::mayThrow;
        if (!m_decoded.contains(boxed.value()) && canCallOnLatePath(Entry::operationAOTTypedToString, mayThrow, { boxed.value() })) {
            return CellPointer(callOnLatePath(Entry::operationAOTTypedToString, mayThrow, OperationEffects { Writes::RuntimeState, true }, { boxed.value() }, { }, createSharedTask<CCallHelpers::JumpList(CCallHelpers&, GPRReg, std::span<const GPRReg>)>([](CCallHelpers& jit, GPRReg result, std::span<const GPRReg>) {
#if CPU(ARM64)
                return CCallHelpers::JumpList(jit.branchTest64(CCallHelpers::NonZero, result, CCallHelpers::TrustedImm64(JSValue::OtherTag)));
#else
                return CCallHelpers::JumpList(jit.branchIfNotCell(result));
#endif
            }), boxed.value()));
        }
        return either<CellPointer>(isCell(boxed), [&] {
            return asCell(boxed);
        }, genericPath);
    }
    if (createsStringGenerically)
        return genericPath();
    auto prediction = predictionOf(id);
    bool isNumberOrNullish = type.primitives & Primitive::Number && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull);
    if (!isNumberOrNullish && (!prediction || !(prediction->isString() || prediction->isNumber())))
        return genericPath();
    auto& expected = m_program.at(!isNumberOrNullish && prediction->isString() ? m_program.types.string : m_program.types.int32);
    return ifPredictionHolds({ { id, &expected } }, Rep::Cell, [&](LBasicBlock) {
        return toStringCell(id);
    }, genericPath).as<CellPointer>();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
