/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// A position is an index in it, and from foldIdenticalFunctions on the image function index.
class LinkedFunctions {
    WTF_MAKE_NONCOPYABLE(LinkedFunctions);
public:
    LinkedFunctions() = default;
    LinkedFunctions(LinkedFunctions&& other) { swap(other); }
    LinkedFunctions& operator=(LinkedFunctions&& other)
    {
        LinkedFunctions(WTF::move(other)).swap(*this);
        return *this;
    }
    ~LinkedFunctions() { clear(); }
    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    CompiledCode& operator[](size_t index)
    {
        RELEASE_ASSERT(index < m_size);
        return m_first[index];
    }
    const CompiledCode& operator[](size_t index) const
    {
        RELEASE_ASSERT(index < m_size);
        return m_first[index];
    }
    CompiledCode* begin() { return m_first; }
    CompiledCode* end() { return m_first + m_size; }
    const CompiledCode* begin() const { return m_first; }
    const CompiledCode* end() const { return m_first + m_size; }
    std::span<CompiledCode> mutableSpan() { return { m_first, m_size }; }
    std::span<const CompiledCode> span() const { return { m_first, m_size }; }
    void grow(size_t newSize, BuildContext&);
    void clear();
    void clearOnAllThreads(BuildContext&);
    void reorder(std::span<const uint32_t> order, BuildContext&);

private:
    void swap(LinkedFunctions&);
    void reserveCapacity(size_t);

    CompiledCode* m_first { nullptr };
    size_t m_size { 0 };
    size_t m_capacity { 0 };
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
