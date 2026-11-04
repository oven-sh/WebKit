/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCompiledCode.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <iterator>
#include <optional>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

// A generic call entry that something mentions and that is fused into its target: CallingConventions::hasFusedCallEntry(). It has a closure number and no CompiledCode.
struct FusedCallEntry {
    Typed::CodeUnit unit;
    // CallingConventions::indexOf(unit) and placeInCodeOf(unit, unitIndex).
    uint32_t unitIndex;
    uint32_t place;
    // CompiledCode::placeInCode of the target.
    uint32_t placeOfTarget;
    EntryDescription entry;
};

// Records that BlocksOfUnits::keep() has answered, in the order in which they were appended. The list owns none of them.
class ListOfKeptCode {
public:
    template<typename Code>
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = CompiledCode;
        using difference_type = ptrdiff_t;
        using pointer = Code*;
        using reference = Code&;

        Iterator() = default;
        explicit Iterator(CompiledCode* const* place)
            : m_place(place)
        {
        }

        Code& operator*() const { return **m_place; }
        Code* operator->() const { return *m_place; }
        Iterator& operator++()
        {
            ++m_place;
            return *this;
        }
        Iterator operator++(int) { return Iterator(m_place++); }
        friend bool operator==(Iterator, Iterator) = default;

    private:
        CompiledCode* const* m_place { nullptr };
    };

    ListOfKeptCode() = default;
    explicit ListOfKeptCode(Vector<CompiledCode*>&& code)
        : m_code(WTF::move(code))
    {
    }

    size_t size() const { return m_code.size(); }
    bool isEmpty() const { return m_code.isEmpty(); }
    CompiledCode& operator[](size_t index) { return *m_code[index]; }
    const CompiledCode& operator[](size_t index) const { return *m_code[index]; }
    Iterator<CompiledCode> begin() { return Iterator<CompiledCode>(m_code.begin()); }
    Iterator<CompiledCode> end() { return Iterator<CompiledCode>(m_code.end()); }
    Iterator<const CompiledCode> begin() const { return Iterator<const CompiledCode>(m_code.begin()); }
    Iterator<const CompiledCode> end() const { return Iterator<const CompiledCode>(m_code.end()); }

    void append(CompiledCode* code) { m_code.append(code); }
    void clear() { m_code.clear(); }

private:
    Vector<CompiledCode*> m_code;
};

// What Compile hands on. The records ascend by placeInCode, `placesInCode` are theirs, and they lie in the blocks.
struct CodeInBlocks {
    ListOfKeptCode code;
    Vector<uint32_t> placesInCode;
    Vector<BlocksOfUnits> blocks;
};

// The code of every unit that was compiled, ascending by CompiledCode::placeInCode. That is the order of the functions of the program, with the units of one function side by side, and the order in which the code lies in the image.
class LinkedFunctions {
    WTF_MAKE_NONCOPYABLE(LinkedFunctions);
public:
    LinkedFunctions() = default;
    // `fusedCallEntries` ascend by place.
    explicit LinkedFunctions(CodeInBlocks&& functions, Vector<FusedCallEntry>&& fusedCallEntries = { })
        : m_functions(WTF::move(functions.code))
        , m_placesInCode(WTF::move(functions.placesInCode))
        , m_blocks(WTF::move(functions.blocks))
        , m_fusedCallEntries(WTF::move(fusedCallEntries))
    {
        RELEASE_ASSERT(m_placesInCode.size() == m_functions.size());
    }
    explicit LinkedFunctions(Vector<CompiledCode>&& functions, Vector<FusedCallEntry>&& fusedCallEntries = { })
        : LinkedFunctions(keepInBlocks(WTF::move(functions)), WTF::move(fusedCallEntries))
    {
    }
    LinkedFunctions(LinkedFunctions&&) = default;
    LinkedFunctions& operator=(LinkedFunctions&& other)
    {
        if (this != &other) {
            this->~LinkedFunctions();
            new (NotNull, this) LinkedFunctions(WTF::move(other));
        }
        return *this;
    }
    ~LinkedFunctions()
    {
        for (CompiledCode& code : m_functions)
            code.~CompiledCode();
    }

    size_t size() const { return m_functions.size(); }
    bool isEmpty() const { return m_functions.isEmpty(); }
    CompiledCode& operator[](size_t index) { return m_functions[index]; }
    const CompiledCode& operator[](size_t index) const { return m_functions[index]; }
    auto begin() { return m_functions.begin(); }
    auto end() { return m_functions.end(); }
    auto begin() const { return m_functions.begin(); }
    auto end() const { return m_functions.end(); }
    std::span<const BlocksOfUnits> blocks() const { return m_blocks.span(); }

    // On all threads.
    void clear(const ParallelLoops& loops)
    {
        forEachRangeOfIndicesInParallel(loops, size(), [&](size_t first, size_t end) {
            for (size_t index = first; index < end; ++index)
                m_functions[index].~CompiledCode();
        });
        m_functions.clear();
        forEachIndexInParallel(loops, static_cast<unsigned>(m_blocks.size()), [&](unsigned index) {
            m_blocks[index] = { };
        });
        *this = { };
    }

    std::span<const FusedCallEntry> fusedCallEntries() const { return m_fusedCallEntries.span(); }

    // The place of the code that whoever names the unit enters, by binary search. A fused call entry is entered at Typed::fusedCallEntry of that code, any other unit at its primary entry.
    std::optional<uint32_t> positionOfUnit(Typed::CodeUnit unit, uint32_t unitIndex) const
    {
        uint32_t place = placeInCodeOf(unit, unitIndex);
        if (unit.role == Typed::CodeRole::GenericCallEntry) {
            auto fused = std::ranges::lower_bound(m_fusedCallEntries, place, { }, &FusedCallEntry::place);
            if (fused != m_fusedCallEntries.end() && fused->place == place)
                place = fused->placeOfTarget;
        }
        return positionOfCodeAt(place);
    }

    std::optional<uint32_t> positionOfCodeAt(uint32_t place) const
    {
        auto found = std::ranges::lower_bound(m_placesInCode, place);
        if (found == m_placesInCode.end() || *found != place)
            return std::nullopt;
        return static_cast<uint32_t>(found - m_placesInCode.begin());
    }

private:
    static CodeInBlocks keepInBlocks(Vector<CompiledCode>&& functions)
    {
        CodeInBlocks kept;
        kept.blocks.grow(1);
        for (CompiledCode& code : functions) {
            kept.placesInCode.append(code.placeInCode);
            kept.code.append(kept.blocks[0].keep(WTF::move(code)));
        }
        return kept;
    }

    ListOfKeptCode m_functions;
    Vector<uint32_t> m_placesInCode;
    Vector<BlocksOfUnits> m_blocks;
    Vector<FusedCallEntry> m_fusedCallEntries;
};

// The positions of the callees of each function. None for a callee without code, which only an inline frame or a site names.
struct ResolvedCallees {
    OptionalIndex at(uint32_t position, uint32_t callee) const { return positions[firstCallee[position] + callee]; }

    // [position], and one more for the end.
    Vector<uint32_t> firstCallee;
    Vector<OptionalIndex> positions;
};

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

Vector<OptionalIndex> indexByFunction(const auto& elements, const ParallelLoops* loops)
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
    explicit CodeIndex(const LinkedFunctions& functions, const ParallelLoops* loops = nullptr)
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

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
