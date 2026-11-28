/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineExecutionFuzzer.h"

#if ENABLE(AOT) && OS(LINUX) && !ASAN_ENABLED

#include "AOTMachineTarget.h"
#include <algorithm>
#include <set>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace AOT {
namespace Machine {
namespace ExecutionFuzzer {

static const uint64_t interestingConstants[] = {
    0xfffe000000000000ull, 0xfffe000000000002ull, 0xfffe000000000001ull, ~0xaull, 0x3ff0000000000000ull, 0x7ff8000000000000ull, 0x2000000000000ull,
    0xaull, 0x2ull, 0x6ull, 0x7ull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0xffffffffull, 0x100000000ull, 0x80000000ull, 0x7fffffffull, 0xffffffff80000000ull
};

struct ProgramGenerator {
    Program& program;
    RandomNumbers random;
    uint64_t seed;
    unsigned pressure { 0 };
    bool withNumbers { true }, withCalls { true }, withMemory { true }, withChecks { true }, withThrow { true };
    unsigned depthOfTrees { 2 };
    unsigned blocksLeft { 12 };
    unsigned statementsPerBlock { 5 };
    unsigned patchPercent { 12 };
    unsigned rarePercent { 15 };
    bool loadsOften { false };
    bool boxes { false };
    unsigned constantMode { 0 };
    std::vector<int> handlers;
    struct Loop {
        int header, exit;
    };
    std::vector<Loop> loops;
    std::vector<char> isCheckPatch;
    unsigned reusePercent { 20 };
    unsigned trapPercent { 0 }, suspendPercent { 0 };
    unsigned readPercent { 0 }, chainPercent { 0 };
    int pointerVar { -1 }, narrowVar { -1 };

    ProgramGenerator(Program& program, uint64_t seed)
        : program(program)
        , random { seed * 0x9e3779b97f4a7c15ull + 12345 }
        , seed(seed)
    {
    }

    int addExpression(const Expression& node)
    {
        program.expressions.push_back(node);
        return static_cast<int>(program.expressions.size()) - 1;
    }
    int newBlock()
    {
        program.blocks.push_back(ProgramBlock());
        return static_cast<int>(program.blocks.size()) - 1;
    }
    int newVariable(TypeKind type, bool raw = true)
    {
        program.variableTypes.push_back(type);
        program.mayHoldAnyBits.push_back(raw);
        program.pointerKinds.push_back(0);
        return static_cast<int>(program.variableTypes.size()) - 1;
    }

    TypeKind integerType() { return random.chance(50) ? Int32 : Int64; }
    TypeKind anyType()
    {
        if (withNumbers && random.chance(30))
            return random.chance(70) ? Double : Float;
        return integerType();
    }
    int variableOfType(TypeKind type)
    {
        std::vector<int> candidates;
        for (unsigned i = 1; i < program.variableTypes.size(); ++i) {
            if (program.variableTypes[i] == type && !program.pointerKinds[i])
                candidates.push_back(static_cast<int>(i));
        }
        return candidates.empty() ? -1 : candidates[random.pick(candidates.size())];
    }
    uint64_t constantBits(TypeKind type)
    {
        switch (type) {
        case Int32:
        case Int64: {
            unsigned width = type == Int32 ? 32 : 64;
            uint64_t bits;
            switch (random.pick(constantMode == 1 ? 12 : 9)) {
            case 0:
                bits = static_cast<uint64_t>(static_cast<int64_t>(random.pick(9)) - 4);
                break;
            case 1:
                bits = 1ull << random.pick(width);
                break;
            case 2:
                bits = (1ull << random.pick(width)) - 1;
                break;
            case 3:
                bits = random.pick(300);
                break;
            case 4:
                bits = static_cast<uint64_t>(-static_cast<int64_t>(random.pick(300)));
                break;
            case 5:
                bits = interestingConstants[random.pick(std::size(interestingConstants))];
                break;
            case 6:
                bits = random.next();
                break;
            case 7:
                bits = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(random.next())));
                break;
            case 8:
                bits = (~0ull >> random.pick(64)) << random.pick(64);
                break;
            case 9:
                bits = (random.next() & 0xfff) << 12;
                break;
            case 10:
                bits = (random.next() & 0xffff) << (16 * random.pick(4));
                break;
            default:
                bits = (1ull << random.pick(width)) + static_cast<uint64_t>(static_cast<int64_t>(random.pick(3)) - 1);
                break;
            }
            return truncateToType(type, bits);
        }
        case Double: {
            static const double doubles[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 2.0, -3.25, 1e10, 7e-5, 1e300, 4294967295.0, 2147483648.0, -2147483648.0, 9007199254740992.0, 9223372036854775808.0, 5e-324 };
            if (random.chance(70))
                return bitsFromDouble(doubles[random.pick(std::size(doubles))]);
            if (random.chance(20))
                return random.chance(50) ? 0x7ff8000000000000ull : random.chance(50) ? 0x7ff0000000000000ull
                                                                                     : 0xfff0000000000000ull;
            return bitsFromDouble(static_cast<double>(static_cast<int32_t>(random.next())) / 8);
        }
        case Float: {
            static const float floats[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -3.25f, 1e10f, 7e-5f, 1e30f };
            if (random.chance(80))
                return bitsFromFloat(floats[random.pick(std::size(floats))]);
            return bitsFromFloat(static_cast<float>(static_cast<int16_t>(random.next())) / 4);
        }
        default:
            abort();
        }
    }
    int makeConstant(TypeKind type, uint64_t bits)
    {
        Expression node;
        node.kind = Expression::Constant;
        node.type = type;
        node.bits = truncateToType(type, bits);
        return addExpression(node);
    }
    int makeVariable(int variable)
    {
        Expression node;
        node.kind = Expression::Variable;
        node.type = program.variableTypes[variable];
        node.variable = variable;
        return addExpression(node);
    }
    int makeUnary(Opcode opcode, int left)
    {
        Expression node;
        node.kind = Expression::UnaryOperation;
        node.opcode = opcode;
        node.type = unaryResultType(opcode, program.expressions[left].type);
        node.children[0] = left;
        return addExpression(node);
    }
    int makeBinary(Opcode opcode, int left, int right)
    {
        Expression node;
        node.kind = Expression::BinaryOperation;
        node.opcode = opcode;
        node.type = isComparison(opcode) ? Int32 : program.expressions[left].type;
        node.children[0] = left;
        node.children[1] = right;
        return addExpression(node);
    }
    int makeSelect(int condition, int left, int right)
    {
        Expression node;
        node.kind = Expression::SelectOperation;
        node.type = program.expressions[left].type;
        node.children[0] = condition;
        node.children[1] = left;
        node.children[2] = right;
        return addExpression(node);
    }
    int wrapExpression(Expression::Kind kind, TypeKind type, int left)
    {
        Expression node;
        node.kind = kind;
        node.type = type;
        node.children[0] = left;
        return addExpression(node);
    }

    bool isCanonical(int value) const
    {
        const Expression& node = program.expressions[value];
        if (node.type != Float && node.type != Double)
            return true;
        switch (node.kind) {
        case Expression::Variable:
            return program.mayHoldAnyBits[node.variable];
        case Expression::Constant:
        case Expression::MemoryLoad:
        case Expression::BitCast:
        case Expression::CanonicalNumber:
            return true;
        case Expression::SelectOperation:
            return isCanonical(node.children[1]) && isCanonical(node.children[2]);
        case Expression::OpaqueValue:
            return isCanonical(node.children[0]);
        default:
            return false;
        }
    }
    int makeCanonical(int value) { return isCanonical(value) ? value : wrapExpression(Expression::CanonicalNumber, program.expressions[value].type, value); }

    int makeLinkedNumber()
    {
        Expression node;
        node.kind = Expression::LinkedNumber;
        node.type = Int32;
        node.bits = random.pick(8);
        return addExpression(node);
    }
    int makeLeaf(TypeKind type)
    {
        if (type == Int32 && random.chance(4))
            return makeLinkedNumber();
        int variable = random.chance(constantMode == 2 ? 30 : 75) ? variableOfType(type) : -1;
        return variable >= 0 ? makeVariable(variable) : makeConstant(type, constantBits(type));
    }
    int makeVariableLeaf(TypeKind type)
    {
        int variable = variableOfType(type);
        return variable >= 0 ? makeVariable(variable) : makeConstant(type, constantBits(type));
    }

    MemoryPlace choosePlace(unsigned bytes, unsigned region, int depth)
    {
        MemoryPlace place;
        place.region = static_cast<uint8_t>(region);
        if (region == InstanceWordsRegion) {
            place.offset = static_cast<int32_t>(random.pick(regionSize / 8) * 8 + (bytes < 8 && random.chance(20) ? random.pick(9 - bytes) : 0));
            return place;
        }
        unsigned size = sizeOfRegion(region);
        place.usesRootHeap = region == RootRegion || (region != ImmutableRegion && region != SlotRegion && region != ArgumentsRegion && region != ConstantDataRegion && region != SumsRegion && region != InstanceWordsRegion && random.chance(10));
        unsigned mode = random.pick(10);
        if (region == FieldsRegion) {
            if (mode < 6) {
                place.mode = random.chance(20) ? NarrowReference : FixedOffset;
                unsigned field = random.pick(size / 8);
                place.offset = static_cast<int32_t>(field * 8 + (bytes < 8 && random.chance(30) ? random.pick(9 - bytes) : 0));
                return place;
            }
        } else if (mode < 5) {
            place.mode = random.chance(20) && region < InstanceRegion ? NarrowReference : FixedOffset;
            place.offset = static_cast<int32_t>(random.chance(60) ? random.pick(size / bytes) * bytes : random.pick(size - bytes + 1));
            return place;
        }
        switch (random.pick(7)) {
        case 0:
            place.mode = SignExtendedIndex;
            break;
        case 1:
            place.mode = WideIndex;
            break;
        case 2:
            place.mode = MultipliedIndex;
            break;
        case 3:
        case 4:
            place.mode = SteppedIndex;
            break;
        case 5:
            place.mode = SumIndex;
            break;
        default:
            place.mode = ZeroExtendedIndex;
            break;
        }
        place.scale = static_cast<uint8_t>(random.chance(60) ? (bytes == 8 ? 3 : bytes == 4 ? 2
                                                                       : bytes == 2         ? 1
                                                                                            : 0)
                                                             : random.pick(4));
        if (region == FieldsRegion) {
            place.scale = 3;
            if (place.mode == SumIndex)
                place.mode = ZeroExtendedIndex;
        }
        unsigned unit = place.mode == MultipliedIndex ? 8u : place.mode == SumIndex ? 1u
                                                                                    : 1u << place.scale;
        if (place.mode == MultipliedIndex)
            place.scale = 8;
        unsigned count = 4;
        while (count * 2 * unit <= size / 2)
            count *= 2;
        place.mask = count - 1;
        unsigned span = (count - 1) * unit + bytes;
        unsigned room = size - span;
        place.offset = region == FieldsRegion ? static_cast<int32_t>(random.pick(room / 8 + 1) * 8) : static_cast<int32_t>(random.pick(room + 1));
        if (place.mode == SteppedIndex) {
            place.step = static_cast<int32_t>(random.pick(5)) - 2;
            place.offset -= place.step * static_cast<int32_t>(unit);
        } else if (place.mode == SumIndex) {
            static const int32_t steps[] = { 1, -1, 8, -8, 4096, -4096, 32760, 65536, 0x7fff0000, -0x7fff0000, 100000 };
            place.step = steps[random.pick(std::size(steps))];
            place.offset -= place.step;
        }
        bool wide = place.mode == WideIndex || place.mode == MultipliedIndex || place.mode == SumIndex;
        place.index = makeExpression(wide ? Int64 : Int32, depth - 1);
        return place;
    }
    unsigned chooseRegionToRead()
    {
        unsigned choice = random.pick(10);
        unsigned region = choice < 3 ? FieldsRegion : choice < 5 ? BytesRegion
            : choice < 6                                         ? ImmutableRegion
            : choice < 7                                         ? RootRegion
            : choice < 8                                         ? InstanceRegion
            : choice < 9                                         ? (random.chance(60) ? SlotRegion : InstanceWordsRegion)
            : random.chance(50)                                  ? ArgumentsRegion
            : random.chance(50)                                  ? ConstantDataRegion
                                                                 : SumsRegion;
        if ((region == SlotRegion && !program.usesSlot) || (region == ArgumentsRegion && !program.usesArguments))
            return FieldsRegion;
        return region;
    }
    unsigned chooseRegionToWrite()
    {
        unsigned choice = random.pick(10);
        unsigned region = choice < 4 ? FieldsRegion : choice < 6 ? BytesRegion
            : choice < 7                                         ? RootRegion
            : choice < 8                                         ? InstanceRegion
            : choice < 9                                         ? (random.chance(60) ? SlotRegion : InstanceWordsRegion)
                                                                 : ArgumentsRegion;
        if ((region == SlotRegion && !program.usesSlot) || (region == ArgumentsRegion && !program.usesArguments))
            return FieldsRegion;
        return region;
    }
    int makeLoad(TypeKind type, int depth)
    {
        Expression node;
        node.kind = Expression::MemoryLoad;
        node.type = type;
        node.opcode = Load;
        if (type == Int32 && random.chance(45)) {
            static const Opcode opcodes[] = { Load8Z, Load8S, Load16Z, Load16S };
            node.opcode = opcodes[random.pick(4)];
        }
        unsigned bytes = node.opcode == Load8Z || node.opcode == Load8S ? 1 : node.opcode == Load16Z || node.opcode == Load16S ? 2
            : type == Int32 || type == Float                                                                                   ? 4
                                                                                                                               : 8;
        MemoryPlace place = choosePlace(bytes, chooseRegionToRead(), depth);
        node.place = place;
        return addExpression(node);
    }

    Opcode relationForType(TypeKind type)
    {
        static const Opcode integerOpcodes[] = { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, Above, Below, AboveEqual, BelowEqual };
        static const Opcode numbers[] = { Equal, NotEqual, LessThan, GreaterThan, LessEqual, GreaterEqual, EqualOrUnordered };
        return type == Float || type == Double ? numbers[random.pick(std::size(numbers))] : integerOpcodes[random.pick(std::size(integerOpcodes))];
    }

    int makeBitTest(TypeKind type, int value)
    {
        int bit = makeBinary(BitAnd, value, makeConstant(type, 1));
        return type == Int32 ? bit : makeBinary(random.chance(50) ? NotEqual : Equal, bit, makeConstant(type, 0));
    }
    int makeComparison(int depth)
    {
        TypeKind type = anyType();
        switch (random.pick(14)) {
        case 0:
        case 1: {
            TypeKind otherType = integerType();
            int bits = makeBinary(BitAnd, makeExpression(otherType, depth - 1), random.chance(70) ? makeConstant(otherType, constantBits(otherType)) : makeExpression(otherType, depth - 1));
            if (random.chance(60))
                return makeBinary(random.chance(50) ? Equal : NotEqual, bits, makeConstant(otherType, random.chance(80) ? 0 : constantBits(otherType)));
            return otherType == Int32 ? bits : makeBinary(NotEqual, bits, makeConstant(otherType, 0));
        }
        case 2: {
            static const Opcode opcodes[] = { Load8Z, Load8S, Load16Z, Load16S };
            Expression node;
            node.kind = Expression::MemoryLoad;
            node.type = Int32;
            node.opcode = opcodes[random.pick(4)];
            unsigned bytes = node.opcode == Load8Z || node.opcode == Load8S ? 1 : 2;
            node.place = choosePlace(bytes, chooseRegionToRead(), depth);
            int left = addExpression(node);
            uint64_t third = random.chance(60) ? static_cast<uint64_t>(static_cast<int64_t>(random.pick(520)) - 260) : constantBits(Int32);
            if (random.chance(40))
                return makeBinary(random.chance(50) ? Equal : NotEqual, makeBinary(BitAnd, left, makeConstant(Int32, random.chance(50) ? 1ull << random.pick(17) : constantBits(Int32))), makeConstant(Int32, random.chance(80) ? 0 : third));
            return makeBinary(relationForType(Int32), left, makeConstant(Int32, third));
        }
        case 3: {
            int first = makeComparison(depth - 1), right = makeComparison(depth - 1);
            if (program.expressions[first].type != program.expressions[right].type)
                return first;
            return makeBinary(random.chance(50) ? BitAnd : BitOr, first, right);
        }
        case 4: {
            TypeKind otherType = integerType();
            if (random.chance(70))
                return makeBitTest(otherType, makeBinary(random.chance(70) ? ZShr : SShr, makeExpression(otherType, depth - 1), makeExpression(Int32, depth - 1)));
            if (random.chance(40))
                return makeBitTest(otherType, makeBinary(Shl, makeExpression(otherType, depth - 1), makeExpression(Int32, depth - 1)));
            if (random.chance(50))
                return makeBitTest(Int64, makeBinary(UMulHigh, makeExpression(Int64, depth - 1), makeExpression(Int64, depth - 1)));
            return makeBitTest(otherType, makeBinary(UDiv, makeExpression(otherType, depth - 1), makeBinary(BitOr, makeBinary(BitAnd, makeLeaf(otherType), makeConstant(otherType, 0x7f)), makeConstant(otherType, 1))));
        }
        case 5: {
            if (random.chance(20))
                return makeBinary(random.chance(80) ? (random.chance(50) ? Equal : NotEqual) : relationForType(Int32), random.chance(60) ? makeLoad(Int32, depth) : makeExpression(Int32, depth - 1), makeLinkedNumber());
            TypeKind otherType = integerType();
            return makeBinary(random.chance(50) ? Below : BelowEqual, makeBinary(Sub, makeExpression(otherType, depth - 1), makeConstant(otherType, constantBits(otherType))), makeConstant(otherType, constantBits(otherType)));
        }
        case 6: {
            int value = makeExpression(Int64, depth - 1);
            switch (random.pick(5)) {
            case 0:
                return makeBinary(random.chance(50) ? AboveEqual : Below, value, makeConstant(Int64, 0xfffe000000000000ull));
            case 1:
                return makeBinary(random.chance(50) ? Equal : NotEqual, makeBinary(BitAnd, value, makeConstant(Int64, 0xfffe000000000002ull)), makeConstant(Int64, 0));
            case 2:
                return makeBinary(random.chance(50) ? Equal : NotEqual, makeBinary(BitAnd, value, makeConstant(Int64, ~8ull)), makeConstant(Int64, 2));
            case 3:
                return makeBinary(random.chance(50) ? Equal : NotEqual, makeBinary(BitAnd, value, makeConstant(Int64, 0xfffe000000000000ull)), makeConstant(Int64, 0));
            default:
                return makeBinary(Equal, makeBinary(BitXor, value, makeConstant(Int64, 6)), makeConstant(Int64, random.pick(2)));
            }
        }
        case 7: {
            return makeOverflowTest(makeWideSum(depth));
        }
        default:
            return makeBinary(relationForType(type), makeExpression(type, depth - 1), random.chance(45) ? makeConstant(type, constantBits(type)) : makeExpression(type, depth - 1));
        }
    }
    int makeWideSum(int depth)
    {
        int left = makeUnary(SExt32, makeExpression(Int32, depth - 1));
        int right = random.chance(40) ? makeConstant(Int64, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32))))) : makeUnary(random.chance(85) ? SExt32 : ZExt32, makeExpression(Int32, depth - 1));
        return random.chance(85) || program.expressions[right].kind != Expression::Constant ? makeBinary(random.chance(60) ? Add : Sub, left, right) : makeBinary(Sub, right, left);
    }
    int makeOverflowTest(int sum)
    {
        switch (random.pick(6)) {
        case 0:
            return makeBinary(random.chance(50) ? Equal : NotEqual, makeUnary(SExt32, makeUnary(Trunc, sum)), sum);
        case 1:
            return makeBinary(random.chance(50) ? Equal : NotEqual, sum, makeUnary(SExt32, makeUnary(Trunc, sum)));
        case 2:
            return makeBinary(random.chance(50) ? GreaterThan : LessEqual, sum, makeConstant(Int64, random.chance(70) ? 0x7fffffff : constantBits(Int64)));
        case 3:
            return makeBinary(random.chance(50) ? LessThan : GreaterEqual, sum, makeConstant(Int64, random.chance(50) ? 0xffffffff80000000ull : random.chance(50) ? 0x80000000ull
                                                                                                                                                                  : constantBits(Int64)));
        case 4:
            return makeBinary(random.chance(50) ? Above : BelowEqual, makeBinary(Add, sum, makeConstant(Int64, 0x80000000ull)), makeConstant(Int64, 0xffffffffull));
        default:
            return makeBinary(relationForType(Int64), sum, makeConstant(Int64, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32))))));
        }
    }
    int lastPredicate { -1 };
    bool isTiny { false };
    std::vector<int> sharedExpressions;
    std::vector<MemoryPlace> sharedPlaces;
    void makeSharedExpressions()
    {
        unsigned count = 2 + random.pick(4);
        for (unsigned i = 0; i < count; ++i) {
            TypeKind type = integerType();
            if (withNumbers && random.chance(12)) {
                int variable = 5 + static_cast<int>(random.pick(3));
                sharedExpressions.push_back(makeBinary(relationForType(Double), makeVariable(variable), random.chance(60) ? makeConstant(Double, constantBits(Double)) : makeVariable(5 + static_cast<int>(random.pick(3)))));
                continue;
            }
            int value;
            if (withMemory && random.chance(45)) {
                Expression node;
                node.kind = Expression::MemoryLoad;
                node.opcode = Load;
                node.type = type;
                unsigned bytes = type == Int32 ? 4 : 8;
                if (type == Int32 && random.chance(50)) {
                    static const Opcode opcodes[] = { Load8Z, Load8S, Load16Z, Load16S };
                    node.opcode = opcodes[random.pick(4)];
                    bytes = node.opcode == Load8Z || node.opcode == Load8S ? 1 : 2;
                }
                node.place.region = static_cast<uint8_t>(random.chance(60) ? FieldsRegion : random.chance(50) ? BytesRegion
                                                                                                              : InstanceRegion);
                node.place.offset = static_cast<int32_t>(random.pick(16) * 8);
                sharedPlaces.push_back(node.place);
                (void)bytes;
                value = addExpression(node);
            } else
                value = makeVariable(random.chance(50) && variableOfType(type) >= 0 ? variableOfType(type) : type == Int64 ? 1 + static_cast<int>(random.pick(4))
                                                                                                                           : fuelVariable);
            type = program.expressions[value].type;
            auto smallConstant = [&] { return makeConstant(type, random.chance(60) ? random.pick(5) : constantBits(type)); };
            switch (random.pick(5)) {
            case 0:
                sharedExpressions.push_back(makeBinary(random.chance(50) ? Equal : NotEqual, makeBinary(BitAnd, value, makeConstant(type, random.chance(70) ? 1ull << random.pick(type == Int32 ? 32 : 64) : constantBits(type))), makeConstant(type, 0)));
                break;
            case 1:
                sharedExpressions.push_back(value);
                break;
            default:
                sharedExpressions.push_back(makeBinary(relationForType(type), value, smallConstant()));
                break;
            }
            if (random.chance(40))
                sharedExpressions.push_back(makeBinary(relationForType(type), value, smallConstant()));
        }
    }
    void appendTinyStatement(int block)
    {
        unsigned choice = random.pick(100);
        if (choice < 45) {
            int variable = anyVariable();
            TypeKind type = program.variableTypes[variable];
            int value;
            if (type == Int32 && random.chance(35)) {
                int predecessor = sharedExpressions[random.pick(sharedExpressions.size())];
                value = program.expressions[predecessor].type == Int32 ? predecessor : makeUnary(Trunc, predecessor);
            } else if (type == Int64 && random.chance(35))
                value = makeBinary(Add, makeUnary(ZExt32, makeLeaf(Int32)), makeConstant(Int64, 0xfffe000000000000ull));
            else if (random.chance(60))
                value = makeConstant(type, random.chance(60) ? random.pick(5) : constantBits(type));
            else
                value = makeExpression(type, 1);
            assign(block, variable, value);
        } else if (choice < 70 && sharedPlaces.size()) {
            Statement statement;
            statement.kind = Statement::MemoryStore;
            statement.access = random.chance(40) ? Store8 : Store;
            TypeKind type = statement.access == Store8 ? Int32 : integerType();
            statement.expression = random.chance(60) ? makeConstant(type, random.pick(5)) : makeLeaf(type);
            statement.place = sharedPlaces[random.pick(sharedPlaces.size())];
            program.blocks[block].statements.push_back(statement);
        } else if (choice < 85 && withCalls) {
            Statement statement;
            statement.kind = Statement::PatchCall;
            statement.patch = newPatch(random.chance(50) ? CallPatch : random.chance(50) ? StubPatch
                                                                                         : InlinePatch);
            program.blocks[block].statements.push_back(statement);
        }
    }
    int makePredicate(int depth)
    {
        if (isTiny && random.chance(75)) {
            int condition = sharedExpressions[random.pick(sharedExpressions.size())];
            if (random.chance(12))
                condition = makeBinary(Equal, condition, makeConstant(program.expressions[condition].type, 0));
            else if (random.chance(6) && isComparison(program.expressions[condition].opcode) && program.expressions[condition].kind == Expression::BinaryOperation)
                condition = makeBinary(BitXor, condition, makeConstant(Int32, 1));
            else if (random.chance(25)) {
                int variable = anyVariable();
                if (program.variableTypes[variable] == Int32 || program.variableTypes[variable] == Int64)
                    return random.chance(50) ? makeVariable(variable) : makeBinary(relationForType(program.variableTypes[variable]), makeVariable(variable), makeConstant(program.variableTypes[variable], random.chance(60) ? random.pick(5) : constantBits(program.variableTypes[variable])));
            }
            return condition;
        }
        if (random.chance(8)) {
            TypeKind type = integerType();
            int value = makeExpression(type, depth);
            return value;
        }
        if (random.chance(sharePercent) && lastPredicate >= 0)
            return lastPredicate;
        int condition = makeComparison(depth);
        lastPredicate = condition;
        if (random.chance(8))
            condition = makeBinary(BitXor, makeUnary(random.chance(50) ? ZExt32 : SExt32, makeBinary(NotEqual, condition, makeConstant(program.expressions[condition].type, 0))), makeConstant(Int64, 1));
        if (random.chance(6))
            condition = makeBinary(Equal, condition, makeConstant(program.expressions[condition].type, 0));
        return condition;
    }

    unsigned sharePercent { 8 };
    bool readsBehindPointer(int value) const
    {
        if (value < 0)
            return false;
        const Expression& node = program.expressions[value];
        if (node.kind == Expression::MemoryLoad && (node.place.baseVariable >= 0 || readsBehindPointer(node.place.index)))
            return true;
        for (int condition : node.children) {
            if (readsBehindPointer(condition))
                return true;
        }
        return false;
    }
    int makeExpression(TypeKind type, int depth)
    {
        int expression = makeNewExpression(type, depth);
        return expression;
    }
    int makeNewExpression(TypeKind type, int depth)
    {
        if (depth <= 0)
            return makeLeaf(type);
        if (random.chance(sharePercent) && program.expressions.size()) {
            for (unsigned tries = 0; tries < 6; ++tries) {
                unsigned back = 1 + random.pick(40);
                if (back > program.expressions.size())
                    break;
                int value = static_cast<int>(program.expressions.size() - back);
                const Expression& node = program.expressions[value];
                if (node.type == type && node.kind != Expression::Constant && node.kind != Expression::Variable && node.kind != Expression::ChillOperation && !readsBehindPointer(value))
                    return value;
            }
        }
        if (random.chance(18))
            return makeLeaf(type);
        if (withMemory && random.chance(loadsOften ? 30 : 12))
            return makeLoad(type, depth);
        if (random.chance(4))
            return wrapExpression(Expression::OpaqueValue, type, makeExpression(type, depth - 1));
        if (random.chance(9))
            return makeSelect(makePredicate(depth - 1), makeExpression(type, depth - 1), makeExpression(type, depth - 1));
        switch (type) {
        case Int32:
            switch (random.pick(20)) {
            case 0:
                return makeUnary(Trunc, makeExpression(Int64, depth - 1));
            case 1:
                return makeUnary(random.chance(50) ? SExt8 : SExt16, makeExpression(Int32, depth - 1));
            case 2:
                return makeUnary(Clz, makeExpression(Int32, depth - 1));
            case 3:
                return makeUnary(Neg, makeExpression(Int32, depth - 1));
            case 4: {
                int condition = makeComparison(depth - 1);
                return program.expressions[condition].type == Int32 ? condition : makeUnary(Trunc, condition);
            }
            case 5:
                return wrapExpression(Expression::LowHalf, Int32, makeExpression(Int64, depth - 1));
            case 6:
                return withNumbers ? wrapExpression(Expression::BitCast, Int32, makeCanonical(makeExpression(Float, depth - 1))) : makeLeaf(type);
            case 7:
                return makeUnary(Trunc, makeBinary(ZShr, makeExpression(Int64, depth - 1), makeConstant(Int32, 32)));
            case 8:
                return makeUnary(PopCount, makeExpression(random.chance(50) ? Int32 : Int32, depth - 1));
            case 9:
                return makeRotation(Int32, depth);
            default:
                return makeBinaryExpression(Int32, depth);
            }
        case Int64:
            switch (random.pick(20)) {
            case 0:
            case 1:
                return makeUnary(random.chance(50) ? SExt32 : ZExt32, makeExpression(Int32, depth - 1));
            case 2:
                return makeUnary(random.chance(50) ? Clz : PopCount, makeExpression(Int64, depth - 1));
            case 3:
                return makeUnary(Neg, makeExpression(Int64, depth - 1));
            case 4:
                return withNumbers ? wrapExpression(Expression::BitCast, Int64, makeCanonical(makeExpression(Double, depth - 1))) : makeLeaf(type);
            case 5:
                return makeBinary(BitOr, makeUnary(ZExt32, makeExpression(Int32, depth - 1)), makeConstant(Int64, 0xfffe000000000000ull));
            case 6:
                return makeBinary(random.chance(50) ? Add : Sub, makeExpression(Int64, depth - 1), makeConstant(Int64, 0x2000000000000ull));
            case 7:
                return makeBinary(Add, makeExpression(Int64, depth - 1), makeBinary(Shl, random.chance(60) ? makeUnary(random.chance(60) ? ZExt32 : SExt32, makeExpression(Int32, depth - 1)) : makeExpression(Int64, depth - 1), makeConstant(Int32, random.chance(80) ? random.pick(4) : random.pick(64))));
            case 8:
                if (random.chance(40))
                    return makeBinary(Shl, makeUnary(random.chance(60) ? ZExt32 : SExt32, makeExpression(Int32, depth - 1)), makeConstant(Int32, random.pick(64)));
                return makeWideSum(depth);
            case 9:
                return makeRotation(Int64, depth);
            case 10:
                return wrapExpression(Expression::NonNegativeExtension, Int64, random.chance(50) ? makeBinary(BitAnd, makeExpression(Int32, depth - 1), makeConstant(Int32, 0x7fffffffu >> random.pick(31))) : makeBinary(ZShr, makeExpression(Int32, depth - 1), makeConstant(Int32, 1 + random.pick(31))));
            case 11:
                return makeBinary(random.chance(50) ? MulHigh : UMulHigh, makeExpression(Int64, depth - 1), random.chance(50) ? makeConstant(Int64, constantBits(Int64)) : makeExpression(Int64, depth - 1));
            default:
                return makeBinaryExpression(Int64, depth);
            }
        case Double:
            switch (random.pick(14)) {
            case 0:
            case 1:
                return makeUnary(IToD, makeExpression(integerType(), depth - 1));
            case 2:
                return makeUnary(Neg, makeExpression(Double, depth - 1));
            case 3:
                return makeUnary(Abs, makeExpression(Double, depth - 1));
            case 4:
                return makeUnary(Sqrt, makeExpression(Double, depth - 1));
            case 5:
                return makeUnary(FloatToDouble, makeExpression(Float, depth - 1));
            case 6:
                return wrapExpression(Expression::BitCast, Double, makeExpression(Int64, depth - 1));
            case 7:
                if (program.usesHostFeatures) {
                    static const Opcode relations[] = { Ceil, Floor, FTrunc };
                    return makeUnary(relations[random.pick(3)], makeExpression(Double, depth - 1));
                }
                [[fallthrough]];
            case 8:
                return makeBinary(random.chance(50) ? FMin : FMax, makeExpression(Double, depth - 1), makeExpression(Double, depth - 1));
            default:
                return makeArithmetic(Double, depth);
            }
        case Float:
            switch (random.pick(10)) {
            case 0:
            case 1:
                return makeUnary(DoubleToFloat, makeExpression(Double, depth - 1));
            case 2:
                return makeUnary(Neg, makeExpression(Float, depth - 1));
            case 3:
                return makeUnary(Abs, makeExpression(Float, depth - 1));
            case 4:
                return makeUnary(Sqrt, makeExpression(Float, depth - 1));
            case 5:
                return wrapExpression(Expression::BitCast, Float, makeExpression(Int32, depth - 1));
            case 6:
                if (program.usesHostFeatures && random.chance(30)) {
                    static const Opcode relations[] = { Ceil, Floor, FTrunc };
                    return makeUnary(relations[random.pick(3)], makeExpression(Float, depth - 1));
                }
                return makeBinary(random.chance(50) ? FMin : FMax, makeExpression(Float, depth - 1), makeExpression(Float, depth - 1));
            default:
                return makeArithmetic(Float, depth);
            }
        default:
            abort();
        }
    }
    int makeRotation(TypeKind type, int depth)
    {
        unsigned width = type == Int32 ? 32 : 64;
        int value = makeExpression(type, depth - 1);
        unsigned count = random.pick(width);
        if (random.chance(50))
            return makeBinary(RotR, value, makeConstant(Int32, random.chance(90) ? count : count + width));
        if (program.expressions[value].kind != Expression::Variable && program.expressions[value].kind != Expression::Constant)
            value = makeVariableLeaf(type);
        return makeBinary(random.chance(50) ? BitOr : BitXor, makeBinary(Shl, value, makeConstant(Int32, count)), makeBinary(ZShr, value, makeConstant(Int32, random.chance(85) ? (width - count) & (width - 1) : random.pick(width))));
    }
    int makeArithmetic(TypeKind type, int depth)
    {
        static const Opcode opcodes[] = { Add, Sub, Mul, Div, Add, Mul };
        Opcode opcode = opcodes[random.pick(std::size(opcodes))];
        int left = makeExpression(type, depth - 1);
        int right = random.chance(25) ? makeConstant(type, constantBits(type)) : makeExpression(type, depth - 1);
        return random.chance(85) ? makeBinary(opcode, left, right) : makeBinary(opcode, right, left);
    }
    int makeBinaryExpression(TypeKind type, int depth)
    {
        unsigned choice = random.pick(100);
        if (choice < 12) {
            static const Opcode opcodes[] = { Shl, SShr, ZShr };
            unsigned width = type == Int32 ? 32 : 64;
            int amount = random.chance(65) ? makeConstant(Int32, random.chance(92) ? random.pick(width) : random.pick(width + 8)) : makeExpression(Int32, depth - 1);
            return makeBinary(opcodes[random.pick(3)], makeExpression(type, depth - 1), amount);
        }
        if (choice < 17) {
            static const Opcode opcodes[] = { Div, UDiv, Mod, UMod };
            unsigned k = random.pick(4);
            int left = makeExpression(type, depth - 1);
            if (random.chance(50)) {
                uint64_t third = constantBits(type);
                bool isMinusOne = third == truncateToType(type, ~0ull);
                if (random.chance(50) && (k == 0 || k == 2)) {
                    Expression node;
                    node.kind = Expression::ChillOperation;
                    node.opcode = opcodes[k];
                    node.type = type;
                    node.children[0] = left;
                    node.children[1] = makeConstant(type, third);
                    return addExpression(node);
                }
                if (!third || isMinusOne)
                    third = 3 + random.pick(1000);
                return makeBinary(opcodes[k], left, makeConstant(type, third));
            }
            if (k == 0 || k == 2) {
                Expression node;
                node.kind = Expression::ChillOperation;
                node.opcode = opcodes[k];
                node.type = type;
                node.children[0] = left;
                node.children[1] = makeExpression(type, depth - 1);
                return addExpression(node);
            }
            int right = makeBinary(BitOr, makeBinary(BitAnd, makeExpression(type, depth - 1), makeConstant(type, 0x7f)), makeConstant(type, 1));
            return makeBinary(opcodes[k], left, right);
        }
        static const Opcode opcodes[] = { Add, Add, Add, Sub, Sub, Mul, BitAnd, BitAnd, BitOr, BitXor };
        Opcode opcode = opcodes[random.pick(std::size(opcodes))];
        int left = makeExpression(type, depth - 1);
        int right = random.chance(38) ? makeConstant(type, constantBits(type)) : random.chance(8) ? left
                                                                                                  : makeExpression(type, depth - 1);
        if (right == left && program.expressions[left].kind != Expression::Variable)
            right = makeLeaf(type);
        return random.chance(85) ? makeBinary(opcode, left, right) : makeBinary(opcode, right, left);
    }

    void assign(int block, int variable, int expression)
    {
        if (program.mayHoldAnyBits[variable])
            expression = makeCanonical(expression);
        Statement statement;
        statement.kind = Statement::Assignment;
        statement.variable = variable;
        statement.expression = expression;
        program.blocks[block].statements.push_back(statement);
    }
    int anyVariable()
    {
        for (;;) {
            int variable = firstFreeVariable + static_cast<int>(random.pick(program.variableTypes.size() - firstFreeVariable));
            if (!program.pointerKinds[variable])
                return variable;
        }
    }

    void chooseEffects(PatchDescription& description)
    {
        switch (random.pick(6)) {
        case 0:
            description.reads = 1;
            break;
        case 1:
            description.writes = static_cast<uint8_t>(2 + random.pick(5));
            description.writesLocalState = random.chance(20);
            break;
        case 2:
            description.reads = 1;
            description.writes = 1;
            description.exitsSideways = random.chance(50);
            break;
        case 3:
            description.writesLocalState = true;
            break;
        case 4:
            description.reads = 1;
            description.writes = static_cast<uint8_t>(2 + random.pick(5));
            break;
        default:
            break;
        }
        if (description.writes >= 2 && description.writes - 2 == ImmutableRegion)
            description.writes = 2 + BytesRegion;
        if (description.writes >= 2 && description.writes - 2 == RootRegion)
            description.writes = 1;
        description.isControlDependent = random.chance(30);
        if (readPercent && description.writes >= 2 && random.chance(60)) {
            static const uint8_t lists[] = { 60, 61, 62, 2 + FieldsRegion, 2 + InstanceRegion };
            description.writes = lists[random.pick(std::size(lists))];
        }
    }
    PatchOperand makeOperand(TypeKind type, uint8_t kind)
    {
        PatchOperand operand;
        operand.expression = makeCanonical(random.chance(20) ? makeConstant(type, constantBits(type)) : random.chance(70) ? makeVariableLeaf(type)
                                                                                                                          : makeExpression(type, 2));
        if (program.expressions[operand.expression].kind == Expression::LinkedNumber)
            operand.expression = makeConstant(type, constantBits(type));
        operand.constraint = kind;
        return operand;
    }
    void chooseOperands(PatchDescription& description, unsigned most)
    {
        unsigned count = random.pick(most + 1);
        unsigned inRegisters[2] = { 0, 0 };
        unsigned nextStack = 0, nextGP = 0, nextFP = 0;
        for (unsigned i = 0; i < count; ++i) {
            TypeKind type = anyType();
            bool isFloat = type == Float || type == Double;
            PatchOperand operand;
            bool isCall = description.kind == CallPatch || description.kind == ThrowingPatch || description.kind == ExitingPatch;
            if (isCall) {
                if (random.chance(15) && description.operands.size()) {
                    operand = makeOperand(type, random.chance(50) ? ValueRep::ColdAny : ValueRep::LateColdAny);
                } else if (random.chance(20) || (isFloat ? nextFP >= 4 : nextGP >= 5)) {
                    operand = makeOperand(type, ValueRep::StackArgument);
                    operand.location = nextStack++;
                } else {
                    operand = makeOperand(type, ValueRep::Register);
                    operand.location = 1000 + (isFloat ? nextFP++ : nextGP++);
                }
            } else {
                static const uint8_t kinds[] = { ValueRep::WarmAny, ValueRep::ColdAny, ValueRep::LateColdAny, ValueRep::SomeRegister, ValueRep::SomeLateRegister, ValueRep::Register, ValueRep::Register };
                uint8_t kind = kinds[random.pick(std::size(kinds))];
                if (description.kind == KeepAlivePatch)
                    kind = ValueRep::ColdAny;
                if (description.kind == StubPatch && random.chance(70))
                    kind = ValueRep::Register;
                if ((kind == ValueRep::SomeRegister || kind == ValueRep::SomeLateRegister || kind == ValueRep::Register) && ++inRegisters[isFloat] > 3)
                    kind = ValueRep::WarmAny;
                operand = makeOperand(type, kind);
                operand.location = random.pick(32);
            }
            if (i && random.chance(12) && program.expressions[description.operands.back().expression].type == type && !description.operands.back().isSlotAddress)
                operand.expression = description.operands.back().expression;
            description.operands.push_back(operand);
        }
    }
    void chooseResults(PatchDescription& description, unsigned most)
    {
        unsigned count = random.pick(most + 1);
        bool isCall = description.kind == CallPatch || description.kind == ThrowingPatch || description.kind == ExitingPatch;
        bool usedFP = false;
        unsigned usedGP = 0;
        for (unsigned i = 0; i < count; ++i) {
            PatchResult result;
            result.variable = anyVariable();
            for (const PatchResult& other : description.results) {
                if (other.variable == result.variable)
                    result.variable = -1;
            }
            if (result.variable < 0)
                continue;
            result.type = program.variableTypes[result.variable];
            bool isFloat = result.type == Float || result.type == Double;
            if (isCall || (description.kind == StubPatch && random.chance(70))) {
                if (isFloat ? usedFP : usedGP >= (isCall ? 2 : 1))
                    continue;
                result.constraint = ValueRep::Register;
                result.location = 2000 + (isFloat ? 0 : usedGP);
                usedFP |= isFloat;
                usedGP += !isFloat;
            } else {
                static const uint8_t kinds[] = { ValueRep::SomeRegister, ValueRep::SomeEarlyRegister, ValueRep::Register };
                result.constraint = kinds[random.pick(3)];
                result.location = random.pick(32);
            }
            description.results.push_back(result);
        }
    }
    int newPatch(PatchKind kind)
    {
        if (kind != ThrowingPatch && kind != BranchingPatch && kind != SuspensionPatch && random.chance(reusePercent)) {
            std::vector<int> candidates;
            for (unsigned i = 0; i < program.patches.size(); ++i) {
                if (program.patches[i].kind == kind && !isCheckPatch[i])
                    candidates.push_back(static_cast<int>(i));
            }
            if (candidates.size())
                return candidates[random.pick(candidates.size())];
        }
        PatchDescription description;
        description.kind = kind;
        description.salt = random.next();
        switch (kind) {
        case CallPatch:
        case ThrowingPatch:
        case ExitingPatch:
            chooseOperands(description, 8);
            chooseResults(description, random.chance(80) ? 1 : 2);
            description.reads = 1;
            description.writes = random.chance(60) ? 1 : 0;
            description.mayCollect = true;
            description.exitsSideways = true;
            description.isControlDependent = true;
            description.needsFrameRecord = random.chance(50);
            if (program.usesSlot && random.chance(15)) {
                description.readsSlot = true;
                PatchOperand operand;
                operand.expression = makeConstant(Int64, 0);
                operand.constraint = ValueRep::Register;
                operand.location = 1005;
                operand.isSlotAddress = true;
                description.operands.push_back(operand);
            }
            break;
        case SuspensionPatch: {
            unsigned count = 1 + random.pick(4);
            for (unsigned i = 0; i < count; ++i)
                description.operands.push_back(makeOperand(i < 2 && random.chance(70) ? Int64 : anyType(), ValueRep::LateColdAny));
            if (random.chance(60)) {
                PatchOperand operand = makeOperand(Int64, ValueRep::Register);
                operand.location = 1000;
                description.operands.insert(description.operands.begin(), operand);
            }
            description.kind = CallPatch;
            chooseResults(description, 2);
            description.kind = SuspensionPatch;
            for (PatchResult& result : description.results) {
                if (result.type == Float || result.type == Double) {
                    description.results.clear();
                    break;
                }
            }
            description.reads = 1;
            description.writes = random.chance(60) ? 1 : 0;
            description.mayCollect = description.exitsSideways = description.isControlDependent = description.needsFrameRecord = true;
            break;
        }
        case StubPatch:
            chooseOperands(description, 5);
            chooseResults(description, 1);
            chooseEffects(description);
            description.mayCollect = random.chance(40);
            description.earlyClobbers = random.chance(40) ? random.next() & random.next() : 0;
            description.lateClobbers = random.chance(70) ? (random.chance(50) ? random.next() : random.next() & random.next()) : 0;
            description.runsWithoutFrame = !description.mayCollect && random.chance(50);
            description.bringsOperands = random.chance(60);
            description.fillsRegisterFromResult = random.chance(25);
            description.constrainsResult = random.chance(30);
            description.needsFrameRecord = random.chance(30);
            break;
        case InlinePatch:
            chooseOperands(description, 6);
            chooseResults(description, 3);
            chooseEffects(description);
            description.writes = 0;
            description.writesLocalState = description.writesLocalState || random.chance(30);
            description.earlyClobbers = random.chance(30) ? random.next() & random.next() : 0;
            description.lateClobbers = random.chance(40) ? random.next() & random.next() : 0;
            description.numberOfGPScratches = static_cast<uint8_t>(random.pick(3));
            description.numberOfFPScratches = static_cast<uint8_t>(random.pick(2));
            description.runsWithoutFrame = random.chance(70);
            break;
        case PurePatch:
            chooseOperands(description, 3);
            for (PatchOperand& operand : description.operands) {
                if (operand.constraint == ValueRep::StackArgument)
                    operand.constraint = ValueRep::WarmAny;
            }
            chooseResults(description, 1);
            description.runsWithoutFrame = true;
            description.numberOfGPScratches = static_cast<uint8_t>(random.pick(2));
            description.lateClobbers = random.chance(30) ? random.next() & random.next() : 0;
            if (description.results.empty()) {
                description.kind = InlinePatch;
                description.writesLocalState = true;
                break;
            }
            if (readPercent && random.chance(50)) {
                static const uint8_t regions[] = { 2 + FieldsRegion, 2 + BytesRegion, 2 + InstanceRegion, 60, 61, 62 };
                description.reads = regions[random.pick(std::size(regions))];
                if (random.chance(70))
                    description.operands.resize(std::min<size_t>(description.operands.size(), 1));
            }
            if (random.chance(60)) {
                static const uint64_t salts[] = { 11, 13, 17 };
                description.salt = salts[random.pick(3)];
            }
            break;
        case KeepAlivePatch:
            chooseOperands(description, 3);
            break;
        case BranchingPatch:
            chooseOperands(description, 4);
            chooseEffects(description);
            description.writes = 0;
            description.writesLocalState = true;
            description.numberOfGPScratches = static_cast<uint8_t>(random.pick(2));
            description.lateClobbers = random.chance(40) ? random.next() & random.next() : 0;
            description.runsWithoutFrame = random.chance(60);
            description.numberOfSuccessors = 2;
            break;
        default:
            break;
        }
        if (program.seed >= firstSeedWithNumberedReads && (description.kind == StubPatch || description.kind == InlinePatch || description.kind == PurePatch))
            description.clearsUpperHalf = random.chance(30);
        program.patches.push_back(description);
        isCheckPatch.push_back(0);
        return static_cast<int>(program.patches.size()) - 1;
    }
    int makeCheckPatch()
    {
        if (random.chance(30)) {
            std::vector<int> candidates;
            for (unsigned i = 0; i < program.patches.size(); ++i) {
                if (isCheckPatch[i])
                    candidates.push_back(static_cast<int>(i));
            }
            if (candidates.size())
                return candidates[random.pick(candidates.size())];
        }
        PatchDescription description;
        description.kind = CallPatch;
        description.salt = random.next();
        for (unsigned count = random.pick(random.chance(50) ? 2 : 5); count; --count)
            description.operands.push_back(makeOperand(anyType(), ValueRep::ColdAny));
        program.patches.push_back(description);
        isCheckPatch.push_back(1);
        return static_cast<int>(program.patches.size()) - 1;
    }
    MemoryPlace choosePlaceBehind(int baseVar, unsigned bytes, bool field)
    {
        MemoryPlace place;
        place.region = field ? FieldsRegion : BytesRegion;
        place.mode = FixedOffset;
        place.baseVariable = baseVar;
        place.offset = static_cast<int32_t>(field ? random.pick(16) * 8 : random.pick(regionSize - bytes + 1));
        return place;
    }
    void appendTrappingLoad(int block)
    {
        bool narrow = random.chance(50);
        int& variable = narrow ? narrowVar : pointerVar;
        if (variable < 0) {
            variable = newVariable(narrow ? Int32 : Int64);
            program.pointerKinds[variable] = narrow ? 2 : 1;
        }
        Statement first;
        first.kind = Statement::PointerSelection;
        first.variable = variable;
        first.expression = random.chance(70) ? makeBinary(NotEqual, makeExpression(Int32, 1), makeConstant(Int32, random.next())) : makePredicate(1);
        if (program.expressions[first.expression].kind == Expression::Constant)
            return;
        program.blocks[block].statements.push_back(first);
        if (random.chance(20))
            appendStatement(block);
        Statement statement;
        statement.kind = Statement::TrappingLoad;
        statement.access = random.chance(70) ? Load8Z : random.chance(50) ? Load8S
                                                                          : Load16Z;
        statement.variable = random.chance(35) ? variableToWrite(Int32) : -1;
        statement.place = choosePlaceBehind(variable, 2, random.chance(70));
        statement.patch = makeTrapPatch();
        program.blocks[block].statements.push_back(statement);
        if (statement.variable >= 0 && random.chance(50) && withChecks) {
            Statement check;
            check.kind = Statement::ExitCheck;
            check.expression = makeBinary(random.chance(50) ? Equal : Above, makeVariable(statement.variable), makeConstant(Int32, random.pick(300)));
            check.patch = makeCheckPatch();
            program.blocks[block].statements.push_back(check);
        }
        for (unsigned count = random.pick(4); count; --count) {
            if (random.chance(30)) {
                int word = anyVariable();
                assign(block, word, makeExpression(program.variableTypes[word], 1));
                continue;
            }
            TypeKind type = anyType();
            unsigned bytes = type == Int32 || type == Float ? 4 : 8;
            if (random.chance(60)) {
                Expression node;
                node.kind = Expression::MemoryLoad;
                node.type = type;
                node.opcode = Load;
                node.place = choosePlaceBehind(variable, bytes, random.chance(70));
                int left = addExpression(node);
                int word = variableToWrite(type);
                if (word >= 0)
                    assign(block, word, left);
            } else {
                Statement otherStatement;
                otherStatement.kind = Statement::MemoryStore;
                otherStatement.access = Store;
                otherStatement.expression = makeCanonical(random.chance(40) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
                otherStatement.place = choosePlaceBehind(variable, bytes, random.chance(70));
                program.blocks[block].statements.push_back(otherStatement);
            }
        }
    }
    int makeTrapPatch()
    {
        PatchDescription description;
        description.kind = CallPatch;
        program.patches.push_back(description);
        isCheckPatch.push_back(2);
        return static_cast<int>(program.patches.size()) - 1;
    }
    unsigned heldPercent { 10 };
    void appendOverlappingStore(int block, const MemoryPlace& place, TypeKind type)
    {
        Statement statement;
        statement.kind = Statement::MemoryStore;
        statement.access = Store;
        statement.place = place;
        if (place.mode == FixedOffset && random.chance(30)) {
            unsigned bytes = type == Int32 || type == Float ? 4 : 8;
            statement.access = random.chance(50) ? Store8 : Store;
            TypeKind otherType = Int32;
            unsigned size = statement.access == Store8 ? 1 : 4;
            int currentBlock = place.offset + static_cast<int>(random.pick(bytes)) - (random.chance(30) ? 1 : 0);
            if (currentBlock >= 0 && currentBlock + size <= sizeOfRegion(place.region) && place.region != InstanceWordsRegion && (place.region != FieldsRegion || currentBlock / 8 == (currentBlock + static_cast<int>(size) - 1) / 8)) {
                statement.place.offset = currentBlock;
                statement.expression = random.chance(50) ? makeConstant(otherType, constantBits(otherType)) : makeLeaf(otherType);
                program.blocks[block].statements.push_back(statement);
                return;
            }
            statement.access = Store;
        }
        statement.expression = makeCanonical(random.chance(50) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
        program.blocks[block].statements.push_back(statement);
    }
    int makeLoadFrom(const MemoryPlace& place, TypeKind type)
    {
        Expression node;
        node.kind = Expression::MemoryLoad;
        node.type = type;
        node.opcode = Load;
        node.place = place;
        return addExpression(node);
    }
    void appendInterference(int block, const MemoryPlace& place, TypeKind type, unsigned percent)
    {
        if (!random.chance(percent))
            return;
        switch (random.pick(4)) {
        case 0:
            appendOverlappingStore(block, place, type);
            break;
        case 1: {
            int variable = variableToWrite(type);
            if (variable >= 0)
                assign(block, variable, makeLoadFrom(place, type));
            break;
        }
        default:
            appendStatement(block);
            break;
        }
    }
    void appendNeighboringAccesses(int block)
    {
        bool loads = random.chance(50);
        unsigned region = loads ? chooseRegionToRead() : chooseRegionToWrite();
        if (region == InstanceWordsRegion || region == SumsRegion)
            region = BytesRegion;
        int baseVar = -1;
        unsigned size = sizeOfRegion(region);
        int currentBlock = static_cast<int>(random.pick(size / 8) * 8);
        TypeKind type = anyType();
        for (unsigned count = 2 + random.pick(3); count; --count) {
            if (random.chance(15))
                type = anyType();
            unsigned bytes = type == Int32 || type == Float ? 4 : 8;
            Opcode opcode = loads ? Load : Store;
            if (type == Int32 && random.chance(15)) {
                opcode = loads ? (random.chance(50) ? Load8Z : Load16S) : random.chance(50) ? Store8
                                                                                            : Store16;
                bytes = opcode == Load8Z || opcode == Store8 ? 1 : 2;
            }
            MemoryPlace place;
            place.region = static_cast<uint8_t>(region);
            place.baseVariable = baseVar;
            place.offset = currentBlock;
            if (region == FieldsRegion && currentBlock / 8 != (currentBlock + static_cast<int>(bytes) - 1) / 8)
                place.offset = currentBlock / 8 * 8;
            if (place.offset < 0 || place.offset + static_cast<int>(bytes) > static_cast<int>(size))
                return;
            if (loads) {
                int variable = variableToWrite(type);
                if (variable < 0 || random.chance(30)) {
                    variable = newVariable(type);
                    program.pointerKinds[variable] = 4;
                }
                Expression node;
                node.kind = Expression::MemoryLoad;
                node.type = type;
                node.opcode = opcode;
                node.place = place;
                assign(block, variable, addExpression(node));
                if (program.pointerKinds[variable] == 4) {
                    Statement statement;
                    statement.kind = Statement::MemoryStore;
                    statement.access = Store;
                    statement.expression = makeCanonical(makeVariable(variable));
                    statement.place.region = InstanceRegion;
                    statement.place.offset = static_cast<int32_t>(random.pick(16) * 8);
                    deferredStatements.push_back(statement);
                }
            } else {
                Statement statement;
                statement.kind = Statement::MemoryStore;
                statement.access = opcode;
                statement.expression = makeCanonical(random.chance(40) ? makeConstant(type, constantBits(type)) : makeVariableLeaf(type));
                statement.place = place;
                program.blocks[block].statements.push_back(statement);
            }
            currentBlock += (random.chance(75) ? static_cast<int>(bytes) : random.chance(50) ? -static_cast<int>(bytes)
                                                                                             : 8);
        }
        for (const Statement& otherStatement : deferredStatements)
            program.blocks[block].statements.push_back(otherStatement);
        deferredStatements.clear();
    }
    std::vector<Statement> deferredStatements;
    void appendSplitUpdate(int block)
    {
        TypeKind type = integerType();
        unsigned bytes = type == Int32 ? 4 : 8;
        MemoryPlace place = choosePlace(bytes, chooseRegionToWrite(), 1);
        int target = newVariable(type), upper;
        program.pointerKinds[target] = 4;
        assign(block, target, makeLoadFrom(place, type));
        appendInterference(block, place, type, 35);
        static const Opcode opcodes[] = { Add, Sub, BitAnd, BitOr, BitXor, Add };
        Opcode opcode = opcodes[random.pick(6)];
        bool wide = type == Int32 && random.chance(35);
        TypeKind targetType = wide ? Int64 : type;
        upper = newVariable(targetType);
        program.pointerKinds[upper] = 4;
        int value = wide ? makeUnary(random.chance(80) ? SExt32 : ZExt32, makeVariable(target)) : makeVariable(target);
        int other = random.chance(50) ? makeConstant(targetType, constantBits(targetType)) : makeExpression(targetType, 1);
        assign(block, upper, random.chance(80) ? makeBinary(opcode, value, other) : makeBinary(opcode, other, value));
        appendInterference(block, place, type, 35);
        Statement statement;
        statement.kind = Statement::MemoryStore;
        statement.access = Store;
        statement.expression = wide ? makeUnary(Trunc, makeVariable(upper)) : makeVariable(upper);
        statement.place = place;
        program.blocks[block].statements.push_back(statement);
        int variable = variableToWrite(random.chance(50) ? type : targetType);
        if (variable >= 0 && random.chance(30))
            assign(block, variable, program.variableTypes[variable] == type && random.chance(50) ? makeVariable(target) : program.variableTypes[variable] == targetType ? makeVariable(upper)
                                                                                                                                                                        : makeVariable(target));
    }
    void appendSharedUses(int block)
    {
        TypeKind type = random.chance(80) ? integerType() : anyType();
        int expression;
        bool isFloat = type == Float || type == Double;
        switch (isFloat ? 5 : random.pick(9)) {
        case 0:
            expression = makeBinary(Shl, makeLeaf(type), makeConstant(Int32, random.chance(70) ? random.pick(4) : random.pick(64)));
            break;
        case 1:
            expression = type == Int64 ? makeUnary(random.chance(50) ? ZExt32 : SExt32, makeLeaf(Int32)) : makeUnary(Trunc, makeLeaf(Int64));
            break;
        case 2:
            expression = makeBinary(BitAnd, makeLeaf(type), random.chance(70) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
            break;
        case 3:
            expression = type == Int32 ? makeBinary(relationForType(Int32), makeLeaf(Int32), random.chance(50) ? makeConstant(Int32, constantBits(Int32)) : makeLeaf(Int32)) : makeWideSum(1);
            break;
        case 4:
            expression = makeBinary(random.chance(50) ? ZShr : SShr, makeLeaf(type), random.chance(50) ? makeLeaf(Int32) : makeConstant(Int32, random.pick(64)));
            break;
        case 5:
            expression = makeLoad(type, 1);
            break;
        case 6:
            expression = makeBinary(random.chance(50) ? Add : Sub, makeLeaf(type), random.chance(50) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
            break;
        case 7:
            expression = makeBinary(Mul, makeLeaf(type), random.chance(50) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
            break;
        default:
            expression = makeExpression(type, 2);
            break;
        }
        int target = newVariable(type, false);
        program.pointerKinds[target] = 4;
        if (program.expressions[expression].type != type)
            return;
        assign(block, target, expression);
        for (unsigned count = 2 + random.pick(2); count; --count) {
            if (random.chance(25))
                appendStatement(block);
            int value = makeVariable(target), other = -1;
            switch (isFloat ? 5 : random.pick(8)) {
            case 0:
                other = type == Int64 ? makeBinary(Add, makeLeaf(Int64), random.chance(50) ? value : makeBinary(Shl, value, makeConstant(Int32, random.pick(4)))) : makeUnary(random.chance(50) ? ZExt32 : SExt32, value);
                break;
            case 1:
                other = makeBinary(relationForType(type), value, random.chance(60) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
                break;
            case 2:
                other = makeSelect(makeBinary(random.chance(50) ? NotEqual : Equal, makeBinary(BitAnd, value, makeConstant(type, random.chance(50) ? 1 : constantBits(type))), makeConstant(type, 0)), makeLeaf(Int32), makeLeaf(Int32));
                break;
            case 3:
                other = makeBinary(random.chance(50) ? Sub : Add, makeLeaf(type), value);
                break;
            case 4:
                other = type == Int64 ? makeUnary(Trunc, value) : makeBinary(Add, makeLeaf(Int64), makeBinary(Shl, makeUnary(random.chance(50) ? ZExt32 : SExt32, value), makeConstant(Int32, random.pick(4))));
                break;
            case 5: {
                Statement statement;
                statement.kind = Statement::MemoryStore;
                statement.access = Store;
                statement.expression = makeCanonical(value);
                statement.place = choosePlace(type == Int32 || type == Float ? 4 : 8, chooseRegionToWrite(), 1);
                program.blocks[block].statements.push_back(statement);
                break;
            }
            case 6: {
                if (!withChecks)
                    break;
                Statement statement;
                statement.kind = Statement::ExitCheck;
                statement.expression = random.chance(50) ? makeBinary(Equal, value, makeConstant(type, constantBits(type))) : makeBinary(NotEqual, makeBinary(BitAnd, value, makeConstant(type, 1)), makeConstant(type, random.chance(50) ? 2 : 0));
                statement.patch = makeCheckPatch();
                program.blocks[block].statements.push_back(statement);
                break;
            }
            default: {
                Expression loadNode;
                loadNode.kind = Expression::MemoryLoad;
                loadNode.type = Int32;
                loadNode.opcode = Load;
                loadNode.place.region = BytesRegion;
                loadNode.place.mode = type == Int64 ? WideIndex : ZeroExtendedIndex;
                loadNode.place.scale = 2;
                loadNode.place.mask = 15;
                loadNode.place.offset = static_cast<int32_t>(random.pick(60));
                loadNode.place.index = value;
                other = addExpression(loadNode);
                break;
            }
            }
            if (other < 0)
                continue;
            int variable = variableToWrite(program.expressions[other].type);
            if (variable >= 0)
                assign(block, variable, other);
        }
    }
    void appendHeldLoad(int block)
    {
        TypeKind type = anyType();
        int target = newVariable(type, true);
        program.pointerKinds[target] = 4;
        MemoryPlace place;
        bool isLoad = withMemory && random.chance(75);
        Opcode opcode = Load;
        if (isLoad) {
            Expression node;
            node.kind = Expression::MemoryLoad;
            node.type = type;
            if (type == Int32 && random.chance(40)) {
                static const Opcode opcodes[] = { Load8Z, Load8S, Load16Z, Load16S };
                opcode = opcodes[random.pick(4)];
            }
            node.opcode = opcode;
            unsigned bytes = opcode == Load8Z || opcode == Load8S ? 1 : opcode == Load16Z || opcode == Load16S ? 2
                : type == Int32 || type == Float                                                               ? 4
                                                                                                               : 8;
            place = choosePlace(bytes, chooseRegionToWrite(), random.chance(60) ? 1 : 2);
            node.place = place;
            assign(block, target, addExpression(node));
        } else
            assign(block, target, random.chance(50) && type == Int32 ? makeComparison(2) : makeExpression(type, 2));
        if (program.expressions[program.blocks[block].statements.back().expression].type != type) {
            program.blocks[block].statements.pop_back();
            return;
        }
        for (unsigned count = random.pick(4); count; --count) {
            unsigned choice = random.pick(100);
            if (isLoad && choice < 45) {
                Statement statement;
                statement.kind = random.chance(70) ? Statement::MemoryStore : Statement::MemoryUpdate;
                TypeKind other = random.chance(70) ? type : integerType();
                if (statement.kind == Statement::MemoryUpdate && (other == Float || other == Double))
                    statement.kind = Statement::MemoryStore;
                unsigned bytes = other == Int32 || other == Float ? 4 : 8;
                statement.access = Store;
                statement.opcode = random.chance(50) ? Add : BitXor;
                statement.place = place;
                if (other == Int32 && random.chance(40)) {
                    statement.access = random.chance(50) ? Store8 : Store16;
                    bytes = statement.access == Store8 ? 1 : 2;
                }
                if (place.mode != FixedOffset || place.offset + static_cast<int>(bytes) > static_cast<int>(sizeOfRegion(place.region)) || (place.region == FieldsRegion && place.offset % 8 + bytes > 8)) {
                    if (other != type || opcode != Load)
                        continue;
                    statement.access = Store;
                }
                statement.expression = makeCanonical(random.chance(50) ? makeConstant(other, constantBits(other)) : makeLeaf(other));
                program.blocks[block].statements.push_back(statement);
            } else
                appendStatement(block);
        }
        int right = block;
        unsigned choice = random.pick(100);
        int value = makeVariable(target);
        if (choice < 40) {
            int variable = anyVariable();
            TypeKind targetType = program.variableTypes[variable];
            if (targetType != type) {
                variable = variableToWrite(type);
                if (variable < 0)
                    return;
            }
            int third;
            if (type == Int32 || type == Int64) {
                static const Opcode opcodes[] = { Add, Sub, Mul, BitAnd, BitOr, BitXor, Add, Sub };
                Opcode otherOpcode = opcodes[random.pick(8)];
                third = random.chance(50) ? makeBinary(otherOpcode, makeLeaf(type), value) : makeBinary(otherOpcode, value, random.chance(50) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
            } else
                third = random.chance(50) ? makeBinary(Add, makeLeaf(type), value) : makeBinary(Sub, value, makeLeaf(type));
            assign(right, variable, third);
        } else if (choice < 65 && variableToWrite(Int32) >= 0) {
            int condition = makeBinary(relationForType(type), random.chance(50) ? value : makeLeaf(type), random.chance(50) ? value : random.chance(50) ? makeConstant(type, constantBits(type))
                                                                                                                                                        : makeLeaf(type));
            int word = variableToWrite(Int32);
            assign(right, word, random.chance(50) ? condition : makeSelect(condition, makeLeaf(Int32), makeLeaf(Int32)));
        } else if (choice < 80 && withChecks) {
            Statement statement;
            statement.kind = Statement::ExitCheck;
            statement.expression = makeBinary(relationForType(type), value, random.chance(60) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
            statement.patch = makeCheckPatch();
            program.blocks[block].statements.push_back(statement);
        } else {
            Statement statement;
            statement.kind = Statement::MemoryStore;
            statement.access = Store;
            statement.expression = makeCanonical(value);
            unsigned bytes = type == Int32 || type == Float ? 4 : 8;
            statement.place = choosePlace(bytes, chooseRegionToWrite(), 1);
            program.blocks[block].statements.push_back(statement);
        }
    }
    void appendConsecutiveCalls(int block)
    {
        int value = program.variableTypes.size() < 150 ? newVariable(Int64) : 1, other = program.variableTypes.size() < 150 ? newVariable(Int64) : 2;
        if (value == 1 || other == 2)
            return;
        auto append = [&](const PatchDescription& description) {
            program.patches.push_back(description);
            isCheckPatch.push_back(0);
            Statement statement;
            statement.kind = Statement::PatchCall;
            statement.patch = static_cast<int>(program.patches.size()) - 1;
            program.blocks[block].statements.push_back(statement);
        };
        auto result = [&](int variable, uint32_t place) {
            PatchResult otherResult;
            otherResult.variable = variable;
            otherResult.type = Int64;
            otherResult.constraint = ValueRep::Register;
            otherResult.location = place;
            return otherResult;
        };
        auto inRegister = [&](int variable, uint32_t place) {
            PatchOperand operand;
            operand.expression = makeVariable(variable);
            operand.constraint = ValueRep::Register;
            operand.location = place;
            return operand;
        };
        PatchDescription otherFirst;
        otherFirst.kind = StubPatch;
        otherFirst.salt = random.next();
        chooseEffects(otherFirst);
        otherFirst.runsWithoutFrame = random.chance(50);
        otherFirst.lateClobbers = random.chance(50) ? random.next() : 0;
        otherFirst.results = { result(value, 2000), result(other, 2001) };
        append(otherFirst);
        for (unsigned i = 1 + random.pick(2); i; --i) {
            PatchDescription otherSecond;
            otherSecond.kind = StubPatch;
            otherSecond.salt = random.next();
            chooseEffects(otherSecond);
            otherSecond.runsWithoutFrame = random.chance(50);
            otherSecond.lateClobbers = random.chance(60) ? random.next() : 0;
            otherSecond.bringsOperands = otherSecond.fillsRegisterFromResult = true;
            otherSecond.movesIntoResult = random.chance(70);
            otherSecond.operands.push_back(inRegister(value, 1003 + random.pick(2)));
            if (random.chance(85))
                otherSecond.operands.push_back(inRegister(other, 1900));
            if (random.chance(40))
                otherSecond.operands.push_back(makeOperand(Int64, random.chance(50) ? ValueRep::ColdAny : ValueRep::WarmAny));
            otherSecond.clearsUpperHalf = random.chance(50);
            if (random.chance(60)) {
                int variable = variableToWrite(random.chance(50) ? Int64 : Int32);
                if (variable > 2) {
                    otherSecond.results = { result(variable, 2000) };
                    otherSecond.results[0].type = program.variableTypes[variable];
                }
            }
            append(otherSecond);
        }
    }
    bool appendNumberedRead(int block)
    {
        unsigned choice = random.pick(100);
        if (!withCalls && choice >= 45 && choice < 65)
            choice = 20;
        Statement statement;
        if (choice < 45) {
            statement.kind = choice < 36 ? Statement::NumberedRead : Statement::LazyConstant;
            TypeKind type = random.chance(80) ? Int64 : Int32;
            statement.variable = variableToWrite(type);
            if (statement.variable < 0 || statement.variable == 1 || statement.variable == 2)
                return false;
            statement.key = random.pick(3);
            statement.form = static_cast<uint8_t>(random.pick(16));
            if (statement.kind == Statement::NumberedRead) {
                statement.expression = random.chance(85) ? makeVariable(1 + static_cast<int>(random.pick(2))) : makeLeaf(Int64);
                statement.place.region = statement.key == 2 ? InstanceRegion : FieldsRegion;
                statement.place.offset = static_cast<int>(statement.key) * 8 + 16;
            } else
                statement.expression = makeLeaf(Int32);
        } else if (choice < 65) {
            statement.kind = Statement::PatchCall;
            statement.patch = newPatch(PurePatch);
            PatchDescription& description = program.patches[statement.patch];
            if (description.kind != PurePatch)
                return false;
            static const uint8_t regions[] = { 2 + FieldsRegion, 2 + InstanceRegion, 60, 61, 62 };
            description.reads = regions[random.pick(std::size(regions))];
            description.salt = 11 + random.pick(2);
            description.operands.resize(std::min<size_t>(description.operands.size(), 1));
            if (description.operands.size()) {
                description.operands[0].expression = makeVariable(1 + static_cast<int>(random.pick(2)));
                description.operands[0].constraint = ValueRep::SomeRegister;
            }
        } else if (choice < 85 || !withCalls) {
            statement.kind = Statement::MemoryStore;
            statement.access = Store;
            TypeKind type = random.chance(80) ? Int64 : Int32;
            statement.expression = makeCanonical(random.chance(40) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
            statement.place.region = random.chance(75) ? FieldsRegion : InstanceRegion;
            statement.place.offset = 8 * (1 + static_cast<int>(random.pick(5)));
        } else if (random.chance(50)) {
            appendConsecutiveCalls(block);
            return true;
        } else {
            statement.kind = Statement::PatchCall;
            statement.patch = newPatch(random.chance(50) ? StubPatch : CallPatch);
            static const uint8_t lists[] = { 60, 61, 62, 2 + FieldsRegion, 2 + InstanceRegion };
            PatchDescription& description = program.patches[statement.patch];
            if (description.kind == StubPatch)
                description.writes = lists[random.pick(std::size(lists))];
        }
        program.blocks[block].statements.push_back(statement);
        return true;
    }
    void appendStatement(int block)
    {
        if (readPercent && random.chance(readPercent) && appendNumberedRead(block))
            return;
        if (isTiny && random.chance(85)) {
            appendTinyStatement(block);
            return;
        }
        if (random.chance(heldPercent) && program.variableTypes.size() < 160) {
            unsigned choice = random.pick(100);
            if (choice < 45 || !withMemory)
                appendHeldLoad(block);
            else if (choice < 62)
                appendSplitUpdate(block);
            else if (choice < 75)
                appendNeighboringAccesses(block);
            else
                appendSharedUses(block);
            return;
        }
        if (random.chance(trapPercent)) {
            appendTrappingLoad(block);
            return;
        }
        if (random.chance(suspendPercent)) {
            Statement statement;
            statement.kind = Statement::PatchCall;
            statement.patch = newPatch(SuspensionPatch);
            program.blocks[block].statements.push_back(statement);
            return;
        }
        unsigned choice = random.pick(100);
        if (withCalls && choice < patchPercent) {
            static const PatchKind kinds[] = { CallPatch, CallPatch, StubPatch, StubPatch, StubPatch, InlinePatch, InlinePatch, PurePatch, PurePatch, KeepAlivePatch, ExitingPatch };
            PatchKind kind = kinds[random.pick(std::size(kinds))];
            Statement statement;
            statement.kind = Statement::PatchCall;
            statement.patch = newPatch(kind);
            program.blocks[block].statements.push_back(statement);
            return;
        }
        if (withChecks && choice < patchPercent + 6) {
            Statement statement;
            statement.kind = Statement::ExitCheck;
            int condition = random.chance(50) ? makeBinary(Equal, makeExpression(Int32, 2), makeConstant(Int32, random.next())) : random.chance(50) ? makeOverflowTest(makeWideSum(2))
                                                                                                                                                    : makePredicate(2);
            statement.expression = condition;
            statement.patch = makeCheckPatch();
            program.blocks[block].statements.push_back(statement);
            return;
        }
        if (withMemory && choice < 45) {
            Statement statement;
            TypeKind type = anyType();
            unsigned bytes = type == Int32 || type == Float ? 4 : 8;
            statement.access = Store;
            if (type == Int32 && random.chance(35)) {
                statement.access = random.chance(50) ? Store8 : Store16;
                bytes = statement.access == Store8 ? 1 : 2;
            }
            unsigned region = chooseRegionToWrite();
            if (random.chance(30) && (type == Int32 || type == Int64)) {
                statement.kind = Statement::MemoryUpdate;
                static const Opcode opcodes[] = { Add, Sub, BitAnd, BitOr, BitXor, Add, Add };
                statement.opcode = opcodes[random.pick(std::size(opcodes))];
                statement.expression = random.chance(55) ? makeConstant(type, random.chance(40) ? (random.chance(50) ? 1 : ~0ull) : constantBits(type)) : makeExpression(type, 1);
                statement.place = choosePlace(bytes, region, 2);
                statement.hasConstantOnLeft = random.chance(20);
                if (random.chance(30)) {
                    statement.variable = variableToWrite(type);
                    statement.form = random.chance(50);
                }
                program.blocks[block].statements.push_back(statement);
                return;
            }
            statement.kind = Statement::MemoryStore;
            if (type == Int32 && statement.access == Store && random.chance(20)) {
                statement.place = choosePlace(4, region, 1);
                Expression node;
                node.kind = Expression::MemoryLoad;
                node.type = Int32;
                node.opcode = Load;
                node.place = statement.place;
                static const Opcode opcodes[] = { Add, Sub, BitAnd, BitOr, BitXor, Add };
                int value = makeUnary(random.chance(80) ? SExt32 : ZExt32, addExpression(node));
                int other = random.chance(40) ? makeConstant(Int64, constantBits(Int64)) : random.chance(50) ? makeUnary(random.chance(50) ? SExt32 : ZExt32, makeLeaf(Int32))
                                                                                                             : makeExpression(Int64, 1);
                statement.expression = makeUnary(Trunc, random.chance(80) ? makeBinary(opcodes[random.pick(6)], value, other) : makeBinary(opcodes[random.pick(6)], other, value));
                program.blocks[block].statements.push_back(statement);
                return;
            }
            statement.expression = makeCanonical(random.chance(25) ? makeConstant(type, constantBits(type)) : makeExpression(type, static_cast<int>(depthOfTrees)));
            statement.place = choosePlace(bytes, region, 2);
            program.blocks[block].statements.push_back(statement);
            if (random.chance(25) && statement.place.mode == FixedOffset && region != FieldsRegion && statement.place.offset + 2 * static_cast<int>(bytes) <= static_cast<int>(sizeOfRegion(region))) {
                Statement otherStatement = statement;
                otherStatement.place.offset += static_cast<int>(bytes);
                otherStatement.expression = makeCanonical(random.chance(50) ? makeConstant(type, constantBits(type)) : makeLeaf(type));
                program.blocks[block].statements.push_back(otherStatement);
            }
            return;
        }
        if (random.chance(2)) {
            Statement statement;
            statement.kind = Statement::MemoryFence;
            program.blocks[block].statements.push_back(statement);
            return;
        }
        int variable = anyVariable();
        if (random.chance(8)) {
            int from = variableOfType(program.variableTypes[variable]);
            if (from >= 0) {
                assign(block, variable, makeVariable(from));
                return;
            }
        }
        assign(block, variable, makeExpression(program.variableTypes[variable], static_cast<int>(depthOfTrees)));
    }
    int variableToWrite(TypeKind type)
    {
        std::vector<int> candidates;
        for (unsigned i = firstFreeVariable; i < program.variableTypes.size(); ++i) {
            if (program.variableTypes[i] == type && !program.pointerKinds[i])
                candidates.push_back(static_cast<int>(i));
        }
        return candidates.empty() ? -1 : candidates[random.pick(candidates.size())];
    }
    void appendStatements(int block, unsigned count)
    {
        for (; count; --count)
            appendStatement(block);
    }
    uint8_t edgeClass()
    {
        unsigned choice = random.pick(100);
        return choice < rarePercent ? 2 : choice < rarePercent + 15 ? 1
                                                                    : 0;
    }
    void jump(int from, int target)
    {
        program.blocks[from].terminator = ProgramBlock::JumpTerminator;
        program.blocks[from].successors = { target };
        program.blocks[from].edgeClasses = { 0 };
    }
    void branch(int from, int expression, int left, int right)
    {
        program.blocks[from].terminator = ProgramBlock::BranchTerminator;
        program.blocks[from].expression = expression;
        program.blocks[from].successors = { left, right };
        program.blocks[from].edgeClasses = { edgeClass(), edgeClass() };
        if (program.blocks[from].edgeClasses[0] == 2 && program.blocks[from].edgeClasses[1] == 2)
            program.blocks[from].edgeClasses[random.pick(2)] = 0;
    }
    void leaveByPatch(int block)
    {
        PatchDescription description;
        description.kind = TailPatch;
        description.salt = random.next();
        description.reads = 1;
        unsigned nextGP = 0, nextFP = 0;
        for (unsigned count = random.pick(6); count; --count) {
            TypeKind type = anyType();
            bool isFloat = type == Float || type == Double;
            PatchOperand operand;
            if (program.usesArguments && random.chance(20))
                operand = makeOperand(type, ValueRep::ColdAny);
            else if (isFloat ? nextFP < 3 : nextGP < 4) {
                operand = makeOperand(type, ValueRep::Register);
                operand.location = 1000 + (isFloat ? nextFP++ : nextGP++);
            } else
                continue;
            description.operands.push_back(operand);
        }
        description.runsWithoutFrame = true;
        for (const PatchOperand& otherOperand : description.operands)
            description.runsWithoutFrame = description.runsWithoutFrame && otherOperand.constraint == ValueRep::Register;
        description.writesLocalState = random.chance(50);
        program.patches.push_back(description);
        isCheckPatch.push_back(0);
        program.blocks[block].terminator = ProgramBlock::PatchTerminator;
        program.blocks[block].patch = static_cast<int>(program.patches.size()) - 1;
        program.blocks[block].successors.clear();
        program.blocks[block].edgeClasses.clear();
    }
    void returnFrom(int block)
    {
        if (withCalls && !program.suspends && random.chance(12)) {
            leaveByPatch(block);
            return;
        }
        program.blocks[block].terminator = ProgramBlock::ReturnTerminator;
        int header = makeConstant(Int64, 0x243f6a8885a308d3ull);
        auto mix = [&](int value) {
            TypeKind type = program.expressions[value].type;
            value = makeCanonical(value);
            if (type == Int32)
                value = makeUnary(ZExt32, value);
            else if (type == Float)
                value = makeUnary(ZExt32, wrapExpression(Expression::BitCast, Int32, value));
            else if (type == Double)
                value = wrapExpression(Expression::BitCast, Int64, value);
            header = makeBinary(Add, makeBinary(Mul, header, makeConstant(Int64, 31)), value);
        };
        if (random.chance(25)) {
            program.blocks[block].expression = makeCanonical(random.chance(50) ? makeVariableLeaf(Int64) : makeExpression(Int64, 2));
            return;
        }
        bool all = random.chance(30);
        for (unsigned variable = 1; variable < program.variableTypes.size(); ++variable) {
            if (!program.pointerKinds[variable] && (all || random.chance(25)))
                mix(makeVariable(static_cast<int>(variable)));
        }
        program.blocks[block].expression = header;
    }

    int appendRegion(int block, int depth)
    {
        unsigned parts = 1 + random.pick(3);
        for (unsigned i = 0; i < parts && block >= 0; ++i) {
            appendStatements(block, random.pick(statementsPerBlock + 1));
            if (blocksLeft < 2 || depth > 4)
                continue;
            if (program.seed >= firstSeedWithNumberedReads && blocksLeft > 4 && random.chance(chainPercent)) {
                blocksLeft -= 3;
                block = emitTestChain(block);
                continue;
            }
            if (isTiny && random.chance(40)) {
                block = emitRepeatedTests(block, depth);
                continue;
            }
            unsigned choice = random.pick(100);
            if (choice < 34)
                block = emitIf(block, depth);
            else if (choice < 52)
                block = emitLoop(block, depth);
            else if (choice < 60)
                block = emitSwitch(block, depth);
            else if (choice < 68 && withThrow)
                block = emitTry(block, depth);
            else if (choice < 74 && loops.size()) {
                --blocksLeft;
                int next = newBlock();
                branch(block, makePredicate(2), random.chance(70) ? loops.back().exit : random.chance(50) ? loops[random.pick(loops.size())].exit
                                                                                                          : loops.back().header,
                    next);
                block = next;
            } else if (choice < 80) {
                int other = newBlock(), next = newBlock();
                blocksLeft -= 2;
                branch(block, makePredicate(2), other, next);
                appendStatements(other, random.pick(3));
                returnFrom(other);
                block = next;
            } else if (choice < 85 && withCalls) {
                int left = newBlock(), right = newBlock(), join = newBlock();
                blocksLeft = blocksLeft > 3 ? blocksLeft - 3 : 0;
                program.blocks[block].terminator = ProgramBlock::PatchTerminator;
                program.blocks[block].patch = newPatch(BranchingPatch);
                program.blocks[block].successors = { left, right };
                program.blocks[block].edgeClasses = { 0, edgeClass() };
                if (program.seed >= firstSeedWithNumberedReads && random.chance(50)) {
                    unsigned which = random.pick(2);
                    program.blocks[block].successors[which] = join;
                    program.blocks[block].edgeClasses[which] = 0;
                    int way = appendRegion(which ? left : right, depth + 1);
                    if (way >= 0)
                        jump(way, join);
                    block = join;
                    continue;
                }
                left = appendRegion(left, depth + 1);
                right = appendRegion(right, depth + 1);
                if (left >= 0)
                    jump(left, join);
                if (right >= 0)
                    jump(right, join);
                block = left >= 0 || right >= 0 ? join : -1;
            } else if (choice < 88 && withCalls && handlers.size())
                block = emitThrowingCall(block);
            else if (choice < 97)
                block = emitRepeatedTests(block, depth);
        }
        return block;
    }
    int emitSumUpdate(int block)
    {
        MemoryPlace place;
        place.region = SumsRegion;
        place.offset = static_cast<int32_t>(random.pick(8) * 8);
        if (random.chance(40)) {
            place.mode = ZeroExtendedIndex;
            place.scale = 3;
            place.mask = 7;
            place.index = makeLeaf(Int32);
        }
        Expression node;
        node.kind = Expression::MemoryLoad;
        node.type = Int64;
        node.opcode = Load;
        node.place = place;
        node.bits = 1;
        int left = addExpression(node);
        static const int64_t amounts[] = { 1, -1, 2, 3, -3, 100, 0x7fffffff, 0xffffffffll, 0x80000000ll, -0x80000000ll, 0x100000000ll };
        int amount = random.chance(55) ? makeConstant(Int64, static_cast<uint64_t>(random.chance(80) ? amounts[random.pick(11)] : static_cast<int64_t>(random.pick(100000)))) : makeUnary(random.chance(75) ? ZExt32 : SExt32, makeExpression(Int32, 1));
        bool adds = random.chance(60);
        int target = newVariable(Int64);
        program.pointerKinds[target] = 4;
        assign(block, target, makeBinary(adds ? Add : Sub, left, amount));
        if (random.chance(20))
            appendStatement(block);
        int condition;
        bool isConst = program.expressions[amount].kind == Expression::Constant;
        int64_t otherAmount = isConst ? static_cast<int64_t>(program.expressions[amount].bits) : 0;
        if (!adds)
            otherAmount = -otherAmount;
        static const int64_t bounds[] = { 8, 100, 0, 0x7fffffff, 0xffffffffll, 0x100000000ll };
        bool passesWhereTrue = random.chance(50);
        if (isConst && otherAmount > 0 && random.chance(70)) {
            int64_t bound = random.chance(50) ? largestCheckedSum : bounds[random.pick(6)];
            condition = makeBinary(passesWhereTrue ? LessEqual : GreaterThan, makeVariable(target), makeConstant(Int64, static_cast<uint64_t>(bound)));
        } else if (isConst && otherAmount < 0 && random.chance(70)) {
            int64_t bound = random.chance(50) ? smallestCheckedSum : bounds[random.pick(6)];
            condition = makeBinary(passesWhereTrue ? GreaterEqual : LessThan, makeVariable(target), makeConstant(Int64, static_cast<uint64_t>(bound)));
        } else
            condition = makeBinary(passesWhereTrue ? BelowEqual : Above, makeBinary(Sub, makeVariable(target), makeConstant(Int64, static_cast<uint64_t>(smallestCheckedSum))), makeConstant(Int64, static_cast<uint64_t>(largestCheckedSum - smallestCheckedSum)));
        int pass = newBlock(), fail = newBlock();
        blocksLeft = blocksLeft > 2 ? blocksLeft - 2 : 0;
        branch(block, condition, passesWhereTrue ? pass : fail, passesWhereTrue ? fail : pass);
        program.blocks[block].edgeClasses = { 0, 0 };
        program.blocks[block].edgeClasses[passesWhereTrue ? 1 : 0] = static_cast<uint8_t>(random.pick(3));
        Statement statement;
        statement.kind = Statement::MemoryStore;
        statement.access = Store;
        statement.expression = makeVariable(target);
        statement.place = place;
        program.blocks[pass].statements.push_back(statement);
        int word = variableToWrite(Int64);
        if (word >= 0 && random.chance(25))
            assign(pass, word, makeVariable(target));
        if (word >= 0 && random.chance(40)) {
            Expression otherAgain = node;
            otherAgain.bits = 0;
            assign(pass, word, addExpression(otherAgain));
        }
        appendStatements(pass, random.pick(2));
        appendStatements(fail, random.pick(3));
        if (word >= 0 && random.chance(25))
            assign(fail, word, makeVariable(target));
        if (random.chance(40)) {
            returnFrom(fail);
            return pass;
        }
        int join = newBlock();
        jump(fail, join);
        jump(pass, join);
        return join;
    }
    struct TestedValue {
        int variable { -1 };
        MemoryPlace place;
        TypeKind type { Int32 };
        Opcode opcode { Load };
    };
    TestedValue testedValueOfType(TypeKind type)
    {
        TestedValue tested;
        tested.type = type;
        if (withMemory && random.chance(40)) {
            tested.place.region = static_cast<uint8_t>(random.chance(60) ? FieldsRegion : random.chance(50) ? BytesRegion
                                                                                                            : ImmutableRegion);
            tested.place.offset = static_cast<int32_t>(random.pick(16) * 8);
            if (type == Int32 && random.chance(40)) {
                static const Opcode opcodes[] = { Load8Z, Load8S, Load16Z, Load16S };
                tested.opcode = opcodes[random.pick(4)];
            }
            return tested;
        }
        tested.variable = variableOfType(type);
        return tested;
    }
    int makeTest(const TestedValue& tested)
    {
        if (tested.variable >= 0)
            return makeVariable(tested.variable);
        Expression node;
        node.kind = Expression::MemoryLoad;
        node.type = tested.type;
        node.opcode = tested.opcode;
        node.place = tested.place;
        return addExpression(node);
    }
    void appendWriteMaybe(int block, const TestedValue& tested, unsigned percent)
    {
        if (!random.chance(percent))
            return;
        if (tested.variable >= 0 || tested.place.region == ImmutableRegion)
            appendStatements(block, 1);
        else
            appendOverlappingStore(block, tested.place, tested.type);
    }
    int emitCheckedSum(int block)
    {
        auto temporary = [&](TypeKind type, int expression) {
            int target = newVariable(type);
            program.pointerKinds[target] = 4;
            assign(block, target, expression);
            return target;
        };
        int own = variableToWrite(Int32);
        int left = own >= 0 && random.chance(50) ? own : temporary(Int32, makeExpression(Int32, 1));
        int right = random.chance(45) ? -1 : random.chance(30) ? variableOfType(Int32)
                                                               : temporary(Int32, makeExpression(Int32, 1));
        int64_t constant = random.chance(60) ? static_cast<int64_t>(random.pick(5)) - 2 : static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32)));
        if (random.chance(20))
            constant = random.chance(50) ? 0x7fffffff : -0x7fffffffll - 1;
        bool adds = random.chance(55);
        int value = makeUnary(random.chance(92) ? SExt32 : ZExt32, makeVariable(left));
        int other = right >= 0 ? makeUnary(random.chance(92) ? SExt32 : ZExt32, makeVariable(right)) : makeConstant(Int64, static_cast<uint64_t>(constant));
        int successor = temporary(Int64, random.chance(85) ? makeBinary(adds ? Add : Sub, value, other) : makeBinary(adds ? Add : Sub, other, value));
        if (random.chance(6))
            appendStatement(block);
        int condition = random.chance(85) ? makeOverflowTest(makeVariable(successor)) : makeBinary(relationForType(Int64), makeBinary(Add, makeVariable(successor), makeConstant(Int64, random.chance(50) ? 0x80000000ull : 0x7fffffffull)), makeConstant(Int64, random.chance(50) ? 0xffffffffull : 0x100000000ull));
        int pass = newBlock(), fail = newBlock();
        blocksLeft = blocksLeft > 2 ? blocksLeft - 2 : 0;
        bool swaps = random.chance(50);
        branch(block, condition, swaps ? fail : pass, swaps ? pass : fail);
        program.blocks[block].edgeClasses = { 0, 0 };
        program.blocks[block].edgeClasses[random.pick(2)] = static_cast<uint8_t>(random.pick(3));
        auto side = [&](int currentBlock, bool narrow) {
            unsigned count = 1 + random.pick(2);
            for (unsigned i = 0; i < count; ++i) {
                switch (narrow ? random.pick(3) : 3 + random.pick(6)) {
                case 0:
                    if (own >= 0)
                        assign(currentBlock, own, makeUnary(Trunc, makeVariable(successor)));
                    break;
                case 1: {
                    int variable = variableToWrite(Int32);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeUnary(Trunc, makeVariable(successor)));
                    break;
                }
                case 2: {
                    int variable = variableToWrite(Int64);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeUnary(ZExt32, makeUnary(Trunc, makeVariable(successor))));
                    break;
                }
                case 3: {
                    int variable = variableToWrite(Int64);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeVariable(successor));
                    break;
                }
                case 4: {
                    int variable = variableToWrite(Double);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeUnary(IToD, makeVariable(successor)));
                    break;
                }
                case 5: {
                    int variable = variableToWrite(Double);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeBinary(adds ? Add : Sub, makeUnary(IToD, makeVariable(left)), right >= 0 ? makeUnary(IToD, makeVariable(right)) : makeConstant(Double, bitsFromDouble(static_cast<double>(constant)))));
                    break;
                }
                case 6: {
                    int variable = variableToWrite(Int32);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeVariable(left));
                    break;
                }
                case 7: {
                    int variable = variableToWrite(Int64);
                    if (variable >= 0)
                        assign(currentBlock, variable, makeUnary(random.chance(50) ? SExt32 : ZExt32, makeVariable(left)));
                    break;
                }
                default: {
                    if (!withCalls)
                        break;
                    PatchDescription description;
                    description.kind = CallPatch;
                    description.salt = random.next();
                    description.reads = 1;
                    description.mayCollect = description.exitsSideways = description.isControlDependent = true;
                    PatchOperand operand;
                    operand.expression = makeVariable(random.chance(70) ? left : successor);
                    operand.constraint = random.chance(50) ? ValueRep::Register : ValueRep::ColdAny;
                    operand.location = 1000;
                    description.operands.push_back(operand);
                    program.patches.push_back(description);
                    isCheckPatch.push_back(0);
                    Statement statement;
                    statement.kind = Statement::PatchCall;
                    statement.patch = static_cast<int>(program.patches.size()) - 1;
                    program.blocks[currentBlock].statements.push_back(statement);
                    break;
                }
                }
            }
        };
        if (random.chance(35)) {
            if (own >= 0 && left == own)
                assign(pass, own, makeUnary(Trunc, makeVariable(successor)));
            else
                side(pass, true);
            if (withCalls && random.chance(50)) {
                PatchDescription description;
                description.kind = CallPatch;
                description.salt = random.next();
                description.reads = 1;
                description.mayCollect = description.exitsSideways = description.isControlDependent = true;
                PatchOperand operand;
                operand.expression = makeVariable(left);
                operand.constraint = random.chance(50) ? ValueRep::Register : ValueRep::ColdAny;
                operand.location = 1000;
                description.operands.push_back(operand);
                program.patches.push_back(description);
                isCheckPatch.push_back(0);
                Statement statement;
                statement.kind = Statement::PatchCall;
                statement.patch = static_cast<int>(program.patches.size()) - 1;
                program.blocks[fail].statements.push_back(statement);
            }
            program.blocks[fail].terminator = ProgramBlock::ReturnTerminator;
            program.blocks[fail].expression = makeBinary(Add, makeUnary(random.chance(50) ? ZExt32 : SExt32, makeVariable(left)), right >= 0 && random.chance(50) ? makeUnary(ZExt32, makeVariable(right)) : makeConstant(Int64, 7));
            return pass;
        }
        bool mixed = random.chance(20);
        side(pass, mixed ? random.chance(50) : true);
        side(fail, mixed ? random.chance(50) : false);
        if (random.chance(30))
            appendStatements(pass, 1);
        if (random.chance(30))
            appendStatements(fail, 1);
        if (random.chance(45)) {
            returnFrom(fail);
            return pass;
        }
        int join = newBlock();
        jump(fail, join);
        jump(pass, join);
        return join;
    }
    int emitRepeatedTests(int block, int depth)
    {
        if (random.chance(16))
            return emitCheckedSum(block);
        if (withMemory && random.chance(15))
            return emitSumUpdate(block);
        switch (random.pick(6)) {
        case 0: {
            TypeKind type = integerType();
            TestedValue tested = testedValueOfType(type);
            if (tested.variable < 0 && !withMemory) {
                tested.variable = variableToWrite(type);
                if (tested.variable < 0)
                    return block;
                assign(block, tested.variable, makeBinary(BitAnd, makeExpression(type, 1), makeConstant(type, 15)));
            }
            unsigned count = 2 + random.pick(5);
            int join = newBlock();
            int64_t start = random.chance(70) ? static_cast<int64_t>(random.pick(6)) : static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32)));
            std::vector<int> arms;
            for (unsigned i = 0; i < count; ++i) {
                int next = newBlock();
                int arm = arms.size() && random.chance(25) ? arms[random.pick(arms.size())] : random.chance(20) ? join
                                                                                                                : newBlock();
                if (arm != join)
                    arms.push_back(arm);
                uint64_t third = static_cast<uint64_t>(random.chance(75) ? start + i : random.chance(60) ? random.pick(40)
                                                                                                         : static_cast<int64_t>(constantBits(type)));
                if (random.chance(75))
                    branch(block, makeBinary(Equal, makeTest(tested), makeConstant(type, third)), arm, next);
                else
                    branch(block, makeBinary(NotEqual, makeTest(tested), makeConstant(type, third)), next, arm);
                if (random.chance(60))
                    program.blocks[block].edgeClasses = { 0, 0 };
                appendWriteMaybe(next, tested, 8);
                block = next;
            }
            std::set<int> done;
            for (int left : arms) {
                if (!done.insert(left).second)
                    continue;
                appendStatements(left, random.pick(3));
                if (random.chance(15))
                    returnFrom(left);
                else
                    jump(left, join);
            }
            appendStatements(block, random.pick(2));
            jump(block, join);
            blocksLeft = blocksLeft > count ? blocksLeft - count : 0;
            return join;
        }
        case 1: {
            TypeKind type = integerType();
            TestedValue tested = testedValueOfType(type);
            if (tested.variable < 0 && !withMemory)
                return block;
            int second = newBlock(), inside = newBlock(), outside = newBlock();
            uint64_t low = random.chance(60) ? random.pick(300) : constantBits(type), high = random.chance(70) ? low + random.pick(300) : constantBits(type);
            static const Opcode lows[] = { LessThan, LessEqual, Below, BelowEqual };
            static const Opcode highs[] = { GreaterThan, GreaterEqual, Above, AboveEqual };
            bool lowFirst = random.chance(50);
            auto test = [&](bool isLow) { return makeBinary(isLow ? lows[random.pick(4)] : highs[random.pick(4)], makeTest(tested), makeConstant(type, isLow ? low : high)); };
            branch(block, test(lowFirst), outside, second);
            if (random.chance(70))
                program.blocks[block].edgeClasses = { 0, 0 };
            appendWriteMaybe(second, tested, 10);
            branch(second, test(!lowFirst), random.chance(90) ? outside : inside, inside);
            if (random.chance(70))
                program.blocks[second].edgeClasses = { 0, 0 };
            appendStatements(outside, random.pick(3));
            blocksLeft = blocksLeft > 3 ? blocksLeft - 3 : 0;
            if (random.chance(40))
                returnFrom(outside);
            else {
                int join = newBlock();
                jump(outside, join);
                appendStatements(inside, random.pick(3));
                jump(inside, join);
                return join;
            }
            return inside;
        }
        case 2: {
            TypeKind type = integerType();
            TestedValue tested = testedValueOfType(type);
            if (tested.variable < 0 && !withMemory)
                return block;
            int second = newBlock(), inside = newBlock(), outside = newBlock();
            unsigned width = type == Int32 ? 32 : 64;
            auto mask = [&] { return random.chance(75) ? 1ull << random.pick(width) : random.chance(50) ? (1ull << random.pick(8)) - 1
                                                                                                        : constantBits(type); };
            auto test = [&](int from, int set, int clear) {
                int bits = makeBinary(BitAnd, makeTest(tested), makeConstant(type, mask()));
                int condition = makeBinary(random.chance(50) ? NotEqual : Equal, bits, makeConstant(type, 0));
                if (program.expressions[condition].opcode == NotEqual)
                    branch(from, condition, set, clear);
                else
                    branch(from, condition, clear, set);
                if (random.chance(75))
                    program.blocks[from].edgeClasses = { 0, 0 };
            };
            test(block, outside, second);
            appendWriteMaybe(second, tested, 10);
            if (random.chance(80))
                test(second, outside, inside);
            else
                test(second, inside, outside);
            appendStatements(outside, random.pick(3));
            blocksLeft = blocksLeft > 3 ? blocksLeft - 3 : 0;
            if (random.chance(40)) {
                returnFrom(outside);
                return inside;
            }
            int join = newBlock();
            jump(outside, join);
            appendStatements(inside, random.pick(3));
            jump(inside, join);
            return join;
        }
        case 3: {
            TypeKind type = integerType();
            bool own = random.chance(isTiny ? 75 : 50);
            int variable = own ? newVariable(type) : variableToWrite(type);
            if (variable < 0)
                return block;
            if (own)
                program.pointerKinds[variable] = 4;
            unsigned count = 2 + random.pick(3);
            int join = newBlock();
            for (unsigned i = 0; i < count; ++i) {
                int arm = newBlock(), next = i + 1 < count ? newBlock() : -1;
                if (next >= 0)
                    branch(block, makePredicate(1), arm, next);
                else
                    jump(block, arm);
                if (!isTiny || random.chance(35))
                    appendStatements(arm, random.pick(2));
                int value;
                switch (random.pick(6)) {
                case 0:
                    value = makeConstant(type, random.pick(3));
                    break;
                case 1:
                    value = makeConstant(type, constantBits(type));
                    break;
                case 2:
                    value = type == Int32 ? makeBinary(relationForType(Int32), makeLeaf(Int32), makeLeaf(Int32)) : makeBinary(Add, makeUnary(ZExt32, makeExpression(Int32, 1)), makeConstant(Int64, 0xfffe000000000000ull));
                    break;
                case 3:
                    value = type == Int64 ? makeConstant(type, interestingConstants[random.pick(std::size(interestingConstants))]) : makeExpression(type, 1);
                    break;
                case 4:
                    value = type == Int64 ? makeBinary(Add, makeUnary(ZExt32, makeLeaf(Int32)), makeConstant(Int64, 0xfffe000000000000ull)) : makeLeaf(type);
                    break;
                default:
                    value = makeExpression(type, 2);
                    break;
                }
                assign(arm, variable, value);
                jump(arm, join);
                block = next;
            }
            blocksLeft = blocksLeft > count ? blocksLeft - count : 0;
            if (random.chance(isTiny ? 10 : 20))
                appendStatements(join, 1);
            if (random.chance(12)) {
                std::vector<int> arms;
                int end = newBlock();
                program.blocks[join].terminator = ProgramBlock::SwitchTerminator;
                program.blocks[join].expression = makeVariable(variable);
                unsigned k = 1 + random.pick(5);
                for (unsigned i = 0; i < k; ++i) {
                    int64_t constant = static_cast<int64_t>(i);
                    program.blocks[join].caseValues.push_back(constant);
                    int target = random.chance(30) ? end : newBlock();
                    if (target != end)
                        arms.push_back(target);
                    program.blocks[join].successors.push_back(target);
                    program.blocks[join].edgeClasses.push_back(random.chance(70) ? 0 : edgeClass());
                }
                int other = newBlock();
                arms.push_back(other);
                program.blocks[join].successors.push_back(other);
                program.blocks[join].edgeClasses.push_back(0);
                for (int target : arms) {
                    appendStatements(target, random.pick(2));
                    if (random.chance(15))
                        returnFrom(target);
                    else
                        jump(target, end);
                }
                return end;
            }
            int left = newBlock(), right = newBlock(), condition;
            switch (random.pick(5)) {
            case 0:
                condition = makeBinary(type == Int64 ? AboveEqual : Above, makeVariable(variable), makeConstant(type, type == Int64 ? 0xfffe000000000000ull : constantBits(type)));
                break;
            case 1:
                condition = makeBinary(random.chance(50) ? Equal : NotEqual, makeVariable(variable), makeConstant(type, random.pick(3)));
                break;
            case 2:
                condition = makeBinary(NotEqual, makeBinary(BitAnd, makeVariable(variable), makeConstant(type, 1)), makeConstant(type, 0));
                break;
            case 3:
                condition = makeBinary(relationForType(type), makeVariable(variable), makeConstant(type, constantBits(type)));
                break;
            default:
                condition = makeVariable(variable);
                break;
            }
            branch(join, condition, left, right);
            if (random.chance(60))
                program.blocks[join].edgeClasses = { 0, 0 };
            int word = variableToWrite(Int32);
            if (type == Int64 && word >= 0 && random.chance(60))
                assign(left, word, makeUnary(Trunc, makeVariable(variable)));
            left = appendRegion(left, depth + 2);
            appendStatements(right, random.pick(3));
            if (random.chance(25)) {
                returnFrom(right);
                return left;
            }
            int end = newBlock();
            if (left >= 0)
                jump(left, end);
            jump(right, end);
            return end;
        }
        default: {
            bool wide = random.chance(60);
            unsigned region = random.chance(70) ? FieldsRegion : chooseRegionToWrite();
            MemoryPlace place = choosePlace(4, region, 1);
            Expression node;
            node.kind = Expression::MemoryLoad;
            node.type = Int32;
            node.opcode = Load;
            node.place = place;
            int otherLeft = addExpression(node);
            int successor;
            bool adds = random.chance(60);
            if (wide) {
                int other = random.chance(60) ? makeConstant(Int64, static_cast<uint64_t>(static_cast<int64_t>(random.chance(60) ? static_cast<int64_t>(random.pick(5)) - 2 : static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32))))))
                                              : makeUnary(random.chance(80) ? SExt32 : ZExt32, makeExpression(Int32, 1));
                int value = makeUnary(SExt32, otherLeft);
                successor = adds && random.chance(20) ? makeBinary(Add, other, value) : makeBinary(adds ? Add : Sub, value, other);
            } else {
                int other = random.chance(60) ? makeConstant(Int32, random.chance(60) ? random.pick(5) : constantBits(Int32)) : makeExpression(Int32, 1);
                successor = makeBinary(adds ? Add : Sub, otherLeft, other);
            }
            int target = newVariable(wide ? Int64 : Int32);
            program.pointerKinds[target] = 4;
            int header = -1;
            if (random.chance(40)) {
                header = newVariable(Int32);
                program.pointerKinds[header] = 4;
                Expression otherCopy = program.expressions[otherLeft];
                assign(block, header, addExpression(otherCopy));
                int read = makeVariable(header);
                program.expressions[otherLeft] = program.expressions[read];
                appendInterference(block, place, Int32, 35);
            }
            assign(block, target, successor);
            appendInterference(block, place, Int32, 20);
            int condition;
            if (wide && random.chance(70))
                condition = makeOverflowTest(makeVariable(target));
            else {
                TypeKind type = wide ? Int64 : Int32;
                condition = makeBinary(relationForType(type), makeVariable(target), makeConstant(type, random.chance(50) ? static_cast<uint64_t>(static_cast<int64_t>(random.pick(200)) - 100) : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32))))));
            }
            int pass = newBlock(), fail = newBlock();
            blocksLeft = blocksLeft > 2 ? blocksLeft - 2 : 0;
            bool storesWhereTrue = random.chance(50);
            branch(block, condition, storesWhereTrue ? pass : fail, storesWhereTrue ? fail : pass);
            program.blocks[block].edgeClasses = { 0, 0 };
            program.blocks[block].edgeClasses[storesWhereTrue ? 1 : 0] = static_cast<uint8_t>(random.pick(3));
            Statement statement;
            statement.kind = Statement::MemoryStore;
            statement.access = Store;
            statement.expression = wide ? makeUnary(Trunc, makeVariable(target)) : makeVariable(target);
            statement.place = place;
            if (random.chance(15)) {
                int word = variableToWrite(Int32);
                if (word >= 0)
                    assign(block, word, condition);
            }
            if (random.chance(15)) {
                int next = newBlock();
                appendInterference(pass, place, Int32, 40);
                jump(pass, next);
                pass = next;
            }
            appendInterference(pass, place, Int32, 30);
            program.blocks[pass].statements.push_back(statement);
            int word = variableToWrite(Int32);
            if (word >= 0 && random.chance(30))
                assign(pass, word, wide ? makeUnary(Trunc, makeVariable(target)) : makeVariable(target));
            appendStatements(pass, random.pick(2));
            appendStatements(fail, random.pick(3));
            if (random.chance(25) && variableToWrite(wide ? Int64 : Int32) >= 0)
                assign(fail, variableToWrite(wide ? Int64 : Int32), makeVariable(target));
            if (random.chance(15) && variableToWrite(wide ? Int64 : Int32) >= 0)
                assign(pass, variableToWrite(wide ? Int64 : Int32), makeVariable(target));
            if (random.chance(40)) {
                returnFrom(fail);
                return pass;
            }
            int join = newBlock();
            jump(fail, join);
            jump(pass, join);
            return join;
        }
        }
    }
    int emitTestChain(int block)
    {
        TypeKind type = random.chance(70) ? Int32 : Int64;
        int variable = variableOfType(type);
        if (variable < 0)
            return block;
        unsigned tests = 2 + random.pick(3), words = 1 + random.pick(2);
        int handedOn[2] = { variableToWrite(random.chance(50) ? Int32 : Int64), random.chance(60) ? -1 : variableToWrite(Int64) };
        uint64_t commonBits = random.pick(4);
        int64_t position = static_cast<int64_t>(random.pick(20)) - 5;
        static const Opcode relations[] = { LessThan, LessEqual, GreaterThan, GreaterEqual, Equal, NotEqual, Below, Above };
        auto fillArm = [&](int arm) {
            for (int word : handedOn) {
                if (word > 2 && word != variable)
                    assign(arm, word, makeConstant(program.variableTypes[word], random.chance(80) ? commonBits : random.pick(4)));
            }
            for (unsigned i = random.pick(words + 1); i; --i) {
                Statement statement;
                statement.kind = Statement::MemoryStore;
                statement.access = Store;
                statement.expression = makeConstant(Int32, random.pick(50));
                statement.place.region = FieldsRegion;
                statement.place.offset = 8 * static_cast<int>(random.pick(3));
                program.blocks[arm].statements.push_back(statement);
            }
        };
        int join = newBlock();
        for (unsigned i = 0; i < tests; ++i) {
            int arm = newBlock(), next = newBlock();
            int64_t bound = random.chance(70) ? position + 1 + static_cast<int64_t>(random.pick(8)) : position - static_cast<int64_t>(random.pick(8));
            Opcode relation = i && random.chance(40) ? GreaterThan : random.chance(50) ? LessThan
                                                                                       : relations[random.pick(std::size(relations))];
            branch(block, makeBinary(relation, makeVariable(variable), makeConstant(type, static_cast<uint64_t>(bound))), arm, next);
            program.blocks[block].edgeClasses = { 0, 0 };
            fillArm(arm);
            jump(arm, join);
            block = next;
            position = bound;
        }
        if (random.chance(50))
            fillArm(block);
        else
            appendStatements(block, random.pick(2));
        jump(block, join);
        return join;
    }
    int emitIf(int block, int depth)
    {
        int left = newBlock(), join = newBlock();
        bool hasElse = random.chance(55);
        int right = hasElse ? newBlock() : join;
        blocksLeft -= 2;
        branch(block, makePredicate(static_cast<int>(depthOfTrees)), random.chance(50) || hasElse ? left : left, right);
        if (!hasElse && random.chance(50)) {
            std::swap(program.blocks[block].successors[0], program.blocks[block].successors[1]);
            std::swap(program.blocks[block].edgeClasses[0], program.blocks[block].edgeClasses[1]);
        }
        int value = appendRegion(left, depth + 1);
        int other = hasElse ? appendRegion(right, depth + 1) : block;
        if (value >= 0)
            jump(value, join);
        if (hasElse && other >= 0)
            jump(other, join);
        return value >= 0 || other >= 0 ? join : -1;
    }
    int emitLoop(int block, int depth)
    {
        int head = newBlock(), exit = newBlock(), body = newBlock();
        blocksLeft = blocksLeft > 3 ? blocksLeft - 3 : 0;
        unsigned kind = random.pick(3);
        TypeKind type = random.chance(75) ? Int32 : Int64;
        int i = newVariable(type);
        int limit = newVariable(type);
        assign(block, i, makeConstant(type, random.chance(70) ? 0 : random.pick(3)));
        assign(block, limit, random.chance(50) ? makeConstant(type, 1 + random.pick(5)) : makeBinary(BitAnd, makeExpression(type, 1), makeConstant(type, 3)));
        loops.push_back({ head, exit });
        if (kind == 0) {
            jump(block, head);
            branch(head, makeBinary(LessThan, makeVariable(i), random.chance(50) ? makeVariable(limit) : makeConstant(type, 1 + random.pick(4))), body, exit);
            program.blocks[head].edgeClasses[0] = 0;
            int end = appendRegion(body, depth + 1);
            if (end >= 0) {
                assign(end, i, makeBinary(Add, makeVariable(i), makeConstant(type, 1)));
                jump(end, head);
            }
        } else if (kind == 1) {
            jump(block, head);
            jump(head, body);
            int end = appendRegion(body, depth + 1);
            if (end >= 0) {
                assign(end, i, makeBinary(Add, makeVariable(i), makeConstant(type, 1)));
                branch(end, makeBinary(random.chance(70) ? LessThan : Below, makeVariable(i), makeVariable(limit)), head, exit);
                program.blocks[end].edgeClasses[0] = 0;
            }
        } else {
            jump(block, head);
            int end = appendRegion(head, depth + 1);
            if (end >= 0) {
                branch(end, makeBinary(GreaterEqual, makeVariable(i), makeVariable(limit)), exit, body);
                program.blocks[end].edgeClasses[1] = 0;
                int last = appendRegion(body, depth + 1);
                if (last >= 0) {
                    assign(last, i, makeBinary(Add, makeVariable(i), makeConstant(type, 1)));
                    jump(last, head);
                }
            }
        }
        loops.pop_back();
        return exit;
    }
    int emitSwitch(int block, int depth)
    {
        TypeKind type = integerType();
        unsigned count = 1 + random.pick(8);
        static const uint64_t masks[] = { 3, 7, 15, 63, ~0ull };
        int value = random.chance(80) ? makeBinary(BitAnd, makeExpression(type, 1), makeConstant(type, masks[random.pick(4)])) : makeExpression(type, 1);
        std::vector<int64_t> cases;
        int64_t start = random.chance(70) ? static_cast<int64_t>(random.pick(5)) - 2 : random.chance(50) ? static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32)))
                                                                                                         : 0;
        for (unsigned i = 0; i < count; ++i) {
            int64_t constant = random.chance(75) ? start + static_cast<int64_t>(i) : random.chance(50) ? static_cast<int64_t>(random.pick(70))
                : type == Int32                                                                        ? static_cast<int64_t>(static_cast<int32_t>(constantBits(Int32)))
                                                                                                       : static_cast<int64_t>(constantBits(Int64));
            if (type == Int32)
                constant = static_cast<int32_t>(constant);
            bool twice = false;
            for (int64_t other : cases)
                twice |= other == constant;
            if (!twice)
                cases.push_back(constant);
        }
        int join = newBlock();
        ProgramBlock::Terminator terminator = ProgramBlock::SwitchTerminator;
        std::vector<int> successors;
        std::vector<uint8_t> edgeClasses;
        std::vector<int> arms;
        for (unsigned i = 0; i <= cases.size(); ++i) {
            if (arms.size() && random.chance(25)) {
                successors.push_back(arms[random.pick(arms.size())]);
                edgeClasses.push_back(edgeClass());
                continue;
            }
            if (random.chance(15)) {
                successors.push_back(join);
                edgeClasses.push_back(edgeClass());
                continue;
            }
            int left = newBlock();
            arms.push_back(left);
            successors.push_back(left);
            edgeClasses.push_back(edgeClass());
            blocksLeft = blocksLeft ? blocksLeft - 1 : 0;
        }
        edgeClasses.back() = 0;
        program.blocks[block].terminator = terminator;
        program.blocks[block].expression = value;
        program.blocks[block].caseValues = cases;
        program.blocks[block].successors = successors;
        program.blocks[block].edgeClasses = edgeClasses;
        bool reached = false;
        for (int successor : successors)
            reached |= successor == join;
        for (unsigned k = 0; k < arms.size(); ++k) {
            int value = blocksLeft > 4 && random.chance(30) ? appendRegion(arms[k], depth + 2) : arms[k];
            if (value == arms[k])
                appendStatements(value, random.pick(3));
            if (value < 0)
                continue;
            if (random.chance(12))
                returnFrom(value);
            else {
                jump(value, k + 1 < arms.size() && random.chance(15) ? arms[k + 1] : join);
                reached = true;
            }
        }
        return reached ? join : -1;
    }
    int emitThrowingCall(int block)
    {
        int next = newBlock(), pad = newBlock();
        program.blocks[block].terminator = ProgramBlock::PatchTerminator;
        program.blocks[block].patch = newPatch(ThrowingPatch);
        program.blocks[block].successors = { next, pad };
        program.blocks[block].edgeClasses = { 0, 2 };
        program.blocks[next].isPatchContinuation = true;
        Statement statement;
        statement.kind = Statement::ResultsOfPatch;
        statement.patch = program.blocks[block].patch;
        program.blocks[next].statements.push_back(statement);
        if (random.chance(40))
            appendStatements(pad, random.pick(2));
        jump(pad, handlers.back());
        return next;
    }
    int emitTry(int block, int depth)
    {
        int handler = newBlock(), after = newBlock();
        blocksLeft = blocksLeft > 2 ? blocksLeft - 2 : 0;
        handlers.push_back(handler);
        int end = block;
        unsigned otherCalls = 1 + random.pick(3);
        for (unsigned i = 0; i < otherCalls && end >= 0; ++i) {
            end = emitThrowingCall(end);
            appendStatements(end, random.pick(3));
            if (random.chance(40))
                end = appendRegion(end, depth + 1);
        }
        handlers.pop_back();
        bool reached = end >= 0;
        if (end >= 0)
            jump(end, after);
        int header = random.chance(60) ? appendRegion(handler, depth + 1) : handler;
        if (header >= 0) {
            if (random.chance(20))
                returnFrom(header);
            else {
                jump(header, after);
                reached = true;
            }
        }
        return reached ? after : -1;
    }

    void appendRandomGraph()
    {
        unsigned count = 4 + random.pick(blocksLeft);
        int first = static_cast<int>(program.blocks.size());
        for (unsigned i = 0; i < count; ++i)
            newBlock();
        jump(0, first);
        for (unsigned i = 0; i < count; ++i) {
            int right = first + static_cast<int>(i);
            appendStatements(right, random.pick(statementsPerBlock + 1));
            auto target = [&] {
                if (i + 1 < count && random.chance(80))
                    return first + static_cast<int>(i) + 1 + static_cast<int>(random.pick(std::min<unsigned>(count - i - 1, random.chance(70) ? 3 : 100)));
                return first + static_cast<int>(random.pick(count));
            };
            unsigned choice = random.pick(100);
            if (i + 1 == count || choice < 6)
                returnFrom(right);
            else if (choice < 25)
                jump(right, target());
            else if (choice < 92)
                branch(right, makePredicate(static_cast<int>(depthOfTrees)), target(), target());
            else {
                TypeKind type = integerType();
                program.blocks[right].terminator = ProgramBlock::SwitchTerminator;
                program.blocks[right].expression = makeBinary(BitAnd, makeExpression(type, 1), makeConstant(type, 7));
                unsigned k = 1 + random.pick(6);
                for (unsigned otherCount = 0; otherCount < k; ++otherCount) {
                    program.blocks[right].caseValues.push_back(otherCount);
                    program.blocks[right].successors.push_back(target());
                    program.blocks[right].edgeClasses.push_back(edgeClass());
                }
                program.blocks[right].successors.push_back(target());
                program.blocks[right].edgeClasses.push_back(0);
            }
        }
    }

    void limitIterations()
    {
        std::vector<int> order(program.blocks.size(), -1), state(program.blocks.size(), 0);
        std::vector<std::pair<int, unsigned>> path { { 0, 0 } };
        std::vector<std::pair<int, unsigned>> back;
        state[0] = 1;
        while (path.size()) {
            auto& [bits, i] = path.back();
            if (i == program.blocks[bits].successors.size()) {
                state[bits] = 2;
                path.pop_back();
                continue;
            }
            int successor = program.blocks[bits].successors[i];
            unsigned edge = i++;
            if (state[successor] == 1)
                back.push_back({ bits, edge });
            else if (!state[successor]) {
                state[successor] = 1;
                path.push_back({ successor, 0 });
            }
        }
        if (back.empty())
            return;
        int outside = newBlock();
        returnFrom(outside);
        std::vector<int> headOf(program.blocks.size(), -1);
        for (auto [bits, i] : back) {
            int target = program.blocks[bits].successors[i];
            if (headOf[target] < 0) {
                int header = newBlock();
                headOf[target] = header;
                assign(header, fuelVariable, makeBinary(Sub, makeVariable(fuelVariable), makeConstant(Int32, 1)));
                program.blocks[header].terminator = ProgramBlock::BranchTerminator;
                program.blocks[header].expression = makeBinary(LessThan, makeVariable(fuelVariable), makeConstant(Int32, 0));
                program.blocks[header].successors = { outside, target };
                program.blocks[header].edgeClasses = { static_cast<uint8_t>(random.pick(3)), 0 };
            }
            program.blocks[bits].successors[i] = headOf[target];
        }
        if (random.chance(60)) {
            unsigned count = static_cast<unsigned>(headOf.size());
            for (unsigned block = 0; block < count; ++block) {
                for (int& successor : program.blocks[block].successors) {
                    if (successor >= 0 && static_cast<unsigned>(successor) < count && headOf[successor] >= 0)
                        successor = headOf[successor];
                }
            }
        }
    }

    void run()
    {
        program.seed = seed;
        program.emissionSeed = random.next();
        program.traitBits = random.next();
        program.usesHostFeatures = random.chance(60);
        program.usesScopes = random.chance(80);
        program.branchReach = random.chance(12) ? 128 << random.pick(6) : -1;
        pressure = random.pick(4);
        withNumbers = random.chance(75);
        withCalls = random.chance(85);
        withMemory = random.chance(85);
        withChecks = random.chance(60);
        withThrow = withCalls && random.chance(40);
        depthOfTrees = 1 + random.pick(4);
        statementsPerBlock = 2 + random.pick(random.chance(80) ? 5 : 12);
        patchPercent = random.chance(30) ? 4 : 14 + random.pick(12);
        rarePercent = random.chance(30) ? 0 : random.chance(50) ? 12
                                                                : 30;
        loadsOften = random.chance(40);
        constantMode = random.pick(3);
        blocksLeft = random.chance(25) ? 2 : random.chance(80) ? 6 + random.pick(16)
                                                               : 30 + random.pick(40);
        program.numberOfSlots = 1 + random.pick(2);
        program.usesSlot = random.chance(45);
        reusePercent = random.chance(50) ? 8 : 35;
        program.suspends = withCalls && random.chance(12);
        program.usesArguments = !program.suspends && random.chance(30);
        trapPercent = random.chance(25) ? 8 : 0;
        if (program.seed >= firstSeedWithNumberedReads) {
            RandomNumbers own { program.seed ^ 0x5eedull };
            readPercent = own.chance(30) ? 15 + own.pick(50) : 0;
            chainPercent = own.chance(60) ? 5 + own.pick(40) : 0;
            if (own.chance(50)) {
                readPercent = 90;
                statementsPerBlock = 6 + own.pick(10);
                blocksLeft = std::min<unsigned>(blocksLeft, 4 + own.pick(8));
                withCalls = withCalls || own.chance(70);
            }
        }
        heldPercent = random.chance(60) ? 8 : 0;
        sharePercent = random.chance(60) ? 10 : 2;
        suspendPercent = program.suspends ? 10 : 0;
        program.marksNarrowReferences = !program.suspends && random.chance(40);
        program.stackCheckMode = random.pick(3);

        program.variableTypes = { Int32, Int64, Int64, Int64, Int64, Double, Double, Double, Int64, Int32 };
        program.mayHoldAnyBits.assign(10, 1);
        program.pointerKinds = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 2 };
        static const unsigned counts[4] = { 4, 9, 18, 34 };
        unsigned count = counts[pressure] + random.pick(6);
        isTiny = random.chance(40);
        if (random.chance(1) && random.chance(25)) {
            blocksLeft = 150 + random.pick(250);
            count = 20 + random.pick(40);
            statementsPerBlock = 8;
            isTiny = false;
        }
        program.extraSlotSize = program.usesSlot && random.chance(15) ? 8u << random.pick(11) : 0;
        if (isTiny) {
            count = 2 + random.pick(3);
            statementsPerBlock = random.chance(60) ? 1 : 2;
            heldPercent = random.chance(80) ? 0 : 5;
            sharePercent = 2;
            patchPercent = 5;
            depthOfTrees = 1;
            blocksLeft = 10 + random.pick(30);
            trapPercent = 0;
            if (random.chance(70))
                rarePercent = 0;
        }
        for (unsigned i = 0; i < count; ++i)
            newVariable(i < 2 ? (i ? Int32 : Int64) : isTiny ? integerType()
                                                             : anyType(),
                random.chance(40));
        if (isTiny)
            makeSharedExpressions();
        int entry = newBlock();
        assign(entry, fuelVariable, makeConstant(Int32, 12 + random.pick(20)));
        unsigned numVars = static_cast<unsigned>(program.variableTypes.size());
        bool uses = withMemory;
        withMemory = false;
        for (unsigned variable = firstFreeVariable; variable < numVars; ++variable) {
            TypeKind type = program.variableTypes[variable];
            int base = random.chance(40) ? makeConstant(type, constantBits(type)) : initialValue(type);
            assign(entry, static_cast<int>(variable), base);
        }
        for (unsigned offset = 0; program.usesSlot && offset < slotSize; offset += 8) {
            Statement statement;
            statement.kind = Statement::MemoryStore;
            statement.access = Store;
            statement.expression = random.chance(50) ? makeConstant(Int64, constantBits(Int64)) : makeVariable(1 + static_cast<int>(random.pick(4)));
            statement.place.region = SlotRegion;
            statement.place.offset = static_cast<int>(offset);
            program.blocks[entry].statements.push_back(statement);
        }
        withMemory = uses;
        if (random.chance(25)) {
            withThrow = false;
            appendRandomGraph();
        } else {
            int end = appendRegion(entry, 0);
            if (end >= 0) {
                appendStatements(end, random.pick(4));
                returnFrom(end);
            }
        }
        std::vector<Statement> initial;
        for (unsigned variable = numVars; variable < program.variableTypes.size(); ++variable) {
            Statement statement;
            bool isPointer = program.pointerKinds[variable] == 1 || program.pointerKinds[variable] == 2;
            statement.kind = isPointer ? Statement::PointerSelection : Statement::Assignment;
            statement.variable = static_cast<int>(variable);
            statement.expression = makeConstant(isPointer ? Int32 : program.variableTypes[variable], isPointer ? 1 : 0);
            initial.push_back(statement);
        }
        std::vector<Statement> ownOfEntry(program.blocks[entry].statements);
        program.blocks[entry].statements.insert(program.blocks[entry].statements.begin(), initial.begin(), initial.end());
        limitIterations();
        if (!program.suspends && program.blocks.size() > 4 && random.chance(8)) {
            std::vector<int> candidates;
            std::vector<char> reached(program.blocks.size(), 0);
            std::vector<int> worklist { 0 };
            reached[0] = 1;
            while (worklist.size()) {
                int right = worklist.back();
                worklist.pop_back();
                for (int successor : program.blocks[right].successors) {
                    if (!reached[successor]) {
                        reached[successor] = 1;
                        worklist.push_back(successor);
                    }
                }
            }
            for (unsigned block = 1; block < program.blocks.size(); ++block) {
                bool fits = reached[block] && !program.blocks[block].isPatchContinuation && (program.blocks[block].terminator != ProgramBlock::ReturnTerminator || random.chance(20));
                for (const ProgramBlock& otherBlock : program.blocks) {
                    if (otherBlock.terminator == ProgramBlock::PatchTerminator && otherBlock.successors.size() > 1 && (otherBlock.successors[0] == static_cast<int>(block) || otherBlock.successors[1] == static_cast<int>(block)))
                        fits = false;
                }
                for (const Statement& otherStatement : program.blocks[block].statements)
                    fits = fits && otherStatement.kind != Statement::ResultsOfPatch;
                if (fits)
                    candidates.push_back(static_cast<int>(block));
            }
            if (candidates.size()) {
                int expression = newBlock();
                program.secondEntryBlock = expression;
                for (const Statement& otherStatement : initial)
                    program.blocks[expression].statements.push_back(otherStatement);
                assign(expression, fuelVariable, makeConstant(Int32, 5 + random.pick(20)));
                for (unsigned variable = firstFreeVariable; variable < numVars; ++variable)
                    assign(expression, static_cast<int>(variable), random.chance(50) ? makeConstant(program.variableTypes[variable], constantBits(program.variableTypes[variable])) : this->initialValue(program.variableTypes[variable]));
                for (unsigned offset = 0; program.usesSlot && offset < slotSize; offset += 8) {
                    Statement statement;
                    statement.kind = Statement::MemoryStore;
                    statement.access = Store;
                    statement.expression = makeConstant(Int64, constantBits(Int64));
                    statement.place.region = SlotRegion;
                    statement.place.offset = static_cast<int>(offset);
                    program.blocks[expression].statements.push_back(statement);
                }
                jump(expression, candidates[random.pick(candidates.size())]);
            }
        }
    }
    int initialValue(TypeKind type)
    {
        int left = 1 + static_cast<int>(random.pick(4));
        switch (type) {
        case Int64:
            return random.chance(50) ? makeVariable(left) : makeBinary(random.chance(50) ? Add : Mul, makeVariable(left), makeConstant(Int64, constantBits(Int64)));
        case Int32:
            return random.chance(60) ? makeUnary(Trunc, makeVariable(left)) : makeBinary(Add, makeUnary(Trunc, makeVariable(left)), makeConstant(Int32, constantBits(Int32)));
        case Double:
            return random.chance(50) ? makeVariable(5 + static_cast<int>(random.pick(3))) : makeBinary(Add, makeVariable(5 + static_cast<int>(random.pick(3))), makeConstant(Double, constantBits(Double)));
        case Float:
            return makeUnary(DoubleToFloat, makeVariable(5 + static_cast<int>(random.pick(3))));
        default:
            abort();
        }
    }
};

Program generateProgram(uint64_t seed)
{
    Program program;
    ProgramGenerator(program, seed).run();
    return program;
}

}
}
}
} // namespace JSC::AOT::Machine::ExecutionFuzzer

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT) && OS(LINUX) && !ASAN_ENABLED
