/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTIRBuilder.h"
#include "AOTPass.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ChainLinkKind : uint8_t { None, Map, Filter };
enum class ChainEndKind : uint8_t { Array, Length, ForEach, Reduce, Some, Every, Find, FindIndex, Join, FlatMap };
enum class ChainMethodRole : uint8_t { Range, Link, End };

struct ChainMethod {
    BuiltinID builtin;
    ChainMethodRole role { ChainMethodRole::End };
    ChainLinkKind link { ChainLinkKind::None };
    ChainEndKind end { ChainEndKind::Array };
    bool takesCallback { true };
    bool stopsEarly { false };
};

std::span<const ChainMethod> chainMethods();
const ChainMethod* findChainMethod(BuiltinID);

struct ChainShape {
    static constexpr unsigned maxNumberOfLinks = 3;
    static constexpr unsigned endWidth = 4;
    static constexpr unsigned linkWidth = 2;
    static constexpr unsigned hasInitialValueShift = endWidth + maxNumberOfLinks * linkWidth;

    constexpr unsigned numberOfLinks() const
    {
        unsigned count = 0;
        while (count < maxNumberOfLinks && links[count] != ChainLinkKind::None)
            ++count;
        return count;
    }

    constexpr uint32_t encode() const
    {
        uint32_t word = static_cast<uint32_t>(end) | static_cast<uint32_t>(hasInitialValue) << hasInitialValueShift;
        for (unsigned i = 0; i < maxNumberOfLinks; ++i)
            word |= static_cast<uint32_t>(links[i]) << (endWidth + i * linkWidth);
        return word;
    }

    static constexpr ChainShape decode(uint32_t word)
    {
        ChainShape shape;
        shape.end = static_cast<ChainEndKind>(word & ((1u << endWidth) - 1));
        shape.hasInitialValue = word >> hasInitialValueShift & 1;
        for (unsigned i = 0; i < maxNumberOfLinks; ++i)
            shape.links[i] = static_cast<ChainLinkKind>(word >> (endWidth + i * linkWidth) & ((1u << linkWidth) - 1));
        return shape;
    }

    friend constexpr bool operator==(const ChainShape&, const ChainShape&) = default;

    std::array<ChainLinkKind, maxNumberOfLinks> links { };
    ChainEndKind end { ChainEndKind::Array };
    bool hasInitialValue { false };
};

constexpr bool endTakesCallback(ChainEndKind end)
{
    switch (end) {
    case ChainEndKind::Array:
    case ChainEndKind::Length:
    case ChainEndKind::Join:
        return false;
    case ChainEndKind::ForEach:
    case ChainEndKind::Reduce:
    case ChainEndKind::Some:
    case ChainEndKind::Every:
    case ChainEndKind::Find:
    case ChainEndKind::FindIndex:
    case ChainEndKind::FlatMap:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool endTakesThisArgument(ChainEndKind end)
{
    return endTakesCallback(end) && end != ChainEndKind::Reduce;
}

constexpr bool endTakesOperand(const ChainShape& shape)
{
    return shape.end == ChainEndKind::Join || (shape.end == ChainEndKind::Reduce && shape.hasInitialValue);
}

struct ArrayMethodCall {
    const ChainMethod* method { nullptr };
    ValueID call;
    ValueID receiver;
    Vector<ValueID, 2> arguments;

    ValueID argumentOrNull(unsigned index) const { return index < arguments.size() ? arguments[index] : ValueID(); }
};

std::optional<ArrayMethodCall> arrayMethodCallOf(const Program&, const Function&, ValueID);

struct ChainCallback {
    ValueID call;
    ValueID function;
    ValueID thisArgument;
    FunctionID callee;
};

struct ArrayChain {
    ChainShape shape;
    BlockID block;
    ValueID source;
    ValueID rangeCall;
    ValueID from;
    ValueID to;
    Vector<ChainCallback, ChainShape::maxNumberOfLinks + 1> callbacks;
    ValueID endOperand;
    ValueID lastCall;

    ValueID firstCall() const { return rangeCall ? rangeCall : callbacks.isEmpty() ? lastCall : callbacks[0].call; }
};

Vector<ArrayChain> findArrayChains(FunctionPassContext&);
ArrayChainDeclineReason checkInterleaving(FunctionPassContext&, const ArrayChain&);
void rewriteArrayChain(FunctionPassContext&, IRBuilder&, const ArrayChain&);

FunctionID findChainTraversal(const Program&);

static_assert(ChainShape::decode(ChainShape { { ChainLinkKind::Filter, ChainLinkKind::Map, ChainLinkKind::None }, ChainEndKind::Reduce, true }.encode()).numberOfLinks() == 2);
static_assert(static_cast<unsigned>(ChainEndKind::FlatMap) < 1u << ChainShape::endWidth && static_cast<unsigned>(ChainLinkKind::Filter) < 1u << ChainShape::linkWidth);

// array-traversal.ts repeats these numbers.
static_assert(ChainShape { { ChainLinkKind::Map, ChainLinkKind::Filter, ChainLinkKind::Map }, ChainEndKind::FlatMap, true }.encode() == (9u | 1u << 4 | 2u << 6 | 1u << 8 | 1u << 10));
static_assert(static_cast<unsigned>(ChainEndKind::Array) == 0 && static_cast<unsigned>(ChainEndKind::Length) == 1 && static_cast<unsigned>(ChainEndKind::ForEach) == 2);
static_assert(static_cast<unsigned>(ChainEndKind::Reduce) == 3 && static_cast<unsigned>(ChainEndKind::Some) == 4 && static_cast<unsigned>(ChainEndKind::Every) == 5);
static_assert(static_cast<unsigned>(ChainEndKind::Find) == 6 && static_cast<unsigned>(ChainEndKind::FindIndex) == 7 && static_cast<unsigned>(ChainEndKind::Join) == 8);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
