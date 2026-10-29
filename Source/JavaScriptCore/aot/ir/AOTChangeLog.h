/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTVector.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ValueEdit : uint8_t {
    Added = 1 << 0,
    Definition = 1 << 1,
    Type = 1 << 2,
    OperandsOfSameTypes = 1 << 3,
    TypedByRule = 1 << 4,
};

enum class BlockEdit : uint16_t {
    Added = 1 << 0,
    Values = 1 << 1,
    Parameters = 1 << 2,
    Terminator = 1 << 3,
    Edges = 1 << 4,
    Arguments = 1 << 5,
    Rest = 1 << 6,
    Incoming = 1 << 7,
    OperandsOfSameTypes = 1 << 8,
};

inline constexpr OptionSet<ValueEdit> anyValueEdit { ValueEdit::Definition, ValueEdit::Type };
inline constexpr OptionSet<ValueEdit> promisesOfValueEdits { ValueEdit::OperandsOfSameTypes, ValueEdit::TypedByRule };
inline constexpr OptionSet<BlockEdit> promisesOfBlockEdits { BlockEdit::OperandsOfSameTypes };
inline constexpr OptionSet<BlockEdit> anyBlockEdit { BlockEdit::Values, BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest };

struct ValueChange {
    ValueID value;
    OptionSet<ValueEdit> edits;
    TypeID typeBefore;
    TypeID expectedBefore;
};

struct BlockChange {
    BlockID block;
    OptionSet<BlockEdit> edits;
};

class ChangeLog {
    WTF_MAKE_NONCOPYABLE(ChangeLog);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ChangeLog);
public:
    ChangeLog() = default;

    class Cursor {
    public:
        Cursor() = default;

    private:
        friend class ChangeLog;
        Cursor(uint32_t generation, uint32_t values, uint32_t blocks)
            : m_generation(generation)
            , m_values(values)
            , m_blocks(blocks)
        {
        }

        uint32_t m_generation { 0 };
        uint32_t m_values { 0 };
        uint32_t m_blocks { 0 };
    };

    Cursor end() const
    {
        didShowEntries();
        return { m_generation, static_cast<uint32_t>(m_values.size()), static_cast<uint32_t>(m_blocks.size()) };
    }

    void settle()
    {
        m_numberOfSettledValues = m_values.size();
        m_numberOfSettledBlocks = m_blocks.size();
    }

    bool hasForgottenSince(const Cursor& cursor) const { return cursor.m_generation != m_generation; }
    bool hasChangesSince(const Cursor& cursor) const { return hasForgottenSince(cursor) || cursor.m_values != m_values.size() || cursor.m_blocks != m_blocks.size(); }
    size_t numberOfEntriesSince(const Cursor& cursor) const
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        return m_values.size() - cursor.m_values + m_blocks.size() - cursor.m_blocks;
    }

    std::span<const ValueChange> valuesSince(const Cursor& cursor) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        didShowEntries();
        return m_values.span().subspan(cursor.m_values);
    }
    std::span<const BlockChange> blocksSince(const Cursor& cursor) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        didShowEntries();
        return m_blocks.span().subspan(cursor.m_blocks);
    }

    // An entry in front of this cursor takes up no later edit. A reader that shows nothing goes on from here, and reads again what stands behind it.
    Cursor endOfShownEntries() const { return { m_generation, static_cast<uint32_t>(m_numberOfShownValues), static_cast<uint32_t>(m_numberOfShownBlocks) }; }
    std::span<const ValueChange> valuesSinceWithoutShowing(const Cursor& cursor) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        return m_values.span().subspan(cursor.m_values);
    }
    std::span<const BlockChange> blocksSinceWithoutShowing(const Cursor& cursor) const LIFETIME_BOUND
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        return m_blocks.span().subspan(cursor.m_blocks);
    }

    bool wasAddedAndShownToNobody(BlockID block) const { return block.index() >= m_firstBlockShownToNobody; }

    // A pass wrote this value's type, and its users have not been retyped yet.
    bool hasUnpropagatedType(ValueID value) const { return m_valuesWithUnpropagatedType.get(value.index()); }
    void markTypeAsUnpropagated(ValueID value)
    {
        size_t size = m_valuesWithUnpropagatedType.size();
        if (value.index() >= size) [[unlikely]]
            m_valuesWithUnpropagatedType.ensureSize(std::max<size_t>(value.index() + 1, size * 2));
        m_valuesWithUnpropagatedType.quickSet(value.index());
    }
    void didPropagateTypes() { m_valuesWithUnpropagatedType.clearAll(); }

    ALWAYS_INLINE void didEdit(ValueID value, OptionSet<ValueEdit> edits, TypeID typeBefore, TypeID expectedBefore)
    {
        if (edits.contains(ValueEdit::Type) && !edits.contains(ValueEdit::Added))
            markTypeAsUnpropagated(value);
        if (edits.contains(ValueEdit::Added) || value.index() >= m_firstValueShownToNobody) {
            didAddOrEditWhatNobodyWasShown(ValueChange { value, edits, typeBefore, expectedBefore });
            return;
        }
        size_t size = m_values.size();
        if (size > m_numberOfShownValues) {
            ValueChange& last = m_values.mutableSpan().data()[size - 1];
            if (last.value == value && !(last.edits ^ edits).containsAny(promisesOfValueEdits)) {
                last.edits.add(edits);
                return;
            }
        }
        if (size == m_values.capacity()) [[unlikely]] {
            makeRoomAndAdd(ValueChange { value, edits, typeBefore, expectedBefore });
            return;
        }
        m_values.unsafeAppendWithoutCapacityCheck(ValueChange { value, edits, typeBefore, expectedBefore });
    }

    ALWAYS_INLINE void didEdit(BlockID block, OptionSet<BlockEdit> edits)
    {
        if (edits.contains(BlockEdit::Added))
            m_firstBlockShownToNobody = std::min(m_firstBlockShownToNobody, block.index());
        else if (block.index() >= m_firstBlockShownToNobody)
            return;
        size_t size = m_blocks.size();
        if (size > m_numberOfShownBlocks) {
            BlockChange& last = m_blocks.mutableSpan().data()[size - 1];
            if (last.block == block && !(last.edits ^ edits).containsAny(promisesOfBlockEdits)) {
                last.edits.add(edits);
                return;
            }
        }
        if (size == m_blocks.capacity()) [[unlikely]] {
            makeRoomAndAdd(block, edits);
            return;
        }
        m_blocks.unsafeAppendWithoutCapacityCheck(BlockChange { block, edits });
    }

    void forget()
    {
        ++m_generation;
        m_values.shrink(0);
        m_blocks.shrink(0);
        m_numberOfShownValues = 0;
        m_numberOfShownBlocks = 0;
        m_numberOfSettledValues = 0;
        m_numberOfSettledBlocks = 0;
        m_valuesWithUnpropagatedType.clearAll();
        didShowEntries();
    }

    void setMaxNumberOfEntries(size_t max) { m_maxNumberOfEntries = max; }

private:
    NEVER_INLINE void didAddOrEditWhatNobodyWasShown(const ValueChange& change)
    {
        if (!change.edits.contains(ValueEdit::Added)) {
            m_values[m_entriesOfValuesShownToNobody[change.value.index() - m_firstValueShownToNobody]].edits.remove(ValueEdit::TypedByRule);
            return;
        }
        m_firstValueShownToNobody = std::min(m_firstValueShownToNobody, change.value.index());
        uint32_t generation = m_generation;
        uint32_t entry = m_values.size();
        if (entry == m_values.capacity())
            makeRoomAndAdd(change);
        else
            m_values.unsafeAppendWithoutCapacityCheck(change);
        if (m_generation == generation)
            m_entriesOfValuesShownToNobody.append(entry);
    }

    NEVER_INLINE void makeRoomAndAdd(const ValueChange& change)
    {
        if (m_values.size() >= m_maxNumberOfEntries) {
            forget();
            return;
        }
        m_values.append(change);
    }

    NEVER_INLINE void makeRoomAndAdd(BlockID block, OptionSet<BlockEdit> edits)
    {
        if (m_blocks.size() >= m_maxNumberOfEntries) {
            forget();
            return;
        }
        m_blocks.append(BlockChange { block, edits });
    }

    void didShowEntries() const
    {
        m_numberOfShownValues = m_values.size();
        m_numberOfShownBlocks = m_blocks.size();
        m_firstValueShownToNobody = std::numeric_limits<uint32_t>::max();
        m_firstBlockShownToNobody = std::numeric_limits<uint32_t>::max();
        m_entriesOfValuesShownToNobody.shrink(0);
    }

    Vector<ValueChange, 8> m_values;
    Vector<BlockChange, 8> m_blocks;
    BitVector m_valuesWithUnpropagatedType;
    mutable size_t m_numberOfShownValues { 0 };
    mutable size_t m_numberOfShownBlocks { 0 };
    mutable uint32_t m_firstValueShownToNobody { std::numeric_limits<uint32_t>::max() };
    mutable uint32_t m_firstBlockShownToNobody { std::numeric_limits<uint32_t>::max() };
    mutable Vector<uint32_t, 8> m_entriesOfValuesShownToNobody;
    size_t m_numberOfSettledValues { 0 };
    size_t m_numberOfSettledBlocks { 0 };
    size_t m_maxNumberOfEntries { 1 << 16 };
    uint32_t m_generation { 1 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
