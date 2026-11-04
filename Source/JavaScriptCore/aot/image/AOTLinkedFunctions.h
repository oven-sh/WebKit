/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTThreads.h"
#include <atomic>

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
    void makeRoomFor(size_t newSize);
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
Vector<OptionalIndex> indexByFunction(std::span<const T> elements, const ParallelLoops* loops)
{
    constexpr size_t elementsInShare = 2048;
    auto forEachShare = [&](const auto& functor) {
        if (!loops) {
            functor(0, elements.size());
            return;
        }
        forEachIndexInParallel(*loops, static_cast<unsigned>((elements.size() + elementsInShare - 1) / elementsInShare), [&](unsigned share) {
            functor(share * elementsInShare, std::min(elements.size(), (share + 1) * elementsInShare));
        });
    };
    std::atomic<uint32_t> endOfFunctions { 0 };
    forEachShare([&](size_t first, size_t end) {
        uint32_t endInShare = 0;
        for (size_t index = first; index < end; ++index) {
            RELEASE_ASSERT(elements[index].function);
            endInShare = std::max(endInShare, elements[index].function.index() + 1);
        }
        uint32_t known = endOfFunctions.load(std::memory_order_relaxed);
        while (known < endInShare && !endOfFunctions.compare_exchange_weak(known, endInShare, std::memory_order_relaxed)) { }
    });
    Vector<OptionalIndex> indices;
    indices.fill(OptionalIndex { }, endOfFunctions.load(std::memory_order_relaxed));
    forEachShare([&](size_t first, size_t end) {
        for (size_t index = first; index < end; ++index) {
            OptionalIndex& indexed = indices[elements[index].function.index()];
            RELEASE_ASSERT(!indexed);
            indexed = { static_cast<uint32_t>(index) };
        }
    });
    return indices;
}

// Valid for one order of the bodies: a step that removes or moves a body needs a new index afterwards.
class CodeIndex {
public:
    explicit CodeIndex(std::span<const CompiledCode> functions, const ParallelLoops* loops = nullptr)
        : m_positions(indexByFunction(functions, loops))
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
    explicit AliasIndex(std::span<const EntryAlias> aliases, const ParallelLoops* loops = nullptr)
        : m_aliases(indexByFunction(aliases, loops))
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
