/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "ArrayBufferSharingMode.h"
#include "JSType.h"
#include <optional>
#include <span>
#include <stdint.h>
#include <utility>
#include <wtf/Assertions.h>
#include <wtf/OptionSet.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

struct RuntimeCellTest {
    CellTestKind kind;
    uint8_t firstCellType;
    uint8_t lastCellType;
    uint8_t unused;
    uint32_t operand;

    static constexpr RuntimeCellTest isObject() { return { CellTestKind::IsObject, 0, 0, 0, 0 }; }
    static constexpr RuntimeCellTest isCallable() { return { CellTestKind::IsCallable, 0, 0, 0, 0 }; }
    static constexpr RuntimeCellTest cellTypeIs(JSType first, JSType last) { return { CellTestKind::CellTypeIs, static_cast<uint8_t>(first), static_cast<uint8_t>(last), 0, 0 }; }
    static constexpr RuntimeCellTest shapeIn(uint32_t shapeTestOffset) { return { CellTestKind::ShapeIn, 0, 0, 0, shapeTestOffset }; }
    static constexpr RuntimeCellTest inheritsClassInfo(BuiltinClassID builtinClass) { return { CellTestKind::InheritsClassInfo, 0, 0, 0, builtinClass.index() }; }
    static constexpr RuntimeCellTest isPristine(BuiltinClassID builtinClass) { return { CellTestKind::IsPristine, 0, 0, 0, builtinClass.index() }; }
    static constexpr RuntimeCellTest isBuiltinCell(uint32_t builtinCellNumber) { return { CellTestKind::IsBuiltinCell, 0, 0, 0, builtinCellNumber }; }
    static constexpr RuntimeCellTest isClosureOf(FunctionID function) { return { CellTestKind::IsClosureOf, 0, 0, 0, function.index() }; }
    static constexpr RuntimeCellTest isSymbol(uint32_t propertyKeyNumber) { return { CellTestKind::IsSymbol, 0, 0, 0, propertyKeyNumber }; }
    static constexpr RuntimeCellTest sharingModeIs(ArrayBufferSharingMode sharingMode) { return { CellTestKind::SharingModeIs, 0, 0, 0, static_cast<uint32_t>(sharingMode) }; }
    static constexpr RuntimeCellTest isNamespaceOf(ModuleID module) { return { CellTestKind::IsNamespaceOf, 0, 0, 0, module.index() }; }

    constexpr JSTypeRange cellTypes() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::CellTypeIs);
        return { static_cast<JSType>(firstCellType), static_cast<JSType>(lastCellType) };
    }
    constexpr uint32_t shapeTestOffset() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::ShapeIn);
        return operand;
    }
    constexpr BuiltinClassID builtinClass() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::InheritsClassInfo || kind == CellTestKind::IsPristine);
        return BuiltinClassID(operand);
    }
    constexpr uint32_t builtinCellNumber() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::IsBuiltinCell);
        return operand;
    }
    constexpr FunctionID function() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::IsClosureOf);
        return FunctionID(operand);
    }
    constexpr uint32_t symbolKeyNumber() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::IsSymbol);
        return operand;
    }
    constexpr ArrayBufferSharingMode sharingMode() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::SharingModeIs);
        return operand ? ArrayBufferSharingMode::Shared : ArrayBufferSharingMode::Default;
    }
    constexpr ModuleID module() const
    {
        ASSERT_UNDER_CONSTEXPR_CONTEXT(kind == CellTestKind::IsNamespaceOf);
        return ModuleID(operand);
    }

    static constexpr ptrdiff_t offsetOfKind() { return 0; }
    static constexpr ptrdiff_t offsetOfFirstCellType() { return 1; }
    static constexpr ptrdiff_t offsetOfLastCellType() { return 2; }
    static constexpr ptrdiff_t offsetOfOperand() { return 4; }
};
static_assert(sizeof(RuntimeCellTest) == 8 && alignof(RuntimeCellTest) == 4 && isImageRecord<RuntimeCellTest>);

struct alignas(4) RuntimeType {
    static constexpr uint8_t hasIntegerRange = 1;
    static constexpr uint8_t hasContents = 2;
    static constexpr uint8_t hasLayoutSet = 4;

    uint16_t primitives;
    uint8_t flags;
    uint8_t unused;
    uint16_t numberOfCellTests;
    uint16_t numberOfLiteralStrings;

    OptionSet<PrimitiveSet> primitiveSet() const { return OptionSet<PrimitiveSet>::fromRaw(primitives); }

    std::span<const RuntimeCellTest> cellTests() const
    {
        return { reinterpret_cast<const RuntimeCellTest*>(this + 1), numberOfCellTests };
    }

    std::span<const uint32_t> literalStrings() const
    {
        return { reinterpret_cast<const uint32_t*>(cellTests().data() + numberOfCellTests), numberOfLiteralStrings };
    }

    std::optional<std::pair<int64_t, int64_t>> integerRange() const
    {
        if (!(flags & hasIntegerRange))
            return std::nullopt;
        const uint32_t* words = optionalWords();
        return std::pair { integerFromHalves(words[0], words[1]), integerFromHalves(words[2], words[3]) };
    }

    std::optional<uint32_t> contents() const
    {
        if (!(flags & hasContents))
            return std::nullopt;
        return optionalWords()[flags & hasIntegerRange ? numberOfIntegerRangeWords : 0];
    }

    std::optional<uint32_t> layoutSet() const
    {
        if (!(flags & hasLayoutSet))
            return std::nullopt;
        return optionalWords()[(flags & hasIntegerRange ? numberOfIntegerRangeWords : 0) + (flags & hasContents ? 1 : 0)];
    }

    bool acceptsEveryObject() const
    {
        for (const RuntimeCellTest& test : cellTests()) {
            if (test.kind == CellTestKind::IsObject)
                return true;
        }
        return false;
    }

    static constexpr size_t sizeInBytes(unsigned numberOfCellTests, unsigned numberOfLiteralStrings, uint8_t flags)
    {
        size_t numberOfOptionalWords = (flags & hasIntegerRange ? numberOfIntegerRangeWords : 0) + (flags & hasContents ? 1 : 0) + (flags & hasLayoutSet ? 1 : 0);
        return sizeof(RuntimeType) + numberOfCellTests * sizeof(RuntimeCellTest) + (numberOfLiteralStrings + numberOfOptionalWords) * sizeof(uint32_t);
    }

    static constexpr ptrdiff_t offsetOfPrimitives() { return 0; }
    static constexpr ptrdiff_t offsetOfFlags() { return 2; }
    static constexpr ptrdiff_t offsetOfNumberOfCellTests() { return 4; }
    static constexpr ptrdiff_t offsetOfNumberOfLiteralStrings() { return 6; }
    static constexpr ptrdiff_t offsetOfCellTests() { return 8; }

private:
    static constexpr size_t numberOfIntegerRangeWords = 4;

    static constexpr int64_t integerFromHalves(uint32_t low, uint32_t high)
    {
        return static_cast<int64_t>(low | static_cast<uint64_t>(high) << 32);
    }

    const uint32_t* optionalWords() const
    {
        return literalStrings().data() + numberOfLiteralStrings;
    }
};
static_assert(sizeof(RuntimeType) == 8 && alignof(RuntimeType) == 4 && isImageRecord<RuntimeType>);
static_assert(RuntimeType::sizeInBytes(0, 0, 0) == 8);
static_assert(RuntimeType::sizeInBytes(0, 0, RuntimeType::hasIntegerRange) == 8 + 16);
static_assert(RuntimeType::sizeInBytes(0, 0, RuntimeType::hasContents) == 8 + 4);
static_assert(RuntimeType::sizeInBytes(2, 3, RuntimeType::hasIntegerRange | RuntimeType::hasContents | RuntimeType::hasLayoutSet) == 8 + 16 + 12 + 16 + 4 + 4);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
