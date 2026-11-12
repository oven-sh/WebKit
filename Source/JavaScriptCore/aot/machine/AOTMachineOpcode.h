/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "JSExportMacros.h"
#include "Width.h"
#include <array>
#include <wtf/Assertions.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT { namespace Machine {

enum TypeKind : uint8_t {
    Void,
    Int32,
    Int64,
    Float,
    Double,
    Tuple,
};

class Type {
public:
    constexpr Type() = default;
    constexpr Type(TypeKind kind)
        : m_kind(kind)
    {
    }
    static constexpr Type tuple(uint16_t index)
    {
        Type result(Tuple);
        result.m_tupleIndex = index;
        return result;
    }

    constexpr TypeKind kind() const { return m_kind; }
    constexpr bool isTuple() const { return m_kind == Tuple; }
    constexpr uint16_t tupleIndex() const { return m_tupleIndex; }
    constexpr bool isInt() const { return m_kind == Int32 || m_kind == Int64; }
    constexpr bool isFloat() const { return m_kind == Float || m_kind == Double; }
    constexpr bool isNumeric() const { return isInt() || isFloat(); }

    friend constexpr bool operator==(Type, Type) = default;

private:
    TypeKind m_kind { Void };
    [[maybe_unused]] uint8_t m_byteThatIsZero { 0 };
    uint16_t m_tupleIndex { 0 };
};
static_assert(sizeof(Type) == 4);

constexpr Type pointerType() { return Int64; }

enum class Bank : uint8_t { GP, FP };
inline constexpr unsigned numberOfBanks = 2;

constexpr Bank bankOf(Type type)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(type.isNumeric());
    return type.isFloat() ? Bank::FP : Bank::GP;
}

constexpr Width widthOf(Type type)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(type.isNumeric());
    return type == Int32 || type == Float ? Width32 : Width64;
}

constexpr unsigned sizeOf(Type type) { return bytesForWidth(widthOf(type)); }

#define FOR_EACH_AOT_MACHINE_OPCODE(v) \
    v(Nop, 0, HasEffects) \
    v(Opaque, 1, IsNeverNumbered) \
    \
    v(Const32, 0, HasNoPosition) \
    v(Const64, 0, HasNoPosition) \
    v(ConstFloat, 0, HasNoPosition) \
    v(ConstDouble, 0, HasNoPosition) \
    v(FramePointer, 0, HasNoPosition) \
    v(SlotBase, 0, HasNoPosition) \
    v(AddressOf, 0, HasNoPosition) \
    v(NumberOf, 0, HasNoPosition) \
    v(InstanceAddressOf, 0, HasNoPosition) \
    v(ArgumentReg, 0, IsNeverNumbered) \
    \
    v(Add, 2, IsCommutative) \
    v(Sub, 2, None) \
    v(Mul, 2, IsCommutative) \
    v(MulHigh, 2, IsCommutative) \
    v(UMulHigh, 2, IsCommutative) \
    v(Div, 2, None) \
    v(UDiv, 2, None) \
    v(Mod, 2, None) \
    v(UMod, 2, None) \
    v(Neg, 1, None) \
    v(BitAnd, 2, IsCommutative) \
    v(BitOr, 2, IsCommutative) \
    v(BitXor, 2, IsCommutative) \
    v(Shl, 2, None) \
    v(SShr, 2, None) \
    v(ZShr, 2, None) \
    v(RotR, 2, None) \
    v(Clz, 1, None) \
    v(PopCount, 1, None) \
    \
    v(Abs, 1, None) \
    v(Ceil, 1, None) \
    v(Floor, 1, None) \
    v(FTrunc, 1, None) \
    v(Sqrt, 1, None) \
    v(FMin, 2, None) \
    v(FMax, 2, None) \
    \
    v(BitwiseCast, 1, None) \
    v(SExt8, 1, None) \
    v(SExt16, 1, None) \
    v(SExt32, 1, None) \
    v(ZExt32, 1, None) \
    v(Trunc, 1, None) \
    v(IToD, 1, None) \
    v(FloatToDouble, 1, None) \
    v(DoubleToFloat, 1, None) \
    \
    v(Equal, 2, IsCommutative | IsComparison) \
    v(NotEqual, 2, IsCommutative | IsComparison) \
    v(LessThan, 2, IsComparison) \
    v(GreaterThan, 2, IsComparison) \
    v(LessEqual, 2, IsComparison) \
    v(GreaterEqual, 2, IsComparison) \
    v(Above, 2, IsComparison) \
    v(Below, 2, IsComparison) \
    v(AboveEqual, 2, IsComparison) \
    v(BelowEqual, 2, IsComparison) \
    v(EqualOrUnordered, 2, IsCommutative | IsComparison) \
    v(Select, 3, None) \
    \
    v(Load8Z, 1, IsLoad) \
    v(Load8S, 1, IsLoad) \
    v(Load16Z, 1, IsLoad) \
    v(Load16S, 1, IsLoad) \
    v(Load, 1, IsLoad) \
    v(Store8, 2, IsStore | HasEffects) \
    v(Store16, 2, IsStore | HasEffects) \
    v(Store, 2, IsStore | HasEffects) \
    v(Fence, 0, HasEffects) \
    \
    v(Phi, 0, IsNeverNumbered) \
    v(Upsilon, 1, HasEffects) \
    v(Extract, 1, IsNeverNumbered) \
    v(Patch, variableChildren, IsNeverNumbered) \
    v(Check, variableChildren, HasEffects) \
    \
    v(Jump, 0, IsTerminal | HasEffects) \
    v(Branch, 1, IsTerminal | HasEffects) \
    v(Switch, 1, IsTerminal | HasEffects) \
    v(Return, variableChildren, IsTerminal | HasEffects) \
    v(Oops, 0, IsTerminal | HasEffects)

enum Opcode : uint8_t {
#define AOT_DECLARE_MACHINE_OPCODE(name, children, traits) name,
    FOR_EACH_AOT_MACHINE_OPCODE(AOT_DECLARE_MACHINE_OPCODE)
#undef AOT_DECLARE_MACHINE_OPCODE
};

#define AOT_COUNT_MACHINE_OPCODE(name, children, traits) + 1
inline constexpr unsigned numberOfOpcodes = 0 FOR_EACH_AOT_MACHINE_OPCODE(AOT_COUNT_MACHINE_OPCODE);
#undef AOT_COUNT_MACHINE_OPCODE

inline constexpr uint8_t variableChildren = 0xff;

namespace OpcodeTrait {
enum : uint16_t {
    None = 0,
    IsCommutative = 1 << 0,
    IsComparison = 1 << 1,
    IsLoad = 1 << 2,
    IsStore = 1 << 3,
    IsTerminal = 1 << 4,
    HasEffects = 1 << 5,
    HasNoPosition = 1 << 6,
    IsNeverNumbered = 1 << 7,
};
}

struct OpcodeDescription {
    ASCIILiteral name;
    uint8_t numberOfChildren;
    uint16_t traits;
};

constexpr std::array<OpcodeDescription, numberOfOpcodes> describeOpcodes()
{
    using namespace OpcodeTrait;
    return { {
#define AOT_DESCRIBE_MACHINE_OPCODE(name, children, traits) { #name ""_s, children, static_cast<uint16_t>(traits) },
        FOR_EACH_AOT_MACHINE_OPCODE(AOT_DESCRIBE_MACHINE_OPCODE)
#undef AOT_DESCRIBE_MACHINE_OPCODE
    } };
}
inline constexpr std::array<OpcodeDescription, numberOfOpcodes> descriptionsOfOpcodes = describeOpcodes();

constexpr const OpcodeDescription& descriptionOf(Opcode opcode)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(opcode < numberOfOpcodes);
    return descriptionsOfOpcodes[opcode];
}

inline constexpr unsigned sizeOfTableByOpcode = 1 << (8 * sizeof(Opcode));
static_assert(numberOfOpcodes <= sizeOfTableByOpcode);

constexpr std::array<uint16_t, sizeOfTableByOpcode> listTraitsOfOpcodes()
{
    using namespace OpcodeTrait;
    return { {
#define AOT_LIST_TRAITS_OF_MACHINE_OPCODE(name, children, traits) static_cast<uint16_t>(traits),
        FOR_EACH_AOT_MACHINE_OPCODE(AOT_LIST_TRAITS_OF_MACHINE_OPCODE)
#undef AOT_LIST_TRAITS_OF_MACHINE_OPCODE
    } };
}
inline constexpr std::array<uint16_t, sizeOfTableByOpcode> traitsOfOpcodes = listTraitsOfOpcodes();

constexpr bool hasTrait(Opcode opcode, uint16_t trait)
{
    ASSERT_UNDER_CONSTEXPR_CONTEXT(opcode < numberOfOpcodes);
    return traitsOfOpcodes[opcode] & trait;
}
constexpr bool isCommutative(Opcode opcode) { return hasTrait(opcode, OpcodeTrait::IsCommutative); }
constexpr bool isComparison(Opcode opcode) { return hasTrait(opcode, OpcodeTrait::IsComparison); }
constexpr bool isLoad(Opcode opcode) { return hasTrait(opcode, OpcodeTrait::IsLoad); }
constexpr bool isStore(Opcode opcode) { return hasTrait(opcode, OpcodeTrait::IsStore); }
constexpr bool isMemoryAccess(Opcode opcode) { return hasTrait(opcode, OpcodeTrait::IsLoad | OpcodeTrait::IsStore); }
constexpr bool isTerminal(Opcode opcode) { return opcode >= Jump; }
static_assert([] {
    for (unsigned opcode = 0; opcode < numberOfOpcodes; ++opcode) {
        if (isTerminal(static_cast<Opcode>(opcode)) != hasTrait(static_cast<Opcode>(opcode), OpcodeTrait::IsTerminal))
            return false;
    }
    return true;
}());
constexpr bool hasNoPosition(Opcode opcode) { return hasTrait(opcode, OpcodeTrait::HasNoPosition); }
constexpr bool isConstant(Opcode opcode) { return opcode == Const32 || opcode == Const64 || opcode == ConstFloat || opcode == ConstDouble; }
constexpr bool namesSymbol(Opcode opcode) { return opcode == AddressOf || opcode == NumberOf || opcode == InstanceAddressOf; }

constexpr bool countsChildrenAtFirstUse(Opcode opcode)
{
    return !hasTrait(opcode, OpcodeTrait::IsLoad | OpcodeTrait::IsStore | OpcodeTrait::IsTerminal | OpcodeTrait::HasEffects | OpcodeTrait::HasNoPosition | OpcodeTrait::IsNeverNumbered);
}
static_assert(countsChildrenAtFirstUse(Add) && countsChildrenAtFirstUse(Div) && countsChildrenAtFirstUse(BitAnd) && countsChildrenAtFirstUse(FMax) && countsChildrenAtFirstUse(BitwiseCast) && countsChildrenAtFirstUse(EqualOrUnordered) && countsChildrenAtFirstUse(Select));
static_assert(!countsChildrenAtFirstUse(Load) && !countsChildrenAtFirstUse(Opaque) && !countsChildrenAtFirstUse(Extract) && !countsChildrenAtFirstUse(Phi) && !countsChildrenAtFirstUse(ArgumentReg) && !countsChildrenAtFirstUse(Patch) && !countsChildrenAtFirstUse(Check) && !countsChildrenAtFirstUse(Upsilon));

constexpr bool readsZeroExtendedWord(Opcode opcode, unsigned childIndex)
{
    if (isStore(opcode))
        return !childIndex;
    if (opcode == Select)
        return childIndex;
    return opcode == Add || opcode == Sub || opcode == Mul || opcode == BitAnd || opcode == BitOr || opcode == BitXor || opcode == Shl || opcode == SShr || opcode == ZShr || opcode == RotR || opcode == Neg || opcode == Clz || opcode == SExt8 || opcode == SExt16 || opcode == SExt32 || opcode == ZExt32 || opcode == IToD || isComparison(opcode);
}

JS_EXPORT_PRIVATE Opcode invertedComparison(Opcode);
JS_EXPORT_PRIVATE Opcode flippedComparison(Opcode);

constexpr Width accessWidthOf(Opcode opcode, Type typeOfWord)
{
    if (opcode == Load8Z || opcode == Load8S || opcode == Store8)
        return Width8;
    if (opcode == Load16Z || opcode == Load16S || opcode == Store16)
        return Width16;
    ASSERT_UNDER_CONSTEXPR_CONTEXT(opcode == Load || opcode == Store);
    return widthOf(typeOfWord);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
