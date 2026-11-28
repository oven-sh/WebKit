/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineOpcode.h"
#include "AOTMachineTestSupport.h"
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#if OS(LINUX) && !ASAN_ENABLED

namespace JSC {
namespace AOT {
namespace Machine {

struct ExecutionFuzzRequest {
    uint64_t seed { 1 };
    unsigned count { 1000 };
    unsigned inputs { 5 };
    std::optional<bool> checksAllocation;
    std::optional<uint64_t> seedToShow;
};

String fuzzOneExecution(uint64_t seed, const ExecutionFuzzRequest&);
unsigned fuzzExecution(const ExecutionFuzzRequest&, PrintStream&);

}
}
} // namespace JSC::AOT::Machine

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace AOT {
namespace Machine {
namespace ExecutionFuzzer {

struct RandomNumbers {
    uint64_t state;
    uint64_t next()
    {
        uint64_t mixed = (state += 0x9e3779b97f4a7c15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebull;
        return mixed ^ (mixed >> 31);
    }
    unsigned pick(size_t count) { return count ? static_cast<unsigned>(next() % count) : 0; }
    bool chance(unsigned percent) { return pick(100) < percent; }
};

enum Region : uint8_t {
    FieldsRegion,
    BytesRegion,
    ImmutableRegion,
    RootRegion,
    InstanceRegion,
    ArgumentsRegion,
    SumsRegion,
    InstanceWordsRegion,
    SlotRegion,
    ConstantDataRegion,
    numberOfRegions,
};
inline constexpr const char* regionNames[] = { "fields", "bytes", "const", "top", "inst", "args", "sums", "words", "slot", "data" };
inline constexpr unsigned regionSize = 128;
inline constexpr unsigned slotSize = 64;
inline unsigned sizeOfRegion(unsigned region) { return region == SlotRegion ? slotSize : regionSize; }

enum AddressingMode : uint8_t {
    FixedOffset,
    ZeroExtendedIndex,
    SignExtendedIndex,
    WideIndex,
    NarrowReference,
    MultipliedIndex,
    SteppedIndex,
    SumIndex,
};
struct MemoryPlace {
    uint8_t region { 0 }, mode { FixedOffset }, scale { 0 };
    bool usesRootHeap { false };
    int index { -1 };
    int32_t offset { 0 };
    int32_t step { 0 };
    uint64_t mask { 0 };
    int baseVariable { -1 };
};

struct Expression {
    enum Kind : uint8_t {
        Variable,
        Constant,
        UnaryOperation,
        BinaryOperation,
        SelectOperation,
        MemoryLoad,
        ChillOperation,
        LowHalf,
        NonNegativeExtension,
        OpaqueValue,
        BitCast,
        CanonicalNumber,
        LinkedNumber,
    } kind { Constant };
    Opcode opcode { Nop };
    TypeKind type { Int32 };
    uint64_t bits { 0 };
    int variable { -1 };
    int children[3] { -1, -1, -1 };
    MemoryPlace place;
};

enum PatchKind : uint8_t {
    CallPatch,
    StubPatch,
    InlinePatch,
    PurePatch,
    KeepAlivePatch,
    ThrowingPatch,
    BranchingPatch,
    ExitingPatch,
    SuspensionPatch,
    TailPatch,
    numberOfPatchKinds,
};
inline constexpr const char* patchKindNames[] = { "call", "stub", "inline", "pure", "keep", "throw", "ways", "exitcall", "suspend", "tail" };

struct PatchOperand {
    int expression { -1 };
    uint8_t constraint { 0 };
    uint32_t location { 0 };
    bool isSlotAddress { false };
};
struct PatchResult {
    int variable { -1 };
    TypeKind type { Int64 };
    uint8_t constraint { 0 };
    uint32_t location { 0 };
};
struct PatchDescription {
    PatchKind kind { CallPatch };
    std::vector<PatchOperand> operands;
    std::vector<PatchResult> results;
    uint8_t reads { 0 }, writes { 0 };
    bool mayCollect { false }, exitsSideways { false }, isControlDependent { false }, writesLocalState { false };
    uint64_t earlyClobbers { 0 }, lateClobbers { 0 };
    uint8_t numberOfGPScratches { 0 }, numberOfFPScratches { 0 };
    bool runsWithoutFrame { false }, bringsOperands { false }, needsFrameRecord { false }, fillsRegisterFromResult { false }, constrainsResult { false }, movesIntoResult { false }, clearsUpperHalf { false };
    unsigned numberOfSuccessors { 0 };
    uint64_t salt { 0 };
    bool readsSlot { false };
};

struct Statement {
    enum Kind : uint8_t {
        Assignment,
        MemoryStore,
        PatchCall,
        ExitCheck,
        MemoryFence,
        MemoryUpdate,
        ResultsOfPatch,
        TrappingLoad,
        PointerSelection,
        NumberedRead,
        LazyConstant,
    } kind { Assignment };
    int variable { -1 };
    int expression { -1 };
    Opcode access { Store };
    Opcode opcode { Nop };
    bool hasConstantOnLeft { false };
    MemoryPlace place;
    int patch { -1 };
    uint8_t form { 0 };
    uint64_t key { 0 };
};
struct ProgramBlock {
    enum Terminator : uint8_t {
        JumpTerminator,
        BranchTerminator,
        SwitchTerminator,
        ReturnTerminator,
        PatchTerminator,
        UnreachableTerminator,
    } terminator { ReturnTerminator };
    std::vector<Statement> statements;
    int expression { -1 };
    int patch { -1 };
    std::vector<int> successors;
    std::vector<uint8_t> edgeClasses;
    std::vector<int64_t> caseValues;
    bool isPatchContinuation { false };
};

struct Program {
    uint64_t seed { 0 };
    std::vector<TypeKind> variableTypes;
    std::vector<char> mayHoldAnyBits;
    std::vector<Expression> expressions;
    std::vector<PatchDescription> patches;
    std::vector<ProgramBlock> blocks;
    uint64_t traitBits { 0 };
    uint64_t emissionSeed { 0 };
    int branchReach { -1 };
    bool usesHostFeatures { true };
    bool usesScopes { true };
    unsigned numberOfSlots { 1 };
    bool usesSlot { true }, usesArguments { false }, suspends { false }, marksNarrowReferences { false };
    std::vector<char> pointerKinds;
    unsigned stackCheckMode { 0 };
    int secondEntryBlock { -1 };
    unsigned extraSlotSize { 0 };
};

inline constexpr uint8_t firstHeapList = 60;
struct MemoryRange {
    unsigned region;
    int offset;
};
inline std::vector<MemoryRange> memoryRangesOf(uint8_t kind)
{
    switch (kind) {
    case 60:
        return { { FieldsRegion, 8 }, { FieldsRegion, 24 } };
    case 61:
        return { { FieldsRegion, 16 }, { InstanceRegion, -1 } };
    case 62:
        return { { FieldsRegion, 24 }, { BytesRegion, -1 }, { FieldsRegion, 40 } };
    default:
        return { { kind - 2u, -1 } };
    }
}

inline bool hasObservableEffects(const PatchDescription& description)
{
    return description.kind == BranchingPatch || description.kind == ThrowingPatch || description.kind == SuspensionPatch || description.kind == TailPatch || description.exitsSideways || description.writesLocalState || description.mayCollect || description.writes;
}

inline constexpr uint64_t firstSeedWithNumberedReads = 1000000000;
inline constexpr int fuelVariable = 0;
inline constexpr int bufferVariable = 8, narrowBufferVariable = 9;
inline constexpr int firstFreeVariable = 10;
inline constexpr unsigned numberOfArgumentVariables = 8;

inline uint64_t truncateToType(TypeKind type, uint64_t bits) { return type == Int32 || type == Float ? static_cast<uint32_t>(bits) : bits; }
inline double doubleFromBits(uint64_t bits)
{
    double value;
    memcpy(&value, &bits, 8);
    return value;
}
inline float floatFromBits(uint64_t bits)
{
    uint32_t word = static_cast<uint32_t>(bits);
    float value;
    memcpy(&value, &word, 4);
    return value;
}
inline uint64_t bitsFromDouble(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);
    return bits;
}
inline uint64_t bitsFromFloat(float value)
{
    uint32_t word;
    memcpy(&word, &value, 4);
    return word;
}

inline bool isNaN(TypeKind type, uint64_t bits) { return type == Double ? std::isnan(doubleFromBits(bits)) : type == Float ? std::isnan(floatFromBits(bits))
                                                                                                                           : false; }

template<typename T> T minimumOf(T left, T right)
{
    if (std::isnan(left) || std::isnan(right))
        return std::numeric_limits<T>::quiet_NaN();
    if (left == right)
        return std::signbit(left) ? left : right;
    return left < right ? left : right;
}
template<typename T> T maximumOf(T left, T right)
{
    if (std::isnan(left) || std::isnan(right))
        return std::numeric_limits<T>::quiet_NaN();
    if (left == right)
        return std::signbit(left) ? right : left;
    return left > right ? left : right;
}

inline constexpr int64_t smallestCheckedSum = -(int64_t { 1 } << 52), largestCheckedSum = (int64_t { 1 } << 52) - 1;
inline uint32_t linkedNumber(uint64_t seed, unsigned index)
{
    RandomNumbers random { seed * 31 + index };
    uint64_t bits = random.next();
    return static_cast<uint32_t>(index & 1 ? bits & 0xffff : index & 2 ? bits & 0x7fffffff
                                                                       : bits & 0x7f);
}

inline uint64_t evaluateUnary(Opcode opcode, TypeKind from, uint64_t operand)
{
    uint32_t word = static_cast<uint32_t>(operand);
    switch (opcode) {
    case Neg:
        if (from == Int32)
            return static_cast<uint32_t>(0u - word);
        if (from == Int64)
            return 0ull - operand;
        if (from == Float)
            return bitsFromFloat(-floatFromBits(operand));
        return bitsFromDouble(-doubleFromBits(operand));
    case Clz:
        return from == Int32 ? (word ? __builtin_clz(word) : 32) : (operand ? __builtin_clzll(operand) : 64);
    case PopCount:
        return from == Int32 ? __builtin_popcount(word) : __builtin_popcountll(operand);
    case Abs:
        return from == Float ? bitsFromFloat(std::fabs(floatFromBits(operand))) : bitsFromDouble(std::fabs(doubleFromBits(operand)));
    case Ceil:
        return from == Float ? bitsFromFloat(std::ceil(floatFromBits(operand))) : bitsFromDouble(std::ceil(doubleFromBits(operand)));
    case Floor:
        return from == Float ? bitsFromFloat(std::floor(floatFromBits(operand))) : bitsFromDouble(std::floor(doubleFromBits(operand)));
    case FTrunc:
        return from == Float ? bitsFromFloat(std::trunc(floatFromBits(operand))) : bitsFromDouble(std::trunc(doubleFromBits(operand)));
    case Sqrt:
        return from == Float ? bitsFromFloat(std::sqrt(floatFromBits(operand))) : bitsFromDouble(std::sqrt(doubleFromBits(operand)));
    case SExt8:
        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(word)));
    case SExt16:
        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(word)));
    case SExt32:
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(word)));
    case ZExt32:
        return word;
    case Trunc:
        return word;
    case IToD:
        return from == Int32 ? bitsFromDouble(static_cast<double>(static_cast<int32_t>(word))) : bitsFromDouble(static_cast<double>(static_cast<int64_t>(operand)));
    case FloatToDouble:
        return bitsFromDouble(static_cast<double>(floatFromBits(operand)));
    case DoubleToFloat:
        return bitsFromFloat(static_cast<float>(doubleFromBits(operand)));
    default:
        abort();
    }
}

template<typename S, typename U> uint64_t evaluateInteger(Opcode opcode, uint64_t leftBits, uint64_t rightBits)
{
    U left = static_cast<U>(leftBits), right = static_cast<U>(rightBits);
    S signedLeft = static_cast<S>(left), signedRight = static_cast<S>(right);
    constexpr unsigned shiftMask = sizeof(U) * 8 - 1;
    unsigned shiftAmount = static_cast<unsigned>(rightBits) & shiftMask;
    auto widen = [](U value) { return static_cast<uint64_t>(value); };
    switch (opcode) {
    case Add:
        return widen(left + right);
    case Sub:
        return widen(left - right);
    case Mul:
        return widen(static_cast<U>(static_cast<uint64_t>(left) * static_cast<uint64_t>(right)));
    case MulHigh:
        return sizeof(U) == 8 ? static_cast<uint64_t>((static_cast<__int128>(signedLeft) * static_cast<__int128>(signedRight)) >> 64) : widen(static_cast<U>((static_cast<int64_t>(signedLeft) * static_cast<int64_t>(signedRight)) >> 32));
    case UMulHigh:
        return sizeof(U) == 8 ? static_cast<uint64_t>((static_cast<unsigned __int128>(left) * static_cast<unsigned __int128>(right)) >> 64) : widen(static_cast<U>((static_cast<uint64_t>(left) * static_cast<uint64_t>(right)) >> 32));
    case Div:
        if (!signedRight)
            abort();
        return signedRight == -1 ? widen(U(0) - left) : widen(static_cast<U>(signedLeft / signedRight));
    case Mod:
        if (!signedRight)
            abort();
        return signedRight == -1 ? 0 : widen(static_cast<U>(signedLeft % signedRight));
    case UDiv:
        if (!right)
            abort();
        return widen(left / right);
    case UMod:
        if (!right)
            abort();
        return widen(left % right);
    case BitAnd:
        return widen(left & right);
    case BitOr:
        return widen(left | right);
    case BitXor:
        return widen(left ^ right);
    case Shl:
        return widen(static_cast<U>(left << shiftAmount));
    case SShr:
        return widen(static_cast<U>(signedLeft >> shiftAmount));
    case ZShr:
        return widen(static_cast<U>(left >> shiftAmount));
    case RotR:
        return widen(shiftAmount ? static_cast<U>((left >> shiftAmount) | (left << (shiftMask + 1 - shiftAmount))) : left);
    case Equal:
        return left == right;
    case NotEqual:
        return left != right;
    case LessThan:
        return signedLeft < signedRight;
    case GreaterThan:
        return signedLeft > signedRight;
    case LessEqual:
        return signedLeft <= signedRight;
    case GreaterEqual:
        return signedLeft >= signedRight;
    case Above:
        return left > right;
    case Below:
        return left < right;
    case AboveEqual:
        return left >= right;
    case BelowEqual:
        return left <= right;
    default:
        abort();
    }
}

template<typename T> bool compareNumbers(Opcode opcode, T left, T right)
{
    switch (opcode) {
    case Equal:
        return left == right;
    case NotEqual:
        return !(left == right);
    case LessThan:
        return left < right;
    case GreaterThan:
        return left > right;
    case LessEqual:
        return left <= right;
    case GreaterEqual:
        return left >= right;
    case EqualOrUnordered:
        return left == right || std::isnan(left) || std::isnan(right);
    default:
        abort();
    }
}

inline uint64_t evaluateBinary(Opcode opcode, TypeKind from, uint64_t left, uint64_t right)
{
    if (from == Int32)
        return evaluateInteger<int32_t, uint32_t>(opcode, left, right);
    if (from == Int64)
        return evaluateInteger<int64_t, uint64_t>(opcode, left, right);
    if (isComparison(opcode))
        return from == Float ? compareNumbers(opcode, floatFromBits(left), floatFromBits(right)) : compareNumbers(opcode, doubleFromBits(left), doubleFromBits(right));
    volatile double leftDouble = doubleFromBits(left), rightDouble = doubleFromBits(right);
    volatile float leftFloat = floatFromBits(left), rightFloat = floatFromBits(right);
    bool isFloat = from == Float;
    switch (opcode) {
    case Add:
        return isFloat ? bitsFromFloat(leftFloat + rightFloat) : bitsFromDouble(leftDouble + rightDouble);
    case Sub:
        return isFloat ? bitsFromFloat(leftFloat - rightFloat) : bitsFromDouble(leftDouble - rightDouble);
    case Mul:
        return isFloat ? bitsFromFloat(leftFloat * rightFloat) : bitsFromDouble(leftDouble * rightDouble);
    case Div:
        return isFloat ? bitsFromFloat(leftFloat / rightFloat) : bitsFromDouble(leftDouble / rightDouble);
    case FMin:
        return isFloat ? bitsFromFloat(minimumOf<float>(leftFloat, rightFloat)) : bitsFromDouble(minimumOf<double>(leftDouble, rightDouble));
    case FMax:
        return isFloat ? bitsFromFloat(maximumOf<float>(leftFloat, rightFloat)) : bitsFromDouble(maximumOf<double>(leftDouble, rightDouble));
    default:
        abort();
    }
}

inline TypeKind unaryResultType(Opcode opcode, TypeKind from)
{
    switch (opcode) {
    case SExt32:
    case ZExt32:
        return Int64;
    case Trunc:
        return Int32;
    case IToD:
    case FloatToDouble:
        return Double;
    case DoubleToFloat:
        return Float;
    default:
        return from;
    }
}

struct Memory {
    uint8_t regions[numberOfRegions][regionSize];
};
inline void fillMemory(Memory& memory, uint64_t seed)
{
    RandomNumbers random { seed };
    for (unsigned choice = 0; choice < numberOfRegions; ++choice) {
        for (unsigned i = 0; i < regionSize; i += 8) {
            uint64_t bits = random.next();
            switch (random.pick(5)) {
            case 0:
                bits = random.pick(4);
                break;
            case 1:
                bits = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(bits)));
                break;
            case 2:
                bits = bitsFromDouble(static_cast<double>(static_cast<int32_t>(bits)) / 8);
                break;
            }
            memcpy(&memory.regions[choice][i], &bits, 8);
        }
    }
    for (unsigned i = 0; i < regionSize; i += 8) {
        static const int64_t sums[] = { 0, 5, -1, -2, 0xffffffffll, 0x100000000ll, 0x1ffffffffll, -0x100000000ll, largestCheckedSum, largestCheckedSum - 1, smallestCheckedSum, smallestCheckedSum + 1, 0x7fffffff, 0x80000000ll };
        int64_t value = random.chance(60) ? sums[random.pick(14)] : static_cast<int64_t>(random.next()) >> 11;
        memcpy(&memory.regions[SumsRegion][i], &value, 8);
    }
    memset(memory.regions[SlotRegion], 0, regionSize);
    memset(memory.regions[ConstantDataRegion], 0, regionSize);
}

inline void fillConstantData(Memory& memory, uint64_t seed)
{
    RandomNumbers random { seed ^ 0xda7a };
    for (unsigned i = 0; i < regionSize; i += 8) {
        uint64_t bits = random.chance(50) ? random.next() : random.pick(100);
        memcpy(&memory.regions[ConstantDataRegion][i], &bits, 8);
    }
}

struct TraceEvent {
    int patch { -1 };
    bool isCheck { false };
    bool exits { false };
    unsigned successor { 0 };
    std::vector<uint64_t> operands;
    std::vector<uint64_t> results;
    std::vector<uint64_t> writes;
    bool hasMemory { false };
    uint64_t memoryHash { 0 }, slotHash { 0 };
    bool hasSlot { false };
};
struct Trace {
    std::vector<TraceEvent> events;
    uint64_t result { 0 };
    bool didExit { false };
    bool isTooLong { false };
    Memory finalMemory;
    uint64_t steps { 0 };
};

inline uint64_t hashBytes(const uint8_t* bytes, unsigned count, uint64_t hash = 0x243f6a8885a308d3ull)
{
    for (unsigned i = 0; i < count; i += 8) {
        uint64_t bits;
        memcpy(&bits, bytes + i, 8);
        hash = (hash ^ bits) * 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 29;
    }
    return hash;
}
template<typename Where> uint64_t hashOfRanges(uint8_t kind, const Where& where)
{
    uint64_t hash = 0x243f6a8885a308d3ull;
    for (const MemoryRange& piece : memoryRangesOf(kind))
        hash = piece.offset >= 0 ? hashBytes(where(piece.region) + piece.offset, 8, hash) : hashBytes(where(piece.region), sizeOfRegion(piece.region), hash);
    return hash;
}
inline uint64_t hashOfMemory(const Memory& memory)
{
    uint64_t hash = 1;
    for (unsigned choice = 0; choice < SlotRegion; ++choice)
        hash = hashBytes(memory.regions[choice], regionSize, hash);
    return hash;
}

struct ProgramArguments {
    uint64_t integers[4];
    uint64_t numbers[3];
};

struct Interpreter {
    const Program& program;
    std::vector<uint64_t> variables;
    Memory memory;
    Trace trace;
    std::vector<uint64_t> pendingResults;
    uint64_t maxSteps { 40000 };

    explicit Interpreter(const Program& program)
        : program(program)
    {
    }

    unsigned offsetOfPlace(const MemoryPlace& place, uint64_t index) const
    {
        int64_t offset = place.offset;
        switch (place.mode) {
        case FixedOffset:
        case NarrowReference:
            break;
        case ZeroExtendedIndex:
            offset += static_cast<int64_t>((index & place.mask) << place.scale);
            break;
        case SignExtendedIndex:
            offset += static_cast<int64_t>((index & place.mask) << place.scale);
            break;
        case WideIndex:
            offset += static_cast<int64_t>((index & place.mask) << place.scale);
            break;
        case MultipliedIndex:
            offset += static_cast<int64_t>((index & place.mask) * place.scale);
            break;
        case SteppedIndex:
            offset += static_cast<int64_t>(((index & place.mask) + static_cast<uint64_t>(static_cast<int64_t>(place.step))) << place.scale);
            break;
        case SumIndex:
            offset += static_cast<int64_t>(index & place.mask) + place.step;
            break;
        }
        return static_cast<unsigned>(offset);
    }
    uint64_t load(Opcode opcode, TypeKind type, unsigned region, unsigned offset) const
    {
        const uint8_t* bytes = memory.regions[region] + offset;
        switch (opcode) {
        case Load8Z:
            return bytes[0];
        case Load8S:
            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(bytes[0])));
        case Load16Z: {
            uint16_t half;
            memcpy(&half, bytes, 2);
            return half;
        }
        case Load16S: {
            int16_t otherHalf;
            memcpy(&otherHalf, bytes, 2);
            return static_cast<uint32_t>(static_cast<int32_t>(otherHalf));
        }
        default:
            break;
        }
        if (type == Int32 || type == Float) {
            uint32_t word;
            memcpy(&word, bytes, 4);
            return word;
        }
        uint64_t distance;
        memcpy(&distance, bytes, 8);
        return distance;
    }
    void store(Opcode opcode, TypeKind type, unsigned region, unsigned offset, uint64_t value)
    {
        uint8_t* bytes = memory.regions[region] + offset;
        unsigned size = opcode == Store8 ? 1 : opcode == Store16 ? 2
            : type == Int32 || type == Float                     ? 4
                                                                 : 8;
        memcpy(bytes, &value, size);
    }
    uint64_t evaluate(int expression)
    {
        const Expression& node = program.expressions[expression];
        switch (node.kind) {
        case Expression::Variable:
            return variables[node.variable];
        case Expression::Constant:
            return truncateToType(node.type, node.bits);
        case Expression::UnaryOperation:
            return truncateToType(node.type, evaluateUnary(node.opcode, program.expressions[node.children[0]].type, evaluate(node.children[0])));
        case Expression::BinaryOperation: {
            uint64_t first = evaluate(node.children[0]), second = evaluate(node.children[1]);
            return evaluateBinary(node.opcode, program.expressions[node.children[0]].type, first, second);
        }
        case Expression::SelectOperation: {
            uint64_t third = evaluate(node.children[0]), first = evaluate(node.children[1]), second = evaluate(node.children[2]);
            return third ? first : second;
        }
        case Expression::MemoryLoad: {
            uint64_t i = node.place.index >= 0 ? evaluate(node.place.index) : 0;
            return load(node.opcode, node.type, node.place.region, offsetOfPlace(node.place, i));
        }
        case Expression::ChillOperation: {
            uint64_t first = evaluate(node.children[0]), second = evaluate(node.children[1]);
            return second ? evaluateBinary(node.opcode, node.type, first, second) : 0;
        }
        case Expression::LowHalf:
            return static_cast<uint32_t>(evaluate(node.children[0]));
        case Expression::NonNegativeExtension:
            return static_cast<uint32_t>(evaluate(node.children[0]));
        case Expression::OpaqueValue:
            return evaluate(node.children[0]);
        case Expression::BitCast:
            return evaluate(node.children[0]);
        case Expression::LinkedNumber:
            return linkedNumber(program.seed, static_cast<unsigned>(node.bits));
        case Expression::CanonicalNumber: {
            uint64_t first = evaluate(node.children[0]);
            return isNaN(node.type, first) ? (node.type == Double ? bitsFromDouble(1.5) : bitsFromFloat(1.5f)) : first;
        }
        }
        abort();
    }
    static uint64_t valueFromHash(TypeKind type, uint64_t hash)
    {
        switch (type) {
        case Int32:
            return hash % 5 == 0 ? static_cast<uint32_t>(static_cast<int32_t>(hash >> 8) % 4) : static_cast<uint32_t>(hash >> 16);
        case Int64:
            return hash % 5 == 0 ? static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(hash >> 7))) : hash % 7 == 0 ? (hash >> 40)
                                                                                                                                : hash;
        case Float:
            return hash % 11 == 0 ? 0x7fc00000u : hash % 13 == 0 ? 0x80000000u
                                                                 : bitsFromFloat(static_cast<float>(static_cast<int32_t>(hash >> 20)) / 4);
        case Double:
            return hash % 11 == 0 ? 0x7ff8000000000000ull : hash % 13 == 0 ? 0x8000000000000000ull
                                                                           : bitsFromDouble(static_cast<double>(static_cast<int64_t>(hash) >> (hash & 31)) / 8);
        default:
            abort();
        }
    }
    static uint64_t valueOfKey(TypeKind type, uint64_t salt)
    {
        uint64_t hash = (salt + 3) * 0x94d049bb133111ebull;
        return truncateToType(type, valueFromHash(type, hash ^ (hash >> 31)));
    }
    bool runPatch(int patch, bool isCheck)
    {
        const PatchDescription& description = program.patches[patch];
        TraceEvent event;
        event.patch = patch;
        event.isCheck = isCheck;
        uint64_t hash = description.salt;
        for (const PatchOperand& operand : description.operands) {
            uint64_t bits = operand.isSlotAddress ? 0 : evaluate(operand.expression);
            event.operands.push_back(bits);
            hash = (hash ^ bits) * 0x9e3779b97f4a7c15ull;
            hash ^= hash >> 31;
        }
        if (isCheck) {
            event.exits = true;
            event.hasMemory = true;
            event.memoryHash = hashOfMemory(memory);
            trace.events.push_back(event);
            trace.didExit = true;
            return false;
        }
        if (description.kind == KeepAlivePatch)
            return true;
        bool inTrace = hasObservableEffects(description);
        if (description.reads == 1) {
            event.hasMemory = true;
            event.memoryHash = hashOfMemory(memory);
            if (!inTrace)
                hash ^= event.memoryHash * 0xff51afd7ed558ccdull;
        } else if (description.kind == PurePatch && description.reads)
            hash ^= hashOfRanges(description.reads, [&](unsigned choice) { return memory.regions[choice]; }) * 0xff51afd7ed558ccdull;
        if (inTrace)
            hash ^= (trace.events.size() + 1) * 0xc2b2ae3d27d4eb4full;
        hash ^= hash >> 29;
        if (description.readsSlot) {
            event.hasSlot = true;
            event.slotHash = hashBytes(memory.regions[SlotRegion], slotSize);
        }
        if (description.kind == TailPatch)
            event.results.push_back(valueFromHash(Int64, hash * 77 + 5));
        for (unsigned i = 0; i < description.results.size(); ++i) {
            uint64_t mixed = (hash + i + 1) * 0xbf58476d1ce4e5b9ull;
            event.results.push_back(truncateToType(description.results[i].type, valueFromHash(description.results[i].type, mixed ^ (mixed >> 27))));
        }
        if (!inTrace) {
            pendingResults = event.results;
            return true;
        }
        if (description.writes) {
            for (unsigned i = 0; i < 2; ++i) {
                uint64_t mixed = (hash + 77 + i) * 0x94d049bb133111ebull;
                mixed ^= mixed >> 31;
                std::vector<MemoryRange> pieces = memoryRangesOf(description.writes);
                MemoryRange piece = pieces[i % pieces.size()];
                unsigned region = description.writes == 1 ? static_cast<unsigned>(mixed % 5) : piece.region;
                if (region == ImmutableRegion)
                    region = BytesRegion;
                if (region == SlotRegion || (description.writes == 1 && description.readsSlot && i)) {
                    if (!description.readsSlot)
                        continue;
                    region = SlotRegion;
                }
                unsigned offset = static_cast<unsigned>((mixed >> 12) % (sizeOfRegion(region) / 8)) * 8;
                if (description.writes >= firstHeapList && piece.offset >= 0)
                    offset = static_cast<unsigned>(piece.offset);
                uint64_t value = valueFromHash(Int64, mixed * 31 + 7);
                memcpy(memory.regions[region] + offset, &value, 8);
                event.writes.push_back(region);
                event.writes.push_back(offset);
                event.writes.push_back(value);
            }
        }
        if (description.kind == ExitingPatch && hash % 9 == 0) {
            event.exits = true;
            trace.events.push_back(event);
            trace.didExit = true;
            return false;
        }
        if (description.kind == ThrowingPatch)
            event.successor = (hash >> 9) % 3 == 0;
        if (description.kind == BranchingPatch)
            event.successor = (hash >> 9) % description.numberOfSuccessors;
        pendingResults = event.results;
        trace.events.push_back(event);
        return true;
    }
    void takeResults(int patch)
    {
        const PatchDescription& description = program.patches[patch];
        for (unsigned i = 0; i < description.results.size(); ++i)
            variables[description.results[i].variable] = pendingResults[i];
    }
    unsigned lastPatchSuccessor() const { return trace.events.back().successor; }

    void run(const ProgramArguments& arguments, const Memory& start, bool atSecondEntry = false)
    {
        memory = start;
        variables.assign(program.variableTypes.size(), 0);
        trace = Trace();
        for (unsigned i = 0; i < 4; ++i)
            variables[1 + i] = arguments.integers[i];
        for (unsigned i = 0; i < 3; ++i)
            variables[5 + i] = arguments.numbers[i];
        variables[bufferVariable] = variables[narrowBufferVariable] = 1;
        int currentBlock = atSecondEntry ? program.secondEntryBlock : 0;
        for (;;) {
            const ProgramBlock& block = program.blocks[currentBlock];
            for (const Statement& statement : block.statements) {
                if (++trace.steps > maxSteps) {
                    trace.isTooLong = true;
                    return;
                }
                switch (statement.kind) {
                case Statement::Assignment:
                    variables[statement.variable] = evaluate(statement.expression);
                    break;
                case Statement::MemoryStore: {
                    uint64_t value = evaluate(statement.expression);
                    uint64_t i = statement.place.index >= 0 ? evaluate(statement.place.index) : 0;
                    store(statement.access, program.expressions[statement.expression].type, statement.place.region, offsetOfPlace(statement.place, i), value);
                    break;
                }
                case Statement::MemoryUpdate: {
                    uint64_t i = statement.place.index >= 0 ? evaluate(statement.place.index) : 0;
                    TypeKind type = program.expressions[statement.expression].type;
                    unsigned offset = offsetOfPlace(statement.place, i);
                    Opcode loadOpcode = statement.access == Store8 ? Load8Z : statement.access == Store16 ? Load16Z
                                                                                                          : Load;
                    uint64_t old = load(loadOpcode, type, statement.place.region, offset);
                    uint64_t other = evaluate(statement.expression);
                    uint64_t sum = statement.hasConstantOnLeft ? evaluateBinary(statement.opcode, type, other, old) : evaluateBinary(statement.opcode, type, old, other);
                    store(statement.access, type, statement.place.region, offset, sum);
                    if (statement.variable >= 0)
                        variables[statement.variable] = truncateToType(type, (statement.form & 1) ? old : sum);
                    break;
                }
                case Statement::PatchCall:
                    if (!runPatch(statement.patch, false))
                        return;
                    if (program.patches[statement.patch].kind != KeepAlivePatch)
                        takeResults(statement.patch);
                    break;
                case Statement::ExitCheck:
                    if (evaluate(statement.expression) && !runPatch(statement.patch, true))
                        return;
                    break;
                case Statement::MemoryFence:
                    break;
                case Statement::PointerSelection:
                    variables[statement.variable] = evaluate(statement.expression) ? 1 : 0;
                    break;
                case Statement::NumberedRead:
                    variables[statement.variable] = (evaluate(statement.expression) & 1) ? load(Load, program.variableTypes[statement.variable], statement.place.region, offsetOfPlace(statement.place, 0)) : valueOfKey(program.variableTypes[statement.variable], statement.key);
                    break;
                case Statement::LazyConstant:
                    variables[statement.variable] = valueOfKey(program.variableTypes[statement.variable], statement.key);
                    break;
                case Statement::TrappingLoad: {
                    if (!variables[statement.place.baseVariable]) {
                        TraceEvent event;
                        event.patch = statement.patch;
                        event.isCheck = event.exits = event.hasMemory = true;
                        event.memoryHash = hashOfMemory(memory);
                        trace.events.push_back(event);
                        trace.didExit = true;
                        return;
                    }
                    if (statement.variable >= 0)
                        variables[statement.variable] = load(statement.access, Int32, statement.place.region, offsetOfPlace(statement.place, 0));
                    break;
                }
                case Statement::ResultsOfPatch:
                    takeResults(statement.patch);
                    break;
                }
            }
            if (++trace.steps > maxSteps) {
                trace.isTooLong = true;
                return;
            }
            switch (block.terminator) {
            case ProgramBlock::JumpTerminator:
                currentBlock = block.successors[0];
                break;
            case ProgramBlock::BranchTerminator:
                currentBlock = block.successors[evaluate(block.expression) ? 0 : 1];
                break;
            case ProgramBlock::SwitchTerminator: {
                uint64_t value = evaluate(block.expression);
                int64_t otherValue = program.expressions[block.expression].type == Int32 ? static_cast<int64_t>(static_cast<int32_t>(value)) : static_cast<int64_t>(value);
                unsigned i = 0;
                while (i < block.caseValues.size() && block.caseValues[i] != otherValue)
                    ++i;
                currentBlock = block.successors[i];
                break;
            }
            case ProgramBlock::ReturnTerminator:
                trace.result = evaluate(block.expression);
                trace.finalMemory = memory;
                return;
            case ProgramBlock::PatchTerminator:
                if (!runPatch(block.patch, false))
                    return;
                if (block.successors.empty()) {
                    trace.result = trace.events.back().results.size() ? trace.events.back().results[0] : 0;
                    trace.finalMemory = memory;
                    return;
                }
                currentBlock = block.successors[lastPatchSuccessor()];
                break;
            case ProgramBlock::UnreachableTerminator:
                abort();
            }
        }
    }
};

Program generateProgram(uint64_t seed);

}
}
}
} // namespace JSC::AOT::Machine::ExecutionFuzzer

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // OS(LINUX) && !ASAN_ENABLED

#endif // ENABLE(AOT)
