/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#if ENABLE(AOT)

#include "AOTMachineTesting.h"
#include "AllowMacroScratchRegisterUsage.h"
#include "JITOperationList.h"
#include "JITOperationValidation.h"
#include <array>
#include <bit>
#include <cmath>
#include <type_traits>
#include <wtf/DataLog.h>
#include <wtf/Int128.h>
#include <wtf/WeakRandom.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Machine {

struct RandomProgram {
    static constexpr unsigned maxVariables = 64;
    static constexpr unsigned numberOfMemoryWords = 16;
    static constexpr unsigned numberOfOutgoingWords = 8;
    static constexpr unsigned maxCases = 9;
    static constexpr unsigned maxOperands = 7;
    static constexpr unsigned maxResults = 3;
    static constexpr uint32_t none = UINT32_MAX;

    enum class Kind : uint8_t { Compute, Copy, Swap, Load, Store, Patch, Check, Split, If, Loop, Break, Continue, Switch, Try, Return };
    enum class PatchEffects : uint8_t { Call, WritesLocalState, ReadsEverything, Nothing };

    enum Flag : uint16_t {
        UsesImmediate = 1 << 0,
        ComparesWithImmediate = 1 << 1,
        TestsBits = 1 << 2,
        IsChill = 1 << 3,
        IsBottomTested = 1 << 4,
        KeepsScope = 1 << 5,
        ReturnsEverything = 1 << 6,
        ScalesSecondOperand = 1 << 7,
        CombinesOperands = 1 << 8,
        CombinesOnLeft = 1 << 9,
    };

    struct Statement {
        Kind kind { Kind::Split };
        Opcode opcode { Nop };
        Opcode relation { Equal };
        Opcode secondRelation { Equal };
        uint16_t flags { 0 };
        uint8_t destination { 0 };
        std::array<uint8_t, 2> operands { 0, 0 };
        std::array<uint8_t, 2> compared { 0, 0 };
        std::array<EdgeClass, 2> classes { EdgeClass::Normal, EdgeClass::Normal };
        uint32_t next { none };
        std::array<uint32_t, 2> children { none, none };
        uint32_t record { 0 };
        uint32_t count { 0 };
        uint64_t immediate { 0 };
        uint64_t comparedImmediate { 0 };
        uint64_t assigned { 0 };
    };

    struct PatchValue {
        TypeKind type { Int64 };
        ValueRep::Kind constraint { ValueRep::WarmAny };
        uint8_t variable { 0 };
        bool isConstant { false };
        bool goesThroughEmitRestore { false };
        uint32_t place { 0 };
        uint64_t bits { 0 };
    };

    struct PatchRecord {
        uint32_t number { 0 };
        uint32_t firstOperand { 0 };
        uint32_t numberOfOperands { 0 };
        uint32_t firstResult { 0 };
        uint32_t numberOfResults { 0 };
        uint64_t earlyClobbers { 0 };
        uint64_t lateClobbers { 0 };
        uint8_t numberOfGPScratchRegisters { 0 };
        uint8_t numberOfFPScratchRegisters { 0 };
        PatchEffects effects { PatchEffects::Call };
        bool isCall { false };
        bool mayThrow { false };
        bool runsWithoutFrame { false };
    };

    struct Case {
        int64_t value { 0 };
        uint32_t list { none };
        EdgeClass edgeClass { EdgeClass::Normal };
    };

    static constexpr uint64_t bitOf(unsigned variable) { return uint64_t { 1 } << variable; }

    uint64_t assignedIn(uint32_t list) const
    {
        uint64_t result = 0;
        for (; list != none; list = statements[list].next)
            result |= statements[list].assigned;
        return result;
    }

    std::span<const PatchValue> operandsOf(const PatchRecord& record) const { return patchValues.span().subspan(record.firstOperand, record.numberOfOperands); }
    std::span<const PatchValue> resultsOf(const PatchRecord& record) const { return patchValues.span().subspan(record.firstResult, record.numberOfResults); }

    Arena arena;
    uint64_t seed { 0 };
    unsigned numberOfVariables { 0 };
    bool usesMemory { false };
    uint32_t body { none };
    std::array<TypeKind, maxVariables> types { };
    std::array<bool, maxVariables> isWitness { };
    std::array<uint64_t, maxVariables> witnessBits { };
    ArenaVector<Statement> statements;
    ArenaVector<PatchValue> patchValues;
    ArenaVector<PatchRecord> patches;
    ArenaVector<Case> cases;
};

static constexpr uint64_t trashOfRandomProcedure = 0x5bad5bad5bad5badull;
static constexpr int64_t answerOfRandomProcedureWithWrongOperand = 0x0bad0bad0bad0badll;
static constexpr uint64_t initialHashOfRandomProgram = 0x243f6a8885a308d3ull;
static constexpr unsigned numberOfRandomTypeKinds = Tuple + 1;

static constexpr uint64_t saltOfRandomPatch(uint32_t number) { return (static_cast<uint64_t>(number) + 1) * 0x9e3779b97f4a7c15ull; }
static constexpr uint64_t saltOfRandomResult(unsigned index) { return (static_cast<uint64_t>(index) + 1) * 0xc2b2ae3d27d4eb4full; }

static void checkRandomProcedure(bool condition, uint64_t seed, ASCIILiteral rule)
{
    if (condition) [[likely]]
        return;
    dataLogLn("FAIL: the random procedure with seed ", seed, ": ", rule);
    CRASH();
}

static void JIT_OPERATION_ATTRIBUTES reportWrongOperandOfRandomPatch(uint64_t seed, uint32_t patch, uint32_t operand, uint64_t found, uint64_t expected)
{
    dataLogLn("FAIL: the random procedure with seed ", seed, ": operand ", operand, " of Patch ", patch, " holds the bits ", found, " where ", expected, " are expected");
}

static void* entryOfRandomPatchReport()
{
#if ENABLE(JIT_OPERATION_VALIDATION)
    JITOperationAnnotation annotation { std::bit_cast<void*>(&reportWrongOperandOfRandomPatch), std::bit_cast<void*>(&reportWrongOperandOfRandomPatch) };
    JITOperationList::populatePointersInEmbedder(&annotation, &annotation + 1);
#endif
    return tagCFunctionPtr<void*, OperationPtrTag>(&reportWrongOperandOfRandomPatch);
}

static unsigned bankIndexOfRandomValue(TypeKind type) { return static_cast<unsigned>(bankOf(Type(type))); }
static bool isLateRandomConstraint(ValueRep::Kind kind) { return kind == ValueRep::LateColdAny || kind == ValueRep::SomeLateRegister; }
static bool isRandomShift(Opcode opcode) { return opcode == Shl || opcode == SShr || opcode == ZShr; }
static bool isRandomDivision(Opcode opcode) { return opcode == Div || opcode == UDiv || opcode == Mod || opcode == UMod; }

template<typename Functor>
static void forEachRandomVariable(uint64_t mask, const Functor& functor)
{
    for (; mask; mask &= mask - 1)
        functor(static_cast<unsigned>(std::countr_zero(mask)));
}

template<typename T, bool = std::is_integral_v<T>>
struct RandomWrappingType {
    using Type = std::make_unsigned_t<T>;
};

template<typename T>
struct RandomWrappingType<T, false> {
    using Type = T;
};

template<typename T>
static uint64_t bitsOfRandomValue(T value)
{
    if constexpr (std::is_same_v<T, double>)
        return std::bit_cast<uint64_t>(value);
    else if constexpr (std::is_same_v<T, float>)
        return std::bit_cast<uint32_t>(value);
    else
        return static_cast<std::make_unsigned_t<T>>(value);
}

static uint64_t canonicalRandomBits(TypeKind type, uint64_t bits)
{
    switch (type) {
    case Int32:
    case Float:
        return static_cast<uint32_t>(bits);
    case Int64:
    case Double:
        return bits;
    case Vector128:
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t bitsInPlaceOfRandomNaN(TypeKind type)
{
    return type == Double ? bitsOfRandomValue(1.5) : bitsOfRandomValue(1.5f);
}

static uint64_t withoutRandomNaN(TypeKind type, uint64_t bits)
{
    bool isNaN = (type == Double && std::isnan(std::bit_cast<double>(bits))) || (type == Float && std::isnan(std::bit_cast<float>(static_cast<uint32_t>(bits))));
    return isNaN ? bitsInPlaceOfRandomNaN(type) : bits;
}

static uint64_t bitsOfRandomResult(TypeKind type, uint64_t sum)
{
    double number = static_cast<double>(static_cast<int64_t>(sum));
    switch (type) {
    case Int32:
        return static_cast<uint32_t>(sum);
    case Int64:
        return sum;
    case Float:
        return bitsOfRandomValue(static_cast<float>(number));
    case Double:
        return bitsOfRandomValue(number);
    case Vector128:
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename T>
static uint64_t evaluateRandomOperationOn(Opcode opcode, T left, T right)
{
    using Wrapping = typename RandomWrappingType<T>::Type;
    constexpr bool isInteger = std::is_integral_v<T>;
    constexpr unsigned amountMask = sizeof(T) * 8 - 1;
    Wrapping a = static_cast<Wrapping>(left);
    Wrapping b = static_cast<Wrapping>(right);
    auto answer = [](auto value) {
        return bitsOfRandomValue(static_cast<T>(value));
    };

    switch (opcode) {
    case Opaque:
    case BitwiseCast:
    case ZExt32:
        return answer(left);
    case Add:
        return answer(a + b);
    case Sub:
        return answer(a - b);
    case Mul:
        return answer(a * b);
    case Div:
        return answer(left / right);
    case UDiv:
        if constexpr (isInteger)
            return answer(a / b);
        break;
    case Mod:
        if constexpr (isInteger)
            return answer(left % right);
        break;
    case UMod:
        if constexpr (isInteger)
            return answer(a % b);
        break;
    case MulHigh:
        if constexpr (std::is_same_v<T, int64_t>)
            return static_cast<uint64_t>((static_cast<Int128>(left) * static_cast<Int128>(right)) >> 64);
        break;
    case UMulHigh:
        if constexpr (std::is_same_v<T, int64_t>)
            return static_cast<uint64_t>((static_cast<UInt128>(a) * static_cast<UInt128>(b)) >> 64);
        break;
    case Neg:
        if constexpr (isInteger)
            return answer(Wrapping { 0 } - a);
        else
            return answer(-left);
    case BitAnd:
        if constexpr (isInteger)
            return answer(a & b);
        break;
    case BitOr:
        if constexpr (isInteger)
            return answer(a | b);
        break;
    case BitXor:
        if constexpr (isInteger)
            return answer(a ^ b);
        break;
    case Shl:
        if constexpr (isInteger)
            return answer(a << (b & amountMask));
        break;
    case SShr:
        if constexpr (isInteger)
            return answer(left >> (b & amountMask));
        break;
    case ZShr:
        if constexpr (isInteger)
            return answer(a >> (b & amountMask));
        break;
    case RotR:
        if constexpr (isInteger)
            return answer(std::rotr(a, static_cast<int>(b & amountMask)));
        break;
    case Clz:
        if constexpr (isInteger)
            return static_cast<uint64_t>(std::countl_zero(a));
        break;
    case Abs:
        if constexpr (!isInteger)
            return answer(std::fabs(left));
        break;
    case Sqrt:
        if constexpr (!isInteger)
            return answer(std::sqrt(left));
        break;
    case SExt8:
        if constexpr (isInteger)
            return answer(static_cast<int8_t>(left));
        break;
    case SExt16:
        if constexpr (isInteger)
            return answer(static_cast<int16_t>(left));
        break;
    case SExt32:
        if constexpr (isInteger)
            return static_cast<uint64_t>(static_cast<int64_t>(left));
        break;
    case Trunc:
        if constexpr (isInteger)
            return static_cast<uint32_t>(a);
        break;
    case IToD:
        if constexpr (isInteger)
            return bitsOfRandomValue(static_cast<double>(left));
        break;
    case FloatToDouble:
        if constexpr (!isInteger)
            return bitsOfRandomValue(static_cast<double>(left));
        break;
    case DoubleToFloat:
        if constexpr (!isInteger)
            return bitsOfRandomValue(static_cast<float>(left));
        break;
    case Equal:
        return left == right;
    case NotEqual:
        return left != right;
    case LessThan:
        return left < right;
    case GreaterThan:
        return left > right;
    case LessEqual:
        return left <= right;
    case GreaterEqual:
        return left >= right;
    case Above:
        if constexpr (isInteger)
            return a > b;
        break;
    case Below:
        if constexpr (isInteger)
            return a < b;
        break;
    case AboveEqual:
        if constexpr (isInteger)
            return a >= b;
        break;
    case BelowEqual:
        if constexpr (isInteger)
            return a <= b;
        break;
    case EqualOrUnordered:
        if constexpr (!isInteger)
            return left == right || std::isnan(left) || std::isnan(right);
        break;
    case Nop:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case ArgumentReg:
    case PopCount:
    case Ceil:
    case Floor:
    case FTrunc:
    case FMin:
    case FMax:
    case Select:
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store:
    case Fence:
    case Phi:
    case Upsilon:
    case Extract:
    case Patch:
    case Check:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint64_t evaluateRandomOperation(Opcode opcode, TypeKind type, uint64_t left, uint64_t right)
{
    switch (type) {
    case Int32:
        return evaluateRandomOperationOn<int32_t>(opcode, static_cast<int32_t>(left), static_cast<int32_t>(right));
    case Int64:
        return evaluateRandomOperationOn<int64_t>(opcode, static_cast<int64_t>(left), static_cast<int64_t>(right));
    case Float:
        return evaluateRandomOperationOn<float>(opcode, std::bit_cast<float>(static_cast<uint32_t>(left)), std::bit_cast<float>(static_cast<uint32_t>(right)));
    case Double:
        return evaluateRandomOperationOn<double>(opcode, std::bit_cast<double>(left), std::bit_cast<double>(right));
    case Vector128:
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

struct RandomPatchSlot {
    TypeKind type { Void };
    bool isLate { false };
    bool isEarlyResult { false };
    bool isFixed { false };
    bool hasExpectedBits { false };
    bool goesThroughEmitRestore { false };
    uint64_t expectedBits { 0 };
};

struct RandomPatchShape {
    uint64_t seed { 0 };
    uint32_t number { 0 };
    bool isCall { false };
    bool mayThrow { false };
    uint8_t numberOfGPScratchRegisters { 0 };
    uint8_t numberOfFPScratchRegisters { 0 };
    std::span<const RandomPatchSlot> results;
    std::span<const RandomPatchSlot> operands;
    RegisterMask earlyClobbered { 0 };
    RegisterMask lateClobbered { 0 };
    RegisterMask allocatable { 0 };
    RegisterMask callerSavedGPRs { 0 };
    RegisterMask callerSavedFPRs { 0 };
    void* report { nullptr };
};

} // anonymous namespace

static Reg takeLowestRandomRegister(RegisterMask& registers, uint64_t seed)
{
    checkRandomProcedure(!!registers, seed, "a generator finds no register that it may write"_s);
    Reg result = Reg::fromIndex(static_cast<unsigned>(std::countr_zero(registers)));
    registers &= registers - 1;
    return result;
}

static RegisterMask lowestRandomRegisters(RegisterMask registers, unsigned count)
{
    RegisterMask result = 0;
    for (; registers && count; --count) {
        RegisterMask rest = registers & (registers - 1);
        result |= registers ^ rest;
        registers = rest;
    }
    return result;
}

static void trashRandomRegisters(CCallHelpers& jit, RegisterMask registers, GPRReg holderOfTrash)
{
    forEachRegister(registers, [&](Reg reg) {
        if (reg.isFPR())
            jit.move64ToDouble(holderOfTrash, reg.fpr());
        else if (reg.gpr() != holderOfTrash)
            jit.move(holderOfTrash, reg.gpr());
    });
}

static void leaveRandomProcedure(CCallHelpers& jit, const PatchParameters& params)
{
    if (params.frameIsSetUp())
        emitFrameTeardown(jit, params.frame());
    jit.ret();
}

static void loadBitsOfRandomOperand(CCallHelpers& jit, const ValueRep& rep, const RandomPatchSlot& slot, GPRReg destination, uint64_t seed)
{
    bool isNarrow = widthOf(Type(slot.type)) == Width32;
    checkRandomProcedure(rep.isReg() || rep.isStack() || rep.isStackArgument() || rep.isConstant(), seed, "a generator is told a constraint where it needs a place"_s);
    if (slot.goesThroughEmitRestore)
        rep.emitRestore(jit, destination);
    else {
        switch (rep.kind()) {
        case ValueRep::Register:
            if (rep.isGPR())
                jit.move(rep.gpr(), destination);
            else
                jit.moveDoubleTo64(rep.fpr(), destination);
            break;
        case ValueRep::Stack:
            if (isNarrow)
                jit.load32(CCallHelpers::Address(GPRInfo::callFrameRegister, rep.offsetFromFP()), destination);
            else
                jit.load64(CCallHelpers::Address(GPRInfo::callFrameRegister, rep.offsetFromFP()), destination);
            break;
        case ValueRep::StackArgument:
            if (isNarrow)
                jit.load32(CCallHelpers::Address(CCallHelpers::stackPointerRegister, rep.offsetFromSP()), destination);
            else
                jit.load64(CCallHelpers::Address(CCallHelpers::stackPointerRegister, rep.offsetFromSP()), destination);
            break;
        case ValueRep::Constant:
            jit.move(CCallHelpers::TrustedImm64(rep.value()), destination);
            break;
        case ValueRep::WarmAny:
        case ValueRep::ColdAny:
        case ValueRep::LateColdAny:
        case ValueRep::SomeRegister:
        case ValueRep::SomeEarlyRegister:
        case ValueRep::SomeLateRegister:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    if (isNarrow)
        jit.zeroExtend32ToWord(destination, destination);
}

static void readRandomOperand(CCallHelpers& jit, const PatchParameters& params, const RandomPatchShape& shape, unsigned operand, GPRReg destination)
{
    const RandomPatchSlot& slot = shape.operands[operand];
    loadBitsOfRandomOperand(jit, params[static_cast<unsigned>(shape.results.size()) + operand], slot, destination, shape.seed);
    if (!slot.hasExpectedBits)
        return;

    CCallHelpers::Jump isRight = jit.branch64(CCallHelpers::Equal, destination, CCallHelpers::TrustedImm64(static_cast<int64_t>(slot.expectedBits)));
    if (!params.frameIsSetUp())
        jit.emitFunctionPrologue();
    jit.move(destination, GPRInfo::argumentGPR3);
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(shape.seed)), GPRInfo::argumentGPR0);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(shape.number)), GPRInfo::argumentGPR1);
    jit.move(CCallHelpers::TrustedImm32(static_cast<int32_t>(operand)), GPRInfo::argumentGPR2);
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(slot.expectedBits)), GPRInfo::argumentGPR4);
    jit.move(CCallHelpers::TrustedImm64(std::bit_cast<int64_t>(shape.report)), GPRInfo::nonPreservedNonArgumentGPR0);
    jit.call(GPRInfo::nonPreservedNonArgumentGPR0, OperationPtrTag);
    if (!params.frameIsSetUp())
        jit.emitFunctionEpilogue();
    jit.move(CCallHelpers::TrustedImm64(answerOfRandomProcedureWithWrongOperand), GPRInfo::returnValueGPR);
    leaveRandomProcedure(jit, params);
    isRight.link(&jit);
}

static void addRandomOperandToSum(CCallHelpers& jit, const PatchParameters& params, const RandomPatchShape& shape, unsigned operand, GPRReg sum, GPRReg temporary)
{
    readRandomOperand(jit, params, shape, operand, temporary);
    jit.add64(sum, sum);
    jit.add64(temporary, sum);
}

static void storeRandomResult(CCallHelpers& jit, const ValueRep& rep, TypeKind type, GPRReg bits, FPRReg numberTemporary, uint64_t seed)
{
    checkRandomProcedure(rep.isReg() || rep.isStackArgument(), seed, "a result has neither a register nor an outgoing word"_s);
    CCallHelpers::Address word(CCallHelpers::stackPointerRegister, rep.isStackArgument() ? rep.offsetFromSP() : 0);
    switch (type) {
    case Int32:
        if (rep.isReg())
            jit.move(bits, rep.gpr());
        else
            jit.store32(bits, word);
        return;
    case Int64:
        if (rep.isReg())
            jit.move(bits, rep.gpr());
        else
            jit.store64(bits, word);
        return;
    case Float:
    case Double: {
        FPRReg number = rep.isReg() ? rep.fpr() : numberTemporary;
        checkRandomProcedure(number != InvalidFPRReg, seed, "a generator lacks the register in which it makes a number for an outgoing word"_s);
        jit.convertInt64ToDouble(bits, number);
        if (type == Float)
            jit.convertDoubleToFloat(number, number);
        if (rep.isReg())
            return;
        if (type == Float)
            jit.storeFloat(number, word);
        else
            jit.storeDouble(number, word);
        return;
    }
    case Vector128:
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void generateRandomPatch(CCallHelpers& jit, const PatchParameters& params, const RandomPatchShape& shape)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    unsigned numberOfResults = static_cast<unsigned>(shape.results.size());
    unsigned numberOfOperands = static_cast<unsigned>(shape.operands.size());
    checkRandomProcedure(params.size() == numberOfResults + numberOfOperands, shape.seed, "a generator is not told of each result and each operand"_s);

    RegisterMask registersOfParameters = 0;
    RegisterMask results = 0;
    RegisterMask fixedResults = 0;
    RegisterMask earlyResults = 0;
    RegisterMask lateOperands = 0;
    for (unsigned index = 0; index < params.size(); ++index) {
        if (!params[index].isReg())
            continue;
        RegisterMask bit = maskOf(params[index].reg());
        registersOfParameters |= bit;
        if (index >= numberOfResults) {
            if (shape.operands[index - numberOfResults].isLate)
                lateOperands |= bit;
            continue;
        }
        results |= bit;
        if (shape.results[index].isFixed)
            fixedResults |= bit;
        if (shape.results[index].isEarlyResult)
            earlyResults |= bit;
    }

    RegisterMask scratch = 0;
    GPRReg sum = InvalidGPRReg;
    GPRReg temporary = InvalidGPRReg;
    FPRReg numberTemporary = InvalidFPRReg;
    if (shape.isCall) {
        RegisterMask gprs = shape.lateClobbered & shape.callerSavedGPRs & ~registersOfParameters;
        RegisterMask fprs = shape.lateClobbered & shape.callerSavedFPRs & ~registersOfParameters;
        sum = takeLowestRandomRegister(gprs, shape.seed).gpr();
        temporary = takeLowestRandomRegister(gprs, shape.seed).gpr();
        numberTemporary = takeLowestRandomRegister(fprs, shape.seed).fpr();
    } else {
        for (unsigned index = 0; index < shape.numberOfGPScratchRegisters; ++index)
            scratch |= maskOf(Reg(params.gpScratch(index)));
        for (unsigned index = 0; index < shape.numberOfFPScratchRegisters; ++index)
            scratch |= maskOf(Reg(params.fpScratch(index)));
        sum = params.gpScratch(0);
        temporary = params.gpScratch(1);
        if (shape.numberOfFPScratchRegisters)
            numberTemporary = params.fpScratch(0);
    }

    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(trashOfRandomProcedure)), temporary);
    trashRandomRegisters(jit, shape.earlyClobbered | earlyResults, temporary);

    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(saltOfRandomPatch(shape.number))), sum);
    for (unsigned operand = 0; operand < numberOfOperands; ++operand) {
        if (!shape.operands[operand].isLate)
            addRandomOperandToSum(jit, params, shape, operand, sum, temporary);
    }

    CCallHelpers::Jump thrown;
    if (shape.mayThrow)
        thrown = jit.branchTest64(CCallHelpers::NonZero, sum, CCallHelpers::TrustedImm32(1));

    for (unsigned result = 0; result < numberOfResults; ++result) {
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(saltOfRandomResult(result))), temporary);
        jit.add64(sum, temporary);
        storeRandomResult(jit, params[result], shape.results[result].type, temporary, numberTemporary, shape.seed);
    }

    RegisterMask writtenForOperands = 0;
    for (unsigned operand = 0; operand < numberOfOperands; ++operand) {
        Reg reg = params.value()->registerToBringOperandTo(operand);
        const ValueRep& rep = params[numberOfResults + operand];
        if (reg && !(rep.isReg() && rep.reg() == reg))
            writtenForOperands |= maskOf(reg);
    }

    RegisterMask available = shape.allocatable & ~maskOf(params.unavailableRegisters()) & ~results & ~lateOperands;
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(trashOfRandomProcedure)), temporary);
    trashRandomRegisters(jit, (shape.lateClobbered & ~fixedResults) | (writtenForOperands & ~results) | available | scratch, temporary);

    for (unsigned operand = 0; operand < numberOfOperands; ++operand) {
        if (shape.operands[operand].isLate)
            readRandomOperand(jit, params, shape, operand, temporary);
    }
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(trashOfRandomProcedure)), temporary);

    if (!shape.mayThrow)
        return;
    CCallHelpers::Jump toContinuation;
    if (!params.fallsThroughToSuccessor(0))
        toContinuation = jit.jump();
    std::span<MacroAssembler::Label* const> labels = params.successorLabels();
    checkRandomProcedure(labels.size() == 2, shape.seed, "a Patch that may throw has not two successors"_s);
    params.addLatePath([thrown, toContinuation, continuation = labels[0], landingPad = labels[1], lost = shape.callerSavedGPRs | shape.callerSavedFPRs, seed = shape.seed](CCallHelpers& jit) {
        if (toContinuation.isSet())
            toContinuation.linkTo(*continuation, &jit);
        thrown.link(&jit);
        RegisterMask rest = lost;
        GPRReg holderOfTrash = takeLowestRandomRegister(rest, seed).gpr();
        jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(trashOfRandomProcedure)), holderOfTrash);
        trashRandomRegisters(jit, rest, holderOfTrash);
        jit.jump().linkTo(*landingPad, &jit);
    });
}

static void generateRandomCheck(CCallHelpers& jit, const PatchParameters& params, const RandomPatchShape& shape)
{
    AllowMacroScratchRegisterUsage allowScratch(jit);
    unsigned numberOfOperands = static_cast<unsigned>(shape.operands.size());
    checkRandomProcedure(params.size() == numberOfOperands, shape.seed, "the generator of a Check is not told of each operand"_s);

    RegisterMask gprs = shape.callerSavedGPRs;
    for (const ValueRep& rep : params) {
        if (rep.isReg())
            gprs &= ~maskOf(rep.reg());
    }
    GPRReg sum = takeLowestRandomRegister(gprs, shape.seed).gpr();
    GPRReg temporary = takeLowestRandomRegister(gprs, shape.seed).gpr();
    jit.move(CCallHelpers::TrustedImm64(static_cast<int64_t>(saltOfRandomPatch(shape.number))), sum);
    for (unsigned operand = 0; operand < numberOfOperands; ++operand)
        addRandomOperandToSum(jit, params, shape, operand, sum, temporary);
    jit.move(sum, GPRInfo::returnValueGPR);
    leaveRandomProcedure(jit, params);
}

namespace {

class RandomProgramGenerator {
public:
    RandomProgramGenerator(RandomProgram& program, const RandomProcedureOptions& options, bool functionHasHandlers)
        : m_program(program)
        , m_options(options)
        , m_random(static_cast<unsigned>(options.seed ^ (options.seed >> 32)))
        , m_blocksLeft(static_cast<int>(options.numberOfBlocks))
        , m_hasLandingPads(options.withLandingPads && functionHasHandlers)
    {
    }

    void run()
    {
        chooseVariables();
        List body;
        for (unsigned variable = 2; variable < m_program.numberOfVariables; ++variable)
            add(body, initializationOf(variable));
        for (unsigned word = 0; m_program.usesMemory && word < RandomProgram::numberOfMemoryWords; ++word) {
            Statement s;
            s.kind = Kind::Store;
            s.operands[0] = variableOf(Int64);
            s.flags = RandomProgram::UsesImmediate;
            s.immediate = word;
            add(body, s);
        }
        fill(body, 0, { }, 0);
        Statement s;
        s.kind = Kind::Return;
        s.flags = RandomProgram::ReturnsEverything;
        add(body, s);
        m_program.body = body.first;
    }

private:
    using Kind = RandomProgram::Kind;
    using Statement = RandomProgram::Statement;
    using PatchValue = RandomProgram::PatchValue;

    static constexpr unsigned maxDepth = 4;
    static constexpr unsigned maxOperandsInRegistersOfBank = 3;

    struct List {
        uint32_t first { RandomProgram::none };
        uint32_t last { RandomProgram::none };
        uint64_t assigned { 0 };
    };

    struct Surroundings {
        bool isInLoop { false };
        bool mayContinue { false };
        bool isInTry { false };
    };

    unsigned pick(size_t limit) { return m_random.getUint32(static_cast<unsigned>(limit)); }
    bool chance(unsigned percent) { return pick(100) < percent; }

    void add(List& list, const Statement& s)
    {
        uint32_t index = m_program.statements.size();
        m_program.statements.append(m_program.arena, s);
        if (list.last == RandomProgram::none)
            list.first = index;
        else
            m_program.statements[list.last].next = index;
        list.last = index;
        list.assigned |= s.assigned;
    }

    void chooseVariables()
    {
        unsigned pressure = pick(3);
        unsigned numberOfIntegers = 4 + pick(5);
        unsigned numberOfNumbers = 2 + pick(4);
        if (pressure == 1) {
            numberOfIntegers = 10 + pick(7);
            numberOfNumbers = 8 + pick(10);
        } else if (pressure == 2) {
            numberOfIntegers = 26 + pick(3);
            numberOfNumbers = 33 + pick(4);
        }
        if (!m_options.withNumbers)
            numberOfNumbers = 0;

        std::array<unsigned, numberOfRandomTypeKinds> counts { };
        counts[Int32] = numberOfIntegers / 3;
        counts[Int64] = numberOfIntegers - counts[Int32];
        counts[Float] = numberOfNumbers / 3;
        counts[Double] = numberOfNumbers - counts[Float];

        unsigned next = 0;
        for (TypeKind type : { Int64, Int32, Double, Float }) {
            unsigned numberOfWitnesses = counts[type] >= 8 ? 2 : counts[type] >= 4 ? 1 : 0;
            m_first[type] = next;
            m_count[type] = counts[type];
            m_plain[type] = counts[type] - numberOfWitnesses;
            for (unsigned index = 0; index < counts[type]; ++index, ++next) {
                m_program.types[next] = type;
                if (index < m_plain[type])
                    continue;
                m_program.isWitness[next] = true;
                m_program.witnessBits[next] = distinctiveConstantOf(type);
                m_witnesses[m_numberOfWitnesses++] = static_cast<uint8_t>(next);
            }
        }
        RELEASE_ASSERT(next <= RandomProgram::maxVariables);
        m_program.numberOfVariables = next;
        m_program.usesMemory = chance(50);
    }

    TypeKind randomType() { return m_program.types[pick(m_program.numberOfVariables)]; }
    TypeKind randomIntegerType() { return chance(50) ? Int32 : Int64; }
    uint8_t variableOf(TypeKind type) { return static_cast<uint8_t>(m_first[type] + pick(m_count[type])); }
    uint8_t plainVariableOf(TypeKind type) { return static_cast<uint8_t>(m_first[type] + pick(m_plain[type])); }

    uint64_t constantOf(TypeKind type)
    {
        static constexpr std::array<double, 10> numbers { 0.0, -0.0, 1.0, -1.0, 0.5, 2.0, -3.25, 1e10, 7.0e-5, 1e30 };
        switch (type) {
        case Int32:
        case Int64: {
            unsigned width = type == Int32 ? 32 : 64;
            uint64_t bits = 0;
            switch (pick(6)) {
            case 0:
                bits = static_cast<uint64_t>(static_cast<int64_t>(pick(9)) - 4);
                break;
            case 1:
                bits = uint64_t { 1 } << pick(width);
                break;
            case 2:
                bits = (uint64_t { 1 } << pick(width)) - 1;
                break;
            case 3:
                bits = uint64_t { 1 } << (width - 1);
                break;
            case 4:
                bits = pick(256);
                break;
            default:
                bits = m_random.getUint64();
                break;
            }
            return canonicalRandomBits(type, bits);
        }
        case Float:
            return bitsOfRandomValue(static_cast<float>(numbers[pick(numbers.size())]));
        case Double:
            if (chance(70))
                return bitsOfRandomValue(numbers[pick(numbers.size())]);
            return bitsOfRandomValue(static_cast<double>(static_cast<int32_t>(m_random.getUint32())) / 8);
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    uint64_t distinctiveConstantOf(TypeKind type)
    {
        uint64_t bits = m_random.getUint64();
        switch (type) {
        case Int32:
        case Int64:
            return canonicalRandomBits(type, bits | 1);
        case Float:
            return bitsOfRandomValue(static_cast<float>(static_cast<int16_t>(bits)) / 4 + 0.125f);
        case Double:
            return bitsOfRandomValue(static_cast<double>(static_cast<int32_t>(bits)) / 16 + 0.03125);
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Opcode relationOf(TypeKind type)
    {
        static constexpr std::array integerRelations { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual };
        static constexpr std::array numberRelations { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, EqualOrUnordered };
        if (Type(type).isInt())
            return integerRelations[pick(integerRelations.size())];
        return numberRelations[pick(numberRelations.size())];
    }

    void chooseCondition(Statement& s)
    {
        TypeKind type = randomType();
        s.compared[0] = variableOf(type);
        if (chance(50)) {
            s.flags |= RandomProgram::ComparesWithImmediate;
            s.comparedImmediate = constantOf(type);
        } else
            s.compared[1] = variableOf(type);
        if (Type(type).isInt() && chance(20)) {
            s.flags |= RandomProgram::TestsBits;
            s.relation = chance(50) ? Equal : NotEqual;
            return;
        }
        s.relation = relationOf(type);
    }

    void chooseSecondComparison(Statement& s)
    {
        TypeKind type = randomType();
        s.opcode = chance(50) ? BitAnd : BitOr;
        s.operands[0] = variableOf(type);
        if (chance(50)) {
            s.flags |= RandomProgram::UsesImmediate;
            s.immediate = constantOf(type);
        } else
            s.operands[1] = variableOf(type);
        s.secondRelation = relationOf(type);
    }

    EdgeClass randomClass()
    {
        unsigned roll = pick(100);
        if (roll < 40)
            return EdgeClass::Normal;
        if (roll < 65 || !m_options.withRareRegions)
            return EdgeClass::Unlikely;
        return EdgeClass::Rare;
    }

    unsigned percentOfReturnsBehind(EdgeClass edgeClass) const
    {
        if (m_options.withArmsThatReturn && edgeClass != EdgeClass::Rare)
            return 70;
        switch (edgeClass) {
        case EdgeClass::Normal:
            return 12;
        case EdgeClass::Unlikely:
            return 25;
        case EdgeClass::Rare:
            return 50;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    static Statement computationInto(unsigned variable)
    {
        Statement s;
        s.kind = Kind::Compute;
        s.destination = static_cast<uint8_t>(variable);
        s.assigned = RandomProgram::bitOf(variable);
        return s;
    }

    Statement initializationOf(unsigned variable)
    {
        Statement s = computationInto(variable);
        TypeKind type = m_program.types[variable];
        if (m_program.isWitness[variable]) {
            s.opcode = Opaque;
            s.flags = RandomProgram::UsesImmediate;
            s.immediate = m_program.witnessBits[variable];
            return s;
        }
        if (variable > m_first[type]) {
            static constexpr std::array operations { Add, Sub, Mul, BitXor };
            s.opcode = operations[pick(Type(type).isInt() ? 4 : 3)];
            s.operands[0] = static_cast<uint8_t>(m_first[type] + pick(variable - m_first[type]));
            s.flags = RandomProgram::UsesImmediate;
            s.immediate = distinctiveConstantOf(type);
            return s;
        }
        switch (type) {
        case Int32:
            s.opcode = Trunc;
            s.operands[0] = variableOf(Int64);
            return s;
        case Double:
            s.opcode = IToD;
            s.operands[0] = variableOf(Int64);
            return s;
        case Float:
            s.opcode = DoubleToFloat;
            s.operands[0] = variableOf(Double);
            return s;
        case Vector128:
        case Void:
        case Int64:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void setUnary(Statement& s, Opcode opcode, TypeKind typeOfOperand)
    {
        s.opcode = opcode;
        s.operands[0] = variableOf(typeOfOperand);
    }

    void setBinary(Statement& s, Opcode opcode, TypeKind type)
    {
        s.opcode = opcode;
        s.operands[0] = variableOf(type);
        if (chance(35)) {
            s.flags |= RandomProgram::UsesImmediate;
            s.immediate = constantOf(type);
        } else
            s.operands[1] = variableOf(type);
    }

    void setShift(Statement& s, TypeKind type)
    {
        static constexpr std::array operations { Shl, SShr, ZShr };
        s.opcode = operations[pick(operations.size())];
        s.operands[0] = variableOf(type);
        if (chance(50)) {
            s.flags |= RandomProgram::UsesImmediate;
            s.immediate = pick(70);
        } else
            s.operands[1] = variableOf(Int32);
    }

    void setScaledSum(Statement& s)
    {
        s.opcode = Add;
        s.flags |= RandomProgram::ScalesSecondOperand;
        s.operands[0] = variableOf(Int64);
        s.immediate = chance(75) ? 1 + pick(3) : pick(70);
        bool indexIsWord = chance(60);
        s.operands[1] = variableOf(indexIsWord ? Int32 : Int64);
        s.secondRelation = chance(70) ? ZExt32 : SExt32;
        if (!indexIsWord || chance(50))
            return;

        s.opcode = Select;
        s.compared[0] = s.operands[1];
        if (chance(70)) {
            s.flags |= RandomProgram::ComparesWithImmediate;
            s.comparedImmediate = constantOf(Int32);
        } else
            s.compared[1] = variableOf(Int32);
        s.relation = relationOf(Int32);
    }

    void setCombinedOperands(Statement& s, TypeKind type)
    {
        static constexpr std::array readersOfShifts { Add, Sub, BitAnd, BitOr, BitXor };
        static constexpr std::array shifts { Shl, SShr, ZShr };
        s.flags |= RandomProgram::CombinesOperands;
        s.operands[0] = variableOf(type);
        s.operands[1] = variableOf(type);
        if (chance(50))
            s.flags |= RandomProgram::CombinesOnLeft;
        if (type == Int64 && m_count[Int32] && chance(35)) {
            s.operands[1] = variableOf(Int32);
            s.secondRelation = chance(50) ? ZExt32 : SExt32;
            s.opcode = chance(50) ? Select : chance(50) ? Add : Sub;
            s.relation = relationOf(Int64);
            s.compared[0] = variableOf(Int64);
            return;
        }
        unsigned roll = pick(100);
        if (roll < 25) {
            s.opcode = chance(50) ? Add : Sub;
            s.secondRelation = Mul;
            s.compared[0] = variableOf(type);
            return;
        }

        s.secondRelation = shifts[pick(shifts.size())];
        s.flags |= RandomProgram::ComparesWithImmediate;
        s.comparedImmediate = pick(type == Int32 ? 34 : 66);
        if (roll < 60) {
            s.opcode = readersOfShifts[pick(readersOfShifts.size())];
            return;
        }
        s.flags |= RandomProgram::UsesImmediate | RandomProgram::CombinesOnLeft;
        if (roll < 80) {
            s.opcode = BitAnd;
            s.immediate = canonicalRandomBits(type, (uint64_t { 1 } << (1 + pick(62))) - (chance(90) ? 1 : 3));
            return;
        }
        s.secondRelation = Shl;
        s.opcode = chance(50) ? SShr : ZShr;
        s.immediate = pick(type == Int32 ? 34 : 66);
    }

    void setDivision(Statement& s, TypeKind type)
    {
        static constexpr std::array operations { Div, UDiv, Mod, UMod };
        static constexpr std::array<int64_t, 10> divisors { 1, 2, 3, 5, 7, 10, 16, 641, -3, -8 };
        s.opcode = operations[pick(operations.size())];
        s.operands[0] = variableOf(type);
        if (chance(40)) {
            s.flags |= RandomProgram::UsesImmediate;
            s.immediate = canonicalRandomBits(type, static_cast<uint64_t>(divisors[pick(divisors.size())]));
        } else
            s.operands[1] = variableOf(type);
        bool isChill = (s.opcode == Div || s.opcode == Mod) && chance(40);
        bool makesRareArm = !(s.flags & RandomProgram::UsesImmediate);
        if (isChill && (m_options.withRareRegions || !makesRareArm))
            s.flags |= RandomProgram::IsChill;
    }

    Statement randomComputation()
    {
        static constexpr std::array integerOperations { Add, Sub, Mul, BitAnd, BitOr, BitXor };
        static constexpr std::array numberOperations { Add, Sub, Mul, Div };
        static constexpr std::array unaryOperationsOnInt32 { Neg, Clz, SExt8, SExt16, Opaque };
        static constexpr std::array unaryOperationsOnDouble { Neg, Abs, Sqrt };

        TypeKind type = randomType();
        Statement s = computationInto(plainVariableOf(type));
        unsigned roll = pick(100);
        if (roll < 10) {
            s.opcode = Select;
            chooseCondition(s);
            s.operands[0] = variableOf(type);
            s.operands[1] = variableOf(type);
            return s;
        }
        switch (type) {
        case Int64:
            if (m_options.withCombinedOperands && chance(40))
                setCombinedOperands(s, Int64);
            else if (m_options.withScaledSums && chance(30))
                setScaledSum(s);
            else if (roll < 55)
                setBinary(s, integerOperations[pick(integerOperations.size())], Int64);
            else if (roll < 65)
                setShift(s, Int64);
            else if (roll < 73)
                setDivision(s, Int64);
            else if (roll < 77)
                setBinary(s, chance(50) ? MulHigh : UMulHigh, Int64);
            else if (roll < 81)
                setUnary(s, chance(50) ? Neg : Opaque, Int64);
            else if (roll < 93 || !m_count[Double])
                setUnary(s, chance(50) ? SExt32 : ZExt32, Int32);
            else
                setUnary(s, BitwiseCast, Double);
            return s;
        case Int32:
            if (m_options.withCombinedOperands && chance(40))
                setCombinedOperands(s, Int32);
            else if (roll < 40)
                setBinary(s, integerOperations[pick(integerOperations.size())], Int32);
            else if (roll < 50)
                setShift(s, Int32);
            else if (roll < 57)
                setDivision(s, Int32);
            else if (roll < 65)
                setUnary(s, unaryOperationsOnInt32[pick(unaryOperationsOnInt32.size())], Int32);
            else if (roll < 75)
                setUnary(s, Trunc, Int64);
            else if (roll < 95 || !m_count[Float]) {
                TypeKind compared = randomType();
                setBinary(s, relationOf(compared), compared);
            } else
                setUnary(s, BitwiseCast, Float);
            return s;
        case Double:
            if (roll < 60)
                setBinary(s, numberOperations[pick(numberOperations.size())], Double);
            else if (roll < 72)
                setUnary(s, unaryOperationsOnDouble[pick(unaryOperationsOnDouble.size())], Double);
            else if (roll < 86)
                setUnary(s, IToD, randomIntegerType());
            else if (roll < 93 || !m_count[Float])
                setUnary(s, BitwiseCast, Int64);
            else
                setUnary(s, FloatToDouble, Float);
            return s;
        case Float:
            if (roll < 60)
                setBinary(s, numberOperations[pick(numberOperations.size())], Float);
            else if (roll < 90)
                setUnary(s, DoubleToFloat, Double);
            else
                setUnary(s, BitwiseCast, Int32);
            return s;
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void addSwap(List& list)
    {
        TypeKind type = randomType();
        if (m_plain[type] < 2)
            type = Int64;
        Statement s;
        s.kind = Kind::Swap;
        unsigned first = pick(m_plain[type]);
        unsigned second = (first + 1 + pick(m_plain[type] - 1)) % m_plain[type];
        s.operands = { static_cast<uint8_t>(m_first[type] + first), static_cast<uint8_t>(m_first[type] + second) };
        s.assigned = RandomProgram::bitOf(s.operands[0]) | RandomProgram::bitOf(s.operands[1]);
        add(list, s);
    }

    void addCopy(List& list)
    {
        TypeKind type = randomType();
        Statement s = computationInto(plainVariableOf(type));
        s.kind = Kind::Copy;
        s.operands[0] = variableOf(type);
        add(list, s);
    }

    void addMemoryAccess(List& list)
    {
        TypeKind type = randomType();
        Statement s;
        if (chance(50)) {
            s = computationInto(plainVariableOf(type));
            s.kind = Kind::Load;
        } else {
            s.kind = Kind::Store;
            s.operands[0] = variableOf(type);
        }
        if (chance(60)) {
            s.flags |= RandomProgram::UsesImmediate;
            s.immediate = pick(RandomProgram::numberOfMemoryWords);
        } else
            s.operands[1] = variableOf(randomIntegerType());
        add(list, s);
    }

    PatchValue constantOperand()
    {
        PatchValue value;
        value.isConstant = true;
        value.type = randomType();
        value.bits = constantOf(value.type);
        return value;
    }

    PatchValue variableOperand(unsigned variable)
    {
        PatchValue value;
        value.variable = static_cast<uint8_t>(variable);
        value.type = m_program.types[variable];
        return value;
    }

    PatchValue randomOperand()
    {
        PatchValue value = chance(12) ? constantOperand() : variableOperand(pick(m_program.numberOfVariables));
        value.goesThroughEmitRestore = chance(50);
        return value;
    }

    PatchValue randomKnownOperand()
    {
        PatchValue value = m_numberOfWitnesses && chance(75) ? variableOperand(m_witnesses[pick(m_numberOfWitnesses)]) : constantOperand();
        value.goesThroughEmitRestore = chance(50);
        return value;
    }

    unsigned takeOutgoingWord(unsigned& taken)
    {
        unsigned word = pick(RandomProgram::numberOfOutgoingWords);
        while (taken & (1u << word))
            word = (word + 1) % RandomProgram::numberOfOutgoingWords;
        taken |= 1u << word;
        return word;
    }

    void append(const PatchValue& value) { m_program.patchValues.append(m_program.arena, value); }

    void addPatch(List& list, const Surroundings& around, bool mustThrow)
    {
        RandomProgram::PatchRecord record;
        record.number = m_program.patches.size();
        record.isCall = m_options.withCalls && chance(m_options.withArmsThatReturn ? 60 : 35);
        record.mayThrow = mustThrow || (around.isInTry && chance(record.isCall ? 70 : 35));

        std::array<unsigned, numberOfBanks> operandsInRegisters { 0, 0 };
        unsigned operandsAnywhere = 0;
        unsigned wordsOfOperands = 0;
        bool usesOutgoingWords = false;
        record.firstOperand = m_program.patchValues.size();
        for (unsigned count = pick(6); count--;) {
            PatchValue value = randomOperand();
            unsigned& inRegisters = operandsInRegisters[bankIndexOfRandomValue(value.type)];
            unsigned roll = pick(100);
            if (record.isCall)
                value.constraint = roll < 60 ? ValueRep::Register : roll < 80 ? ValueRep::StackArgument : roll < 90 ? ValueRep::WarmAny : ValueRep::ColdAny;
            else
                value.constraint = roll < 30 ? ValueRep::SomeRegister : roll < 50 ? ValueRep::Register : roll < 60 ? ValueRep::StackArgument : roll < 75 ? ValueRep::WarmAny : ValueRep::ColdAny;
            bool wantsRegister = value.constraint == ValueRep::Register || value.constraint == ValueRep::SomeRegister;
            if (wantsRegister && inRegisters == maxOperandsInRegistersOfBank) {
                value.constraint = ValueRep::StackArgument;
                wantsRegister = false;
            }
            if (record.isCall && (value.constraint == ValueRep::WarmAny || value.constraint == ValueRep::ColdAny) && operandsAnywhere++ >= 2)
                value.constraint = ValueRep::StackArgument;
            if (wantsRegister) {
                ++inRegisters;
                value.place = m_random.getUint32();
            } else if (value.constraint == ValueRep::StackArgument) {
                value.place = takeOutgoingWord(wordsOfOperands);
                usesOutgoingWords = true;
            }
            append(value);
        }
        for (unsigned count = pick(3); count--;) {
            PatchValue value = randomKnownOperand();
            unsigned& inRegisters = operandsInRegisters[bankIndexOfRandomValue(value.type)];
            value.constraint = ValueRep::LateColdAny;
            if (!record.isCall && inRegisters < maxOperandsInRegistersOfBank && chance(30)) {
                value.constraint = ValueRep::SomeLateRegister;
                ++inRegisters;
            }
            append(value);
        }
        record.numberOfOperands = m_program.patchValues.size() - record.firstOperand;

        Statement s;
        s.kind = Kind::Patch;
        std::array<unsigned, numberOfBanks> resultsInRegisters { 0, 0 };
        unsigned wordsOfResults = 0;
        unsigned rollOfResults = pick(100);
        record.firstResult = m_program.patchValues.size();
        for (unsigned count = rollOfResults < 15 ? 0 : rollOfResults < 65 ? 1 : rollOfResults < 88 ? 2 : 3; count--;) {
            PatchValue value = variableOperand(plainVariableOf(randomType()));
            if (s.assigned & RandomProgram::bitOf(value.variable))
                continue;
            s.assigned |= RandomProgram::bitOf(value.variable);
            unsigned bank = bankIndexOfRandomValue(value.type);
            unsigned limit = record.isCall && bank == static_cast<unsigned>(Bank::FP) ? 1 : 2;
            unsigned roll = pick(100);
            if (record.isCall)
                value.constraint = roll < 85 ? ValueRep::Register : ValueRep::StackArgument;
            else
                value.constraint = roll < 40 ? ValueRep::SomeRegister : roll < 65 ? ValueRep::SomeEarlyRegister : roll < 85 ? ValueRep::Register : ValueRep::StackArgument;
            if (resultsInRegisters[bank] == limit)
                value.constraint = ValueRep::StackArgument;
            if (value.constraint == ValueRep::StackArgument) {
                value.place = takeOutgoingWord(wordsOfResults);
                usesOutgoingWords = true;
            } else {
                value.place = record.isCall ? resultsInRegisters[bank] : m_random.getUint32();
                ++resultsInRegisters[bank];
            }
            append(value);
        }
        record.numberOfResults = m_program.patchValues.size() - record.firstResult;

        if (!record.isCall) {
            static constexpr std::array effects { RandomProgram::PatchEffects::Call, RandomProgram::PatchEffects::WritesLocalState, RandomProgram::PatchEffects::ReadsEverything, RandomProgram::PatchEffects::Nothing };
            record.effects = effects[pick(effects.size())];
            record.numberOfGPScratchRegisters = static_cast<uint8_t>(2 + pick(2));
            record.numberOfFPScratchRegisters = static_cast<uint8_t>((m_options.withNumbers ? 1 : 0) + pick(2));
            record.earlyClobbers = m_random.getUint64();
            record.earlyClobbers &= m_random.getUint64();
            record.lateClobbers = m_random.getUint64();
            record.lateClobbers &= m_random.getUint64();
            record.runsWithoutFrame = !record.mayThrow && !usesOutgoingWords && chance(50);
        }
        if (record.mayThrow)
            m_blocksLeft -= 2;

        s.record = m_program.patches.size();
        m_program.patches.append(m_program.arena, record);
        add(list, s);
    }

    void addCheck(List& list)
    {
        Statement s;
        s.kind = Kind::Check;
        s.compared[0] = plainVariableOf(randomIntegerType());
        s.flags = RandomProgram::TestsBits | RandomProgram::ComparesWithImmediate;
        s.relation = Equal;
        for (unsigned count = 3 + pick(3); count--;)
            s.comparedImmediate |= uint64_t { 1 } << pick(32);

        RandomProgram::PatchRecord record;
        record.number = m_program.patches.size();
        record.firstOperand = m_program.patchValues.size();
        std::array<unsigned, numberOfBanks> operandsInRegisters { 0, 0 };
        for (unsigned count = pick(5); count--;) {
            PatchValue value = randomOperand();
            unsigned& inRegisters = operandsInRegisters[bankIndexOfRandomValue(value.type)];
            unsigned roll = pick(100);
            value.constraint = roll < 60 ? ValueRep::ColdAny : roll < 80 ? ValueRep::WarmAny : ValueRep::SomeRegister;
            if (value.constraint == ValueRep::SomeRegister && inRegisters++ >= 2)
                value.constraint = ValueRep::ColdAny;
            append(value);
        }
        record.numberOfOperands = m_program.patchValues.size() - record.firstOperand;
        s.record = m_program.patches.size();
        m_program.patches.append(m_program.arena, record);
        add(list, s);
    }

    void addSimple(List& list, const Surroundings& around)
    {
        unsigned roll = pick(100);
        if (roll < 58)
            add(list, randomComputation());
        else if (roll < 72)
            addPatch(list, around, false);
        else if (roll < 76)
            addCheck(list);
        else if (roll < 81)
            addSwap(list);
        else if (roll < 85)
            addCopy(list);
        else if (roll < 88 && m_numberOfWitnesses)
            add(list, initializationOf(m_witnesses[pick(m_numberOfWitnesses)]));
        else if (m_program.usesMemory)
            addMemoryAccess(list);
        else
            add(list, randomComputation());
    }

    void addReturn(List& list)
    {
        Statement s;
        s.kind = Kind::Return;
        s.count = 1 + pick(4);
        s.operands[0] = static_cast<uint8_t>(pick(m_program.numberOfVariables));
        s.operands[1] = static_cast<uint8_t>(pick(m_program.numberOfVariables));
        s.compared[0] = static_cast<uint8_t>(pick(m_program.numberOfVariables));
        s.compared[1] = static_cast<uint8_t>(pick(m_program.numberOfVariables));
        add(list, s);
    }

    void addIf(List& list, unsigned depth, const Surroundings& around)
    {
        Statement s;
        s.kind = Kind::If;
        chooseCondition(s);
        if (chance(25))
            chooseSecondComparison(s);
        bool hasElse = chance(60);
        m_blocksLeft -= hasElse ? 3 : 2;
        unsigned specialEdge = pick(2);
        s.classes[specialEdge] = randomClass();
        if (chance(50))
            s.flags |= RandomProgram::KeepsScope;
        List thenList;
        bool thenReturns = fill(thenList, depth + 1, around, percentOfReturnsBehind(s.classes[0]));
        List elseList;
        if (hasElse)
            fill(elseList, depth + 1, around, thenReturns ? 0 : percentOfReturnsBehind(s.classes[1]));
        s.children = { thenList.first, elseList.first };
        s.assigned = thenList.assigned | elseList.assigned;
        add(list, s);
    }

    void addLoop(List& list, unsigned depth, const Surroundings& around)
    {
        Statement s;
        s.kind = Kind::Loop;
        m_blocksLeft -= 3;
        bool isBottomTested = chance(35);
        if (isBottomTested)
            s.flags |= RandomProgram::IsBottomTested;
        if (chance(30)) {
            s.flags |= RandomProgram::UsesImmediate;
            s.immediate = (isBottomTested ? 1 : 0) + pick(4);
        } else {
            s.operands[0] = variableOf(randomIntegerType());
            s.immediate = (isBottomTested ? 1 : 0) + pick(3);
        }
        if (chance(25))
            s.classes[1] = EdgeClass::Unlikely;
        Surroundings inside = around;
        inside.isInLoop = true;
        inside.mayContinue = !isBottomTested;
        List body;
        fill(body, depth + 1, inside, 0);
        for (unsigned count = pick(3); count--;)
            addSwap(body);
        s.children[0] = body.first;
        s.assigned = body.assigned;
        add(list, s);
    }

    void addExit(List& list, const Surroundings& around)
    {
        Statement s;
        s.kind = around.mayContinue && chance(40) ? Kind::Continue : Kind::Break;
        chooseCondition(s);
        s.classes[0] = randomClass();
        m_blocksLeft -= 1;
        add(list, s);
    }

    void addSwitch(List& list, unsigned depth, const Surroundings& around)
    {
        Statement s;
        s.kind = Kind::Switch;
        s.operands[0] = variableOf(randomIntegerType());
        s.immediate = 15;
        s.count = chance(20) ? 7 + pick(3) : 2 + pick(3);
        bool isDense = s.count >= 7 || chance(50);
        m_blocksLeft -= static_cast<int>(s.count) + 2;
        if (chance(50))
            s.flags |= RandomProgram::KeepsScope;

        std::array<RandomProgram::Case, RandomProgram::maxCases> cases;
        int64_t value = pick(3);
        for (unsigned index = 0; index < s.count; ++index) {
            cases[index].value = value;
            value += isDense ? 1 : 1 + pick(4);
            cases[index].edgeClass = randomClass();
            if (!chance(70))
                continue;
            List body;
            fill(body, depth + 1, around, percentOfReturnsBehind(cases[index].edgeClass));
            cases[index].list = body.first;
            s.assigned |= body.assigned;
        }
        List otherwise;
        fill(otherwise, depth + 1, around, 0);
        s.children[0] = otherwise.first;
        s.assigned |= otherwise.assigned;
        s.record = m_program.cases.size();
        for (unsigned index = 0; index < s.count; ++index)
            m_program.cases.append(m_program.arena, cases[index]);
        add(list, s);
    }

    void addTry(List& list, unsigned depth, const Surroundings& around)
    {
        Statement s;
        s.kind = Kind::Try;
        m_blocksLeft -= 3;
        Surroundings inside = around;
        inside.isInTry = true;
        List body;
        addPatch(body, inside, true);
        fill(body, depth + 1, inside, 0);
        List handler;
        fill(handler, depth + 1, around, 30);
        s.children = { body.first, handler.first };
        s.assigned = body.assigned | handler.assigned;
        add(list, s);
    }

    void addControl(List& list, unsigned depth, const Surroundings& around)
    {
        unsigned roll = pick(100);
        if (roll < 30)
            addIf(list, depth, around);
        else if (roll < 50)
            addLoop(list, depth, around);
        else if (roll < 60)
            addSwitch(list, depth, around);
        else if (roll < 68) {
            m_blocksLeft -= 1;
            add(list, Statement { });
        } else if (roll < 82 && around.isInLoop)
            addExit(list, around);
        else if (roll >= 82 && m_hasLandingPads)
            addTry(list, depth, around);
        else
            addIf(list, depth, around);
    }

    bool fill(List& list, unsigned depth, const Surroundings& around, unsigned percentOfReturns)
    {
        for (;;) {
            for (unsigned count = 1 + pick(2 * m_options.operationsPerBlock); count--;)
                addSimple(list, around);
            if (m_blocksLeft <= 0 || depth >= maxDepth)
                break;
            addControl(list, depth, around);
            if (depth && chance(40))
                break;
        }
        if (!chance(percentOfReturns))
            return false;
        if (m_options.withArmsThatReturn && chance(70))
            addPatch(list, around, false);
        addReturn(list);
        return true;
    }

    RandomProgram& m_program;
    const RandomProcedureOptions& m_options;
    WeakRandom m_random;
    int m_blocksLeft;
    bool m_hasLandingPads;
    std::array<unsigned, numberOfRandomTypeKinds> m_first { };
    std::array<unsigned, numberOfRandomTypeKinds> m_count { };
    std::array<unsigned, numberOfRandomTypeKinds> m_plain { };
    std::array<uint8_t, 8> m_witnesses { };
    unsigned m_numberOfWitnesses { 0 };
};

class RandomProgramEmitter {
public:
    RandomProgramEmitter(const RandomProgram& program, TestProcedure& test)
        : m_program(program)
        , m_test(test)
        , m_procedure(test.procedure())
        , m_out(test.out())
        , m_report(entryOfRandomPatchReport())
    {
        RegisterFile file = registerFileFor(m_procedure.traits());
        for (unsigned bank = 0; bank < numberOfBanks; ++bank) {
            m_callerSaved[bank] = file.allocatable[bank] & ~file.calleeSaved;
            m_allocatable |= file.allocatable[bank];
        }
    }

    void run()
    {
        m_current[0] = m_test.argumentGPR(0);
        m_current[1] = m_test.argumentGPR(1);
        if (m_program.usesMemory)
            m_memory = m_out.lockedStackSlot(RandomProgram::numberOfMemoryWords * sizeof(uint64_t));
        check(!emitList(m_program.body), "the tree does not end in a Return"_s);
    }

private:
    using Kind = RandomProgram::Kind;
    using Statement = RandomProgram::Statement;
    using PatchValue = RandomProgram::PatchValue;
    using PatchRecord = RandomProgram::PatchRecord;
    using Values = std::array<LValue, RandomProgram::maxVariables>;

    struct Join {
        Block* block { nullptr };
        uint64_t mask { 0 };
        Values phis { };
    };

    struct LoopContext {
        const Join* head;
        const Join* exit;
        LValue counter;
        LoopContext* outer;
    };

    struct HandlerContext {
        const Join* join;
        HandlerContext* outer;
        Block* landingPad { nullptr };
        Values valuesAtLandingPad { };
    };

    void check(bool condition, ASCIILiteral rule) const { checkRandomProcedure(condition, m_program.seed, rule); }
    TypeKind typeOf(unsigned variable) const { return m_program.types[variable]; }

    LValue constant(TypeKind type, uint64_t bits)
    {
        switch (type) {
        case Int32:
            return m_out.const32(static_cast<int32_t>(bits));
        case Int64:
            return m_out.const64(static_cast<int64_t>(bits));
        case Float:
            return m_out.constFloat(std::bit_cast<float>(static_cast<uint32_t>(bits)));
        case Double:
            return m_out.constDouble(std::bit_cast<double>(bits));
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LValue witness(TypeKind type, uint64_t bits)
    {
        switch (type) {
        case Int32:
        case Int64:
            return m_out.opaque(constant(type, bits));
        case Float:
            return m_out.bitwiseCast(m_out.opaque(constant(Int32, bits)), Float);
        case Double:
            return m_out.bitwiseCast(m_out.opaque(constant(Int64, bits)), Double);
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LValue withoutNaN(LValue value)
    {
        if (!value->type().isFloat())
            return value;
        LValue replacement = constant(value->type().kind(), bitsInPlaceOfRandomNaN(value->type().kind()));
        LValue isNumber = m_out.equal(value, value);
        return m_out.select(isNumber, value, replacement);
    }

    LValue bitsOf(LValue value)
    {
        switch (value->type().kind()) {
        case Int32:
            return m_out.zeroExtPtr(value);
        case Int64:
            return value;
        case Float:
            return m_out.zeroExtPtr(m_out.bitCast(value, Int32));
        case Double:
            return m_out.bitCast(value, Int64);
        case Vector128:
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LValue conditionThatMayFold(const Statement& s)
    {
        TypeKind type = typeOf(s.compared[0]);
        LValue left = m_current[s.compared[0]];
        LValue right = s.flags & RandomProgram::ComparesWithImmediate ? constant(type, s.comparedImmediate) : m_current[s.compared[1]];
        if (s.flags & RandomProgram::TestsBits) {
            left = m_out.bitAnd(left, right);
            right = constant(type, 0);
        }
        return m_out.binary(s.relation, left, right);
    }

    LValue unfolded(LValue value) { return value->isConstant() ? m_out.opaque(value) : value; }
    LValue condition(const Statement& s) { return unfolded(conditionThatMayFold(s)); }

    LValue conditionOfIf(const Statement& s)
    {
        if (s.opcode == Nop)
            return condition(s);
        TypeKind type = typeOf(s.operands[0]);
        LValue right = s.flags & RandomProgram::UsesImmediate ? constant(type, s.immediate) : m_current[s.operands[1]];
        LValue first = conditionThatMayFold(s);
        return unfolded(m_out.binary(s.opcode, first, m_out.binary(s.secondRelation, m_current[s.operands[0]], right)));
    }

    LValue compute(const Statement& s)
    {
        TypeKind result = typeOf(s.destination);
        bool usesImmediate = s.flags & RandomProgram::UsesImmediate;
        if (s.flags & RandomProgram::ScalesSecondOperand) {
            LValue index = m_current[s.operands[1]];
            if (index->type() == Int32)
                index = m_out.unary(s.secondRelation, index);
            LValue sum = m_out.binary(Add, m_current[s.operands[0]], m_out.binary(Shl, index, constant(Int32, s.immediate)));
            return s.opcode == Select ? m_out.select(conditionThatMayFold(s), sum, index) : sum;
        }
        if (s.flags & RandomProgram::CombinesOperands) {
            bool combinesOnLeft = s.flags & RandomProgram::CombinesOnLeft;
            if (s.secondRelation == ZExt32 || s.secondRelation == SExt32) {
                LValue extension = m_out.unary(s.secondRelation, m_current[s.operands[1]]);
                LValue left = combinesOnLeft ? extension : m_current[s.operands[0]];
                LValue right = combinesOnLeft ? m_current[s.operands[0]] : extension;
                if (s.opcode == Select)
                    return m_out.select(m_out.binary(s.relation, left, right), m_current[s.operands[0]], m_current[s.compared[0]]);
                return m_out.binary(s.opcode, left, right);
            }
            auto constantFor = [&](Opcode opcode, uint64_t bits) { return constant(isRandomShift(opcode) ? Int32 : result, bits); };
            LValue rightOfInner = s.flags & RandomProgram::ComparesWithImmediate ? constantFor(s.secondRelation, s.comparedImmediate) : m_current[s.compared[0]];
            LValue inner = m_out.binary(s.secondRelation, m_current[s.operands[1]], rightOfInner);
            LValue other = usesImmediate ? constantFor(s.opcode, s.immediate) : m_current[s.operands[0]];
            return combinesOnLeft ? m_out.binary(s.opcode, inner, other) : m_out.binary(s.opcode, other, inner);
        }
        if (s.opcode == Select)
            return m_out.select(conditionThatMayFold(s), m_current[s.operands[0]], m_current[s.operands[1]]);
        if (s.opcode == Opaque && usesImmediate)
            return witness(result, s.immediate);

        TypeKind type = typeOf(s.operands[0]);
        LValue first = m_current[s.operands[0]];
        if (s.opcode == Opaque)
            return m_out.opaque(first);
        if (s.opcode == BitwiseCast)
            return withoutNaN(m_out.bitwiseCast(first, result));
        if (descriptionOf(s.opcode).numberOfChildren == 1)
            return withoutNaN(m_out.unary(s.opcode, first));

        LValue second = usesImmediate ? constant(isRandomShift(s.opcode) ? Int32 : type, s.immediate) : m_current[s.operands[1]];
        if (Type(type).isInt() && isRandomDivision(s.opcode)) {
            if (!usesImmediate)
                second = m_out.bitOr(m_out.bitAnd(second, 0x7f), 1);
            return s.flags & RandomProgram::IsChill ? m_out.chill(s.opcode, first, second) : m_out.binary(s.opcode, first, second);
        }
        return withoutNaN(m_out.binary(s.opcode, first, second));
    }

    TypedPointer wordOf(const Statement& s)
    {
        const IndexedAbstractHeap& heap = m_test.heaps().indexedContiguousProperties;
        if (s.flags & RandomProgram::UsesImmediate)
            return m_out.address(m_memory, heap.at(static_cast<ptrdiff_t>(s.immediate)));
        LValue index = m_out.zeroExtPtr(m_out.bitAnd(m_current[s.operands[1]], RandomProgram::numberOfMemoryWords - 1));
        return m_out.baseIndex(heap.atAnyIndex(), m_memory, index, ScaleEight);
    }

    Join createJoin(uint64_t mask)
    {
        Join join;
        join.block = m_out.newBlock();
        join.mask = mask;
        forEachRandomVariable(mask, [&](unsigned variable) {
            join.phis[variable] = m_out.phiIn(join.block, typeOf(variable));
        });
        return join;
    }

    void passTo(const Join& join)
    {
        forEachRandomVariable(join.mask, [&](unsigned variable) {
            m_out.upsilon(m_current[variable], join.phis[variable]);
        });
    }

    void passAndJumpTo(const Join& join)
    {
        passTo(join);
        m_out.jump(join.block);
    }

    void enter(const Join& join, const ScopeFork* fork = nullptr)
    {
        if (fork)
            m_out.appendToJoin(join.block, *fork);
        else
            m_out.appendTo(join.block);
        forEachRandomVariable(join.mask, [&](unsigned variable) {
            m_current[variable] = join.phis[variable];
        });
    }

    void enterArm(Block* block, const ScopeFork* fork)
    {
        if (fork)
            m_out.appendToArm(block, *fork);
        else
            m_out.appendTo(block);
    }

    void emitArm(Block* block, const ScopeFork* fork, const Values& valuesAtFork, uint32_t list, const Join& join)
    {
        m_current = valuesAtFork;
        enterArm(block, fork);
        if (emitList(list))
            passAndJumpTo(join);
    }

    void emitIf(const Statement& s)
    {
        LValue predicate = conditionOfIf(s);
        bool hasElse = s.children[1] != RandomProgram::none;
        Join join = createJoin(s.assigned);
        Block* thenBlock = m_out.newBlock();
        Block* elseBlock = hasElse ? m_out.newBlock() : join.block;
        if (!hasElse)
            passTo(join);
        Values valuesAtFork = m_current;
        ScopeFork fork;
        const ScopeFork* keptScope = nullptr;
        if (s.flags & RandomProgram::KeepsScope) {
            fork = m_out.forkScope();
            keptScope = &fork;
        }
        m_out.branch(predicate, Edge(thenBlock, s.classes[0]), Edge(elseBlock, s.classes[1]));
        emitArm(thenBlock, keptScope, valuesAtFork, s.children[0], join);
        if (hasElse)
            emitArm(elseBlock, keptScope, valuesAtFork, s.children[1], join);
        m_current = valuesAtFork;
        enter(join, keptScope);
    }

    void emitSwitch(const Statement& s)
    {
        LValue subject = unfolded(m_out.bitAnd(m_current[s.operands[0]], static_cast<int64_t>(s.immediate)));
        Join join = createJoin(s.assigned);
        std::span<const RandomProgram::Case> cases = m_program.cases.span().subspan(s.record, s.count);
        std::array<SwitchCase, RandomProgram::maxCases> targets;
        for (unsigned index = 0; index < s.count; ++index)
            targets[index] = SwitchCase { cases[index].value, Edge(m_out.newBlock(), cases[index].edgeClass) };
        Block* otherwise = m_out.newBlock();
        Values valuesAtFork = m_current;
        ScopeFork fork;
        const ScopeFork* keptScope = nullptr;
        if (s.flags & RandomProgram::KeepsScope) {
            fork = m_out.forkScope();
            keptScope = &fork;
        }
        m_out.switchOn(subject, std::span<const SwitchCase>(targets.data(), s.count), Edge(otherwise));
        for (unsigned index = 0; index < s.count; ++index)
            emitArm(targets[index].target.block(), keptScope, valuesAtFork, cases[index].list, join);
        emitArm(otherwise, keptScope, valuesAtFork, s.children[0], join);
        m_current = valuesAtFork;
        enter(join, keptScope);
    }

    void emitLoop(const Statement& s)
    {
        LValue count = nullptr;
        if (s.flags & RandomProgram::UsesImmediate)
            count = m_out.const32(static_cast<int32_t>(s.immediate));
        else
            count = m_out.add(m_out.bitAnd(m_current[s.operands[0]], 3), static_cast<int64_t>(s.immediate));
        TypeKind typeOfCounter = count->type().kind();
        bool isBottomTested = s.flags & RandomProgram::IsBottomTested;

        Join head = createJoin(s.assigned);
        Join exit = createJoin(s.assigned);
        LValue counter = m_out.phiIn(head.block, typeOfCounter);
        m_out.upsilon(count, counter);
        passAndJumpTo(head);
        enter(head);
        if (!isBottomTested) {
            Block* body = m_out.newBlock();
            passTo(exit);
            m_out.branch(m_out.notEqual(counter, constant(typeOfCounter, 0)), Edge(body), Edge(exit.block, s.classes[1]));
            m_out.appendTo(body);
        }

        LoopContext loop { &head, &exit, counter, m_loop };
        m_loop = &loop;
        check(emitList(s.children[0]), "the body of a loop ends in a Return"_s);
        m_loop = loop.outer;

        LValue next = m_out.sub(counter, 1);
        m_out.upsilon(next, counter);
        passTo(head);
        if (isBottomTested) {
            passTo(exit);
            m_out.branch(m_out.notEqual(next, constant(typeOfCounter, 0)), Edge(head.block), Edge(exit.block, s.classes[1]));
        } else
            m_out.jump(head.block);
        enter(exit);
    }

    void emitExit(const Statement& s)
    {
        LValue predicate = condition(s);
        const Join& target = s.kind == Kind::Break ? *m_loop->exit : *m_loop->head;
        if (s.kind == Kind::Continue)
            m_out.upsilon(m_out.sub(m_loop->counter, 1), m_loop->counter);
        passTo(target);
        Block* next = m_out.newBlock();
        m_out.branch(predicate, Edge(target.block, s.classes[0]), Edge(next));
        m_out.appendTo(next);
    }

    void emitTry(const Statement& s)
    {
        Join handlerJoin = createJoin(m_program.assignedIn(s.children[0]));
        Join after = createJoin(s.assigned);
        HandlerContext handler { &handlerJoin, m_handler };
        m_handler = &handler;
        check(emitList(s.children[0]), "the body of a try ends in a Return"_s);
        m_handler = handler.outer;
        passAndJumpTo(after);
        enter(handlerJoin);
        if (emitList(s.children[1]))
            passAndJumpTo(after);
        enter(after);
    }

    void emitReturn(const Statement& s)
    {
        LValue hash = m_out.const64(static_cast<int64_t>(initialHashOfRandomProgram));
        auto mix = [&](LValue value) {
            LValue bits = bitsOf(value);
            hash = m_out.add(m_out.mul(hash, 31), bits);
        };
        if (s.flags & RandomProgram::ReturnsEverything) {
            for (unsigned variable = 0; variable < m_program.numberOfVariables; ++variable)
                mix(m_current[variable]);
            const IndexedAbstractHeap& heap = m_test.heaps().indexedContiguousProperties;
            for (unsigned word = 0; m_program.usesMemory && word < RandomProgram::numberOfMemoryWords; ++word)
                mix(m_out.load64(m_out.address(m_memory, heap.at(word))));
        } else {
            std::array<uint8_t, 4> variables { s.operands[0], s.operands[1], s.compared[0], s.compared[1] };
            for (unsigned index = 0; index < s.count; ++index)
                mix(m_current[variables[index]]);
        }
        m_out.ret(hash);
    }

    Reg registerAt(RegisterMask candidates, uint32_t ordinal)
    {
        check(!!candidates, "no register is left for a Register constraint"_s);
        for (unsigned skipped = ordinal % static_cast<unsigned>(std::popcount(candidates)); skipped--;)
            candidates &= candidates - 1;
        return Reg::fromIndex(static_cast<unsigned>(std::countr_zero(candidates)));
    }

    static Reg returnRegister(TypeKind type, unsigned ordinal)
    {
        if (Type(type).isFloat())
            return FPRInfo::returnValueFPR;
        return ordinal ? GPRInfo::returnValueGPR2 : GPRInfo::returnValueGPR;
    }

    ValueRep constraintOf(const PatchValue& value, bool isResultOfCall, RegisterMask& fixed, unsigned& demand)
    {
        switch (value.constraint) {
        case ValueRep::WarmAny:
        case ValueRep::ColdAny:
        case ValueRep::LateColdAny:
            return value.constraint;
        case ValueRep::SomeRegister:
        case ValueRep::SomeEarlyRegister:
        case ValueRep::SomeLateRegister:
            ++demand;
            return value.constraint;
        case ValueRep::Register: {
            Reg reg = isResultOfCall ? returnRegister(value.type, value.place) : registerAt(m_callerSaved[bankIndexOfRandomValue(value.type)] & ~fixed, value.place);
            fixed |= maskOf(reg);
            return ValueRep::reg(reg);
        }
        case ValueRep::StackArgument:
            m_procedure.requestCallArgAreaSizeInBytes(static_cast<unsigned>((value.place + 1) * sizeof(uint64_t)));
            return ValueRep::stackArgument(static_cast<ValueRep::OffsetType>(value.place * sizeof(uint64_t)));
        case ValueRep::Stack:
        case ValueRep::Constant:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    LValue valueOf(const PatchValue& value) { return value.isConstant ? constant(value.type, value.bits) : m_current[value.variable]; }

    RandomPatchShape* createShape(const PatchRecord& record)
    {
        Arena& arena = m_procedure.arena();
        std::span<const PatchValue> operands = m_program.operandsOf(record);
        std::span<const PatchValue> results = m_program.resultsOf(record);
        std::span<RandomPatchSlot> operandSlots = arena.createSpan<RandomPatchSlot>(operands.size());
        for (size_t index = 0; index < operands.size(); ++index) {
            const PatchValue& operand = operands[index];
            RandomPatchSlot& slot = operandSlots[index];
            slot.type = operand.type;
            slot.isLate = isLateRandomConstraint(operand.constraint);
            slot.goesThroughEmitRestore = operand.goesThroughEmitRestore;
            slot.hasExpectedBits = operand.isConstant || m_program.isWitness[operand.variable];
            slot.expectedBits = operand.isConstant ? operand.bits : m_program.witnessBits[operand.variable];
        }
        std::span<RandomPatchSlot> resultSlots = arena.createSpan<RandomPatchSlot>(results.size());
        for (size_t index = 0; index < results.size(); ++index) {
            resultSlots[index].type = results[index].type;
            resultSlots[index].isEarlyResult = results[index].constraint == ValueRep::SomeEarlyRegister;
            resultSlots[index].isFixed = results[index].constraint == ValueRep::Register;
        }

        RandomPatchShape* shape = arena.create<RandomPatchShape>();
        shape->seed = m_program.seed;
        shape->number = record.number;
        shape->isCall = record.isCall;
        shape->mayThrow = record.mayThrow;
        shape->numberOfGPScratchRegisters = record.numberOfGPScratchRegisters;
        shape->numberOfFPScratchRegisters = record.numberOfFPScratchRegisters;
        shape->results = resultSlots;
        shape->operands = operandSlots;
        shape->allocatable = m_allocatable;
        shape->callerSavedGPRs = m_callerSaved[static_cast<unsigned>(Bank::GP)];
        shape->callerSavedFPRs = m_callerSaved[static_cast<unsigned>(Bank::FP)];
        shape->report = m_report;
        return shape;
    }

    static Effects effectsOf(RandomProgram::PatchEffects effects)
    {
        Effects result = Effects::none();
        switch (effects) {
        case RandomProgram::PatchEffects::Call:
            return Effects::forCall();
        case RandomProgram::PatchEffects::WritesLocalState:
            result.writesLocalState = true;
            return result;
        case RandomProgram::PatchEffects::ReadsEverything:
            result.reads = HeapSet::top();
            return result;
        case RandomProgram::PatchEffects::Nothing:
            return result;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Block* landingPadForCurrentValues(bool& isNew)
    {
        HandlerContext& handler = *m_handler;
        bool isShared = handler.landingPad;
        forEachRandomVariable(handler.join->mask, [&](unsigned variable) {
            isShared = isShared && handler.valuesAtLandingPad[variable] == m_current[variable];
        });
        isNew = !isShared;
        if (isNew) {
            handler.landingPad = m_out.newBlock();
            handler.valuesAtLandingPad = m_current;
        }
        return handler.landingPad;
    }

    void emitPatch(const Statement& s)
    {
        const PatchRecord& record = m_program.patches[s.record];
        std::span<const PatchValue> operands = m_program.operandsOf(record);
        std::span<const PatchValue> results = m_program.resultsOf(record);
        RandomPatchShape* shape = createShape(record);

        std::array<RegisterMask, numberOfBanks> fixedOperands { 0, 0 };
        std::array<RegisterMask, numberOfBanks> fixedResults { 0, 0 };
        std::array<unsigned, numberOfBanks> demand { record.numberOfGPScratchRegisters, record.numberOfFPScratchRegisters };
        std::array<LValue, RandomProgram::maxOperands> values { };
        std::array<ValueRep, RandomProgram::maxOperands> operandConstraints;
        for (size_t index = 0; index < operands.size(); ++index) {
            unsigned bank = bankIndexOfRandomValue(operands[index].type);
            values[index] = valueOf(operands[index]);
            operandConstraints[index] = constraintOf(operands[index], false, fixedOperands[bank], demand[bank]);
        }
        ArenaVector<ValueRep> resultConstraints;
        std::array<Type, RandomProgram::maxResults> typesOfResults;
        for (size_t index = 0; index < results.size(); ++index) {
            unsigned bank = bankIndexOfRandomValue(results[index].type);
            typesOfResults[index] = results[index].type;
            resultConstraints.append(m_procedure.arena(), constraintOf(results[index], record.isCall, fixedResults[bank], demand[bank]));
        }

        for (unsigned bank = 0; bank < numberOfBanks; ++bank) {
            if (record.isCall) {
                shape->lateClobbered |= m_callerSaved[bank];
                continue;
            }
            RegisterMask fixed = fixedOperands[bank] | fixedResults[bank];
            RegisterMask free = m_callerSaved[bank] & ~fixed;
            unsigned numberFree = static_cast<unsigned>(std::popcount(free));
            check(numberFree >= demand[bank], "a Patch asks for more registers than there are"_s);
            RegisterMask allowed = lowestRandomRegisters((record.earlyClobbers | record.lateClobbers) & free, numberFree - demand[bank]);
            shape->earlyClobbered |= record.earlyClobbers & (allowed | (fixedResults[bank] & ~fixedOperands[bank]));
            shape->lateClobbered |= record.lateClobbers & (allowed | fixed);
        }

        Type type = Void;
        if (results.size() == 1)
            type = typesOfResults[0];
        else if (results.size() > 1)
            type = m_procedure.addTuple(std::span<const Type>(typesOfResults.data(), results.size()));

        Block* continuation = nullptr;
        Block* landingPad = nullptr;
        bool landingPadIsNew = false;
        if (record.mayThrow) {
            check(m_handler, "a Patch may throw outside a try"_s);
            continuation = m_out.newBlock();
            landingPad = landingPadForCurrentValues(landingPadIsNew);
        }

        PatchOperation* patch = m_out.patch(type);
        for (size_t index = 0; index < operands.size(); ++index)
            patch->append(values[index], operandConstraints[index]);
        if (!results.empty())
            patch->resultConstraints = resultConstraints;
        patch->effects = effectsOf(record.effects);
        patch->clobberEarly(shape->earlyClobbered);
        patch->clobberLate(shape->lateClobbered);
        patch->numGPScratchRegisters = record.numberOfGPScratchRegisters;
        patch->numFPScratchRegisters = record.numberOfFPScratchRegisters;
        patch->setRunsWithoutFrame(record.runsWithoutFrame);
        patch->setShowsRegistersToCollector(record.runsWithoutFrame);
        patch->setBringsOperandsToTheirRegisters(!(shape->number % 2));
        patch->setGenerator([shape](CCallHelpers& jit, const PatchParameters& params) {
            generateRandomPatch(jit, params, *shape);
        });

        if (record.mayThrow) {
            m_out.m_block->appendSuccessor(Edge(continuation, EdgeClass::Normal));
            m_out.m_block->appendSuccessor(Edge(landingPad, EdgeClass::Rare));
            patch->clobberLateOnSuccessor(1, registerSetOf(shape->callerSavedGPRs | shape->callerSavedFPRs));
            patch->effects.terminal = true;
            if (landingPadIsNew) {
                m_out.appendTo(landingPad);
                passAndJumpTo(*m_handler->join);
            }
            m_out.appendTo(continuation);
        }

        if (results.size() == 1)
            m_current[results[0].variable] = patch;
        for (unsigned index = 0; results.size() > 1 && index < results.size(); ++index)
            m_current[results[index].variable] = m_out.extract(patch, index);
    }

    void emitCheck(const Statement& s)
    {
        const PatchRecord& record = m_program.patches[s.record];
        std::span<const PatchValue> operands = m_program.operandsOf(record);
        RandomPatchShape* shape = createShape(record);
        std::array<LValue, RandomProgram::maxOperands> values { };
        for (size_t index = 0; index < operands.size(); ++index)
            values[index] = valueOf(operands[index]);
        PatchOperation* operation = m_out.checkIsZero(conditionThatMayFold(s));
        for (size_t index = 0; index < operands.size(); ++index)
            operation->append(values[index], operands[index].constraint);
        operation->setGenerator([shape](CCallHelpers& jit, const PatchParameters& params) {
            generateRandomCheck(jit, params, *shape);
        });
    }

    bool emitList(uint32_t index)
    {
        for (; index != RandomProgram::none; index = m_program.statements[index].next) {
            if (!emit(m_program.statements[index]))
                return false;
        }
        return true;
    }

    bool emit(const Statement& s)
    {
        switch (s.kind) {
        case Kind::Compute:
            m_current[s.destination] = compute(s);
            return true;
        case Kind::Copy:
            m_current[s.destination] = m_current[s.operands[0]];
            return true;
        case Kind::Swap:
            std::swap(m_current[s.operands[0]], m_current[s.operands[1]]);
            return true;
        case Kind::Load:
            m_current[s.destination] = withoutNaN(m_out.load(wordOf(s), typeOf(s.destination)));
            return true;
        case Kind::Store:
            m_out.store(m_current[s.operands[0]], wordOf(s));
            return true;
        case Kind::Patch:
            emitPatch(s);
            return true;
        case Kind::Check:
            emitCheck(s);
            return true;
        case Kind::Split: {
            Block* next = m_out.newBlock();
            m_out.jump(next);
            m_out.appendToContinuation(next);
            return true;
        }
        case Kind::If:
            emitIf(s);
            return true;
        case Kind::Loop:
            emitLoop(s);
            return true;
        case Kind::Break:
        case Kind::Continue:
            emitExit(s);
            return true;
        case Kind::Switch:
            emitSwitch(s);
            return true;
        case Kind::Try:
            emitTry(s);
            return true;
        case Kind::Return:
            emitReturn(s);
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const RandomProgram& m_program;
    TestProcedure& m_test;
    Procedure& m_procedure;
    Output& m_out;
    void* m_report;
    std::array<RegisterMask, numberOfBanks> m_callerSaved { 0, 0 };
    RegisterMask m_allocatable { 0 };
    Values m_current { };
    LValue m_memory { nullptr };
    LoopContext* m_loop { nullptr };
    HandlerContext* m_handler { nullptr };
};

class RandomProgramInterpreter {
public:
    RandomProgramInterpreter(const RandomProgram& program, int64_t a, int64_t b)
        : m_program(program)
    {
        m_values[0] = static_cast<uint64_t>(a);
        m_values[1] = static_cast<uint64_t>(b);
    }

    int64_t run()
    {
        checkRandomProcedure(runList(m_program.body) == Completion::Return, m_program.seed, "the tree does not end in a Return"_s);
        return static_cast<int64_t>(m_result);
    }

private:
    using Kind = RandomProgram::Kind;
    using Statement = RandomProgram::Statement;
    using PatchValue = RandomProgram::PatchValue;

    enum class Completion : uint8_t { Normal, Break, Continue, Throw, Return };

    TypeKind typeOf(unsigned variable) const { return m_program.types[variable]; }
    uint64_t bitsOf(const PatchValue& value) const { return value.isConstant ? value.bits : m_values[value.variable]; }

    bool condition(const Statement& s) const
    {
        uint64_t left = m_values[s.compared[0]];
        uint64_t right = s.flags & RandomProgram::ComparesWithImmediate ? s.comparedImmediate : m_values[s.compared[1]];
        if (s.flags & RandomProgram::TestsBits) {
            left &= right;
            right = 0;
        }
        return evaluateRandomOperation(s.relation, typeOf(s.compared[0]), left, right);
    }

    bool conditionOfIf(const Statement& s) const
    {
        bool first = condition(s);
        if (s.opcode == Nop)
            return first;
        uint64_t right = s.flags & RandomProgram::UsesImmediate ? s.immediate : m_values[s.operands[1]];
        bool second = evaluateRandomOperation(s.secondRelation, typeOf(s.operands[0]), m_values[s.operands[0]], right);
        return s.opcode == BitAnd ? first && second : first || second;
    }

    uint64_t compute(const Statement& s) const
    {
        bool usesImmediate = s.flags & RandomProgram::UsesImmediate;
        if (s.flags & RandomProgram::ScalesSecondOperand) {
            uint64_t index = m_values[s.operands[1]];
            if (typeOf(s.operands[1]) == Int32 && s.secondRelation == SExt32)
                index = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(index)));
            if (s.opcode == Select && !condition(s))
                return index;
            return m_values[s.operands[0]] + (index << (s.immediate & 63));
        }
        if (s.flags & RandomProgram::CombinesOperands) {
            TypeKind type = typeOf(s.destination);
            bool combinesOnLeft = s.flags & RandomProgram::CombinesOnLeft;
            if (s.secondRelation == ZExt32 || s.secondRelation == SExt32) {
                uint32_t word = static_cast<uint32_t>(m_values[s.operands[1]]);
                uint64_t extension = s.secondRelation == ZExt32 ? word : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(word)));
                uint64_t left = combinesOnLeft ? extension : m_values[s.operands[0]];
                uint64_t right = combinesOnLeft ? m_values[s.operands[0]] : extension;
                if (s.opcode == Select)
                    return m_values[evaluateRandomOperation(s.relation, Int64, left, right) ? s.operands[0] : s.compared[0]];
                return evaluateRandomOperation(s.opcode, Int64, left, right);
            }
            uint64_t rightOfInner = s.flags & RandomProgram::ComparesWithImmediate ? s.comparedImmediate : m_values[s.compared[0]];
            uint64_t inner = canonicalRandomBits(type, evaluateRandomOperation(s.secondRelation, type, m_values[s.operands[1]], rightOfInner));
            uint64_t other = usesImmediate ? s.immediate : m_values[s.operands[0]];
            return canonicalRandomBits(type, evaluateRandomOperation(s.opcode, type, combinesOnLeft ? inner : other, combinesOnLeft ? other : inner));
        }
        if (s.opcode == Select)
            return m_values[s.operands[condition(s) ? 0 : 1]];
        if (s.opcode == Opaque && usesImmediate)
            return s.immediate;
        TypeKind type = typeOf(s.operands[0]);
        uint64_t right = usesImmediate ? s.immediate : m_values[s.operands[1]];
        if (Type(type).isInt() && isRandomDivision(s.opcode) && !usesImmediate)
            right = (right & 0x7f) | 1;
        TypeKind result = typeOf(s.destination);
        return withoutRandomNaN(result, canonicalRandomBits(result, evaluateRandomOperation(s.opcode, type, m_values[s.operands[0]], right)));
    }

    uint64_t& wordOf(const Statement& s)
    {
        uint64_t index = s.flags & RandomProgram::UsesImmediate ? s.immediate : m_values[s.operands[1]];
        return m_memory[index & (RandomProgram::numberOfMemoryWords - 1)];
    }

    uint64_t sumOfEarlyOperands(const RandomProgram::PatchRecord& record) const
    {
        uint64_t sum = saltOfRandomPatch(record.number);
        for (const PatchValue& operand : m_program.operandsOf(record)) {
            if (!isLateRandomConstraint(operand.constraint))
                sum = sum * 2 + bitsOf(operand);
        }
        return sum;
    }

    Completion runPatch(const Statement& s)
    {
        const RandomProgram::PatchRecord& record = m_program.patches[s.record];
        uint64_t sum = sumOfEarlyOperands(record);
        if (record.mayThrow && (sum & 1))
            return Completion::Throw;
        unsigned index = 0;
        for (const PatchValue& result : m_program.resultsOf(record))
            m_values[result.variable] = bitsOfRandomResult(result.type, sum + saltOfRandomResult(index++));
        return Completion::Normal;
    }

    Completion runLoop(const Statement& s)
    {
        uint64_t count = s.immediate;
        if (!(s.flags & RandomProgram::UsesImmediate))
            count += m_values[s.operands[0]] & 3;
        bool isBottomTested = s.flags & RandomProgram::IsBottomTested;
        while (isBottomTested || count) {
            Completion completion = runList(s.children[0]);
            if (completion == Completion::Break)
                break;
            if (completion == Completion::Throw || completion == Completion::Return)
                return completion;
            if (!--count)
                break;
        }
        return Completion::Normal;
    }

    Completion runSwitch(const Statement& s)
    {
        int64_t subject = static_cast<int64_t>(m_values[s.operands[0]] & s.immediate);
        for (const RandomProgram::Case& switchCase : m_program.cases.span().subspan(s.record, s.count)) {
            if (switchCase.value == subject)
                return runList(switchCase.list);
        }
        return runList(s.children[0]);
    }

    void runReturn(const Statement& s)
    {
        m_result = initialHashOfRandomProgram;
        auto mix = [&](uint64_t bits) {
            m_result = m_result * 31 + bits;
        };
        if (s.flags & RandomProgram::ReturnsEverything) {
            for (unsigned variable = 0; variable < m_program.numberOfVariables; ++variable)
                mix(m_values[variable]);
            for (unsigned word = 0; m_program.usesMemory && word < RandomProgram::numberOfMemoryWords; ++word)
                mix(m_memory[word]);
            return;
        }
        std::array<uint8_t, 4> variables { s.operands[0], s.operands[1], s.compared[0], s.compared[1] };
        for (unsigned index = 0; index < s.count; ++index)
            mix(m_values[variables[index]]);
    }

    Completion runList(uint32_t index)
    {
        for (; index != RandomProgram::none; index = m_program.statements[index].next) {
            Completion completion = run(m_program.statements[index]);
            if (completion != Completion::Normal)
                return completion;
        }
        return Completion::Normal;
    }

    Completion run(const Statement& s)
    {
        switch (s.kind) {
        case Kind::Compute:
            m_values[s.destination] = compute(s);
            return Completion::Normal;
        case Kind::Copy:
            m_values[s.destination] = m_values[s.operands[0]];
            return Completion::Normal;
        case Kind::Swap:
            std::swap(m_values[s.operands[0]], m_values[s.operands[1]]);
            return Completion::Normal;
        case Kind::Load: {
            TypeKind type = typeOf(s.destination);
            m_values[s.destination] = withoutRandomNaN(type, canonicalRandomBits(type, wordOf(s)));
            return Completion::Normal;
        }
        case Kind::Store: {
            uint64_t& word = wordOf(s);
            uint64_t bits = m_values[s.operands[0]];
            word = widthOf(Type(typeOf(s.operands[0]))) == Width32 ? (word & ~uint64_t { UINT32_MAX }) | bits : bits;
            return Completion::Normal;
        }
        case Kind::Patch:
            return runPatch(s);
        case Kind::Check:
            if (!condition(s))
                return Completion::Normal;
            m_result = sumOfEarlyOperands(m_program.patches[s.record]);
            return Completion::Return;
        case Kind::Split:
            return Completion::Normal;
        case Kind::If:
            return runList(s.children[conditionOfIf(s) ? 0 : 1]);
        case Kind::Loop:
            return runLoop(s);
        case Kind::Break:
            return condition(s) ? Completion::Break : Completion::Normal;
        case Kind::Continue:
            return condition(s) ? Completion::Continue : Completion::Normal;
        case Kind::Switch:
            return runSwitch(s);
        case Kind::Try: {
            Completion completion = runList(s.children[0]);
            return completion == Completion::Throw ? runList(s.children[1]) : completion;
        }
        case Kind::Return:
            runReturn(s);
            return Completion::Return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const RandomProgram& m_program;
    std::array<uint64_t, RandomProgram::maxVariables> m_values { };
    std::array<uint64_t, RandomProgram::numberOfMemoryWords> m_memory { };
    uint64_t m_result { 0 };
};

} // anonymous namespace

static void destroyRandomProgram(RandomProgram* program)
{
    delete program;
}

std::unique_ptr<RandomProgram, void (*)(RandomProgram*)> buildRandomProcedure(TestProcedure& test, const RandomProcedureOptions& options)
{
    std::unique_ptr<RandomProgram, void (*)(RandomProgram*)> program(new RandomProgram, destroyRandomProgram);
    program->seed = options.seed;
    RandomProgramGenerator(*program, options, test.procedure().traits().hasHandlers).run();
    RandomProgramEmitter(*program, test).run();
    return program;
}

int64_t interpret(const RandomProgram& program, int64_t a, int64_t b)
{
    return RandomProgramInterpreter(program, a, b).run();
}

} } } // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
