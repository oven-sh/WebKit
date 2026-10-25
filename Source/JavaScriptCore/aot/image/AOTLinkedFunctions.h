/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTThreads.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// A position is an index in it, and from foldIdenticalFunctions on the image function index.
class LinkedFunctions : public ParallelArray<CompiledCode> {
public:
    using ParallelArray::ParallelArray;

    size_t size() const { return span().size(); }
    bool isEmpty() const { return !size(); }
    CompiledCode& operator[](size_t index)
    {
        RELEASE_ASSERT(index < size());
        return mutableSpan()[index];
    }
    const CompiledCode& operator[](size_t index) const
    {
        RELEASE_ASSERT(index < size());
        return span()[index];
    }
    CompiledCode* begin() { return mutableSpan().data(); }
    CompiledCode* end() { return begin() + size(); }
    const CompiledCode* begin() const { return span().data(); }
    const CompiledCode* end() const { return begin() + size(); }
    void grow(size_t newSize, BuildContext&);
    void clearOnAllThreads(BuildContext&);
    void reorder(std::span<const uint32_t> order, BuildContext&);
};

enum class AliasPlacement : uint8_t {
    AtPrimaryEntry,
    BeforeBody,
    AfterBody,
};

struct EntryAlias {
    std::optional<uint32_t> innerEntryOffset() const
    {
        switch (placement) {
        case AliasPlacement::AtPrimaryEntry:
            return std::nullopt;
        case AliasPlacement::BeforeBody:
        case AliasPlacement::AfterBody:
            return start + prefixSize;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    FunctionID function;
    FunctionID body;
    EntryDescription entry;
    AliasPlacement placement { AliasPlacement::AtPrimaryEntry };
    uint32_t start { 0 };
    uint32_t prefixSize { 0 };
};

struct FoldedFunction {
    FunctionID function;
    FunctionID canonicalFunction;
    uint32_t size;
};

struct ResolvedCallee {
    OptionalIndex body;
    OptionalIndex alias;
};

struct ResolvedCallees {
    ResolvedCallee at(uint32_t position, uint32_t callee) const { return callees[position][callee]; }

    Vector<Vector<ResolvedCallee, 0>> callees;
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

template<typename T>
Vector<OptionalIndex> indexByFunction(std::span<const T> elements)
{
    uint32_t endOfFunctions = 0;
    for (const T& element : elements) {
        RELEASE_ASSERT(element.function);
        endOfFunctions = std::max(endOfFunctions, element.function.index() + 1);
    }
    Vector<OptionalIndex> indices;
    indices.fill(OptionalIndex { }, endOfFunctions);
    uint32_t index = 0;
    for (const T& element : elements) {
        OptionalIndex& indexed = indices[element.function.index()];
        RELEASE_ASSERT(!indexed);
        indexed = { index++ };
    }
    return indices;
}

// Valid for one order of the bodies: a step that removes or moves a body needs a new index afterwards.
class CodeIndex {
public:
    explicit CodeIndex(std::span<const CompiledCode> functions)
        : m_positions(indexByFunction(functions))
    {
    }

    std::optional<uint32_t> indexOf(FunctionID function) const
    {
        return function.index() < m_positions.size() && m_positions[function.index()] ? std::optional<uint32_t>(*m_positions[function.index()]) : std::nullopt;
    }

private:
    Vector<OptionalIndex> m_positions;
};

// Valid for one list of aliases: a step that adds, removes or moves an alias needs a new index afterwards.
class AliasIndex {
public:
    explicit AliasIndex(std::span<const EntryAlias> aliases)
        : m_aliases(indexByFunction(aliases))
    {
    }

    OptionalIndex aliasOf(FunctionID function) const
    {
        return function.index() < m_aliases.size() ? m_aliases[function.index()] : OptionalIndex { };
    }

private:
    Vector<OptionalIndex> m_aliases;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
