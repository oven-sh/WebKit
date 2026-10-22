/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ValueEdit : uint8_t {
    // The function has one value more.
    Added = 1 << 0,
    // Opcode, operands, payload, flags, origin.
    Definition = 1 << 1,
    // Type, expected type, representation.
    Type = 1 << 2,
    // Beside Definition, a promise: nothing but operands has changed, and each has the types of the one it replaces.
    OperandsOfSameTypes = 1 << 3,
    // Beside Added, a promise: the value has the types that its rule gives.
    TypedByRule = 1 << 4,
};

enum class BlockEdit : uint16_t {
    // As for a value. Its targets have no entry either.
    Added = 1 << 0,
    // Which values the block lists, and in which order.
    Values = 1 << 1,
    Parameters = 1 << 2,
    // Kind, operands and payload of the terminator.
    Terminator = 1 << 3,
    // How many targets there are, which block a target or the handler names, whether there is a handler.
    Edges = 1 << 4,
    // Arguments and proofs along an edge.
    Arguments = 1 << 5,
    // Subject, role, frequency, shape.
    Rest = 1 << 6,
    // A block that named this one as a target or as its handler was handed out for an edit of its edges: this one may have lost that edge.
    Incoming = 1 << 7,
    // Beside Terminator, Arguments and Rest, a promise: nothing but operands of the terminator, arguments and the subject has changed, as for ValueEdit::OperandsOfSameTypes.
    OperandsOfSameTypes = 1 << 8,
};

inline constexpr OptionSet<ValueEdit> anyValueEdit { ValueEdit::Definition, ValueEdit::Type };
// An entry with a promise takes in no edit without it.
inline constexpr OptionSet<ValueEdit> promisesOfValueEdits { ValueEdit::OperandsOfSameTypes, ValueEdit::TypedByRule };
inline constexpr OptionSet<BlockEdit> promisesOfBlockEdits { BlockEdit::OperandsOfSameTypes };
inline constexpr OptionSet<BlockEdit> anyBlockEdit { BlockEdit::Values, BlockEdit::Parameters, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments, BlockEdit::Rest };

struct ValueChange {
    ValueID value;
    OptionSet<ValueEdit> edits;
    // As they were when the reference was handed out. Of several entries for one value, the first behind a cursor says what held at the cursor.
    TypeID typeBefore;
    TypeID expectedBefore;
};

struct BlockChange {
    BlockID block;
    OptionSet<BlockEdit> edits;
};

// What has been edited in one function, in the order of the edits.
class ChangeLog {
    WTF_MAKE_NONCOPYABLE(ChangeLog);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ChangeLog);
public:
    ChangeLog() = default;

    // A cursor that nobody has set has seen nothing: hasForgottenSince() holds for it.
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

    // An entry says that a reference for writing was handed out, not that the writing is over. Between two runs of passes no such reference is left, and the runner says so.
    void settle()
    {
        m_numberOfSettledValues = m_values.size();
        m_numberOfSettledBlocks = m_blocks.size();
    }
    // The cursor for a reader that may be asked while a pass holds such a reference: it reads what is not settled again the next time.
    Cursor endOfSettledEntries() const
    {
        didShowEntries();
        return { m_generation, static_cast<uint32_t>(m_numberOfSettledValues), static_cast<uint32_t>(m_numberOfSettledBlocks) };
    }

    // The entries behind the cursor are gone: blocks and values were renumbered, the body was replaced, or there were more edits than a walk of the function costs.
    bool hasForgottenSince(const Cursor& cursor) const { return cursor.m_generation != m_generation; }
    bool hasChangesSince(const Cursor& cursor) const { return hasForgottenSince(cursor) || cursor.m_values != m_values.size() || cursor.m_blocks != m_blocks.size(); }
    size_t numberOfEntriesSince(const Cursor& cursor) const
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        return m_values.size() - cursor.m_values + m_blocks.size() - cursor.m_blocks;
    }

    // Good until the next edit of the function. One value or block may come several times.
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

    // One entry at a time, for a reader that edits the function while it reads: its own edits come in their turn. None where the cursor stands at the end.
    std::optional<ValueChange> takeValueChange(Cursor& cursor) const
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        if (cursor.m_values == m_values.size())
            return std::nullopt;
        didShowEntries();
        return m_values[cursor.m_values++];
    }
    std::optional<BlockChange> takeBlockChange(Cursor& cursor) const
    {
        RELEASE_ASSERT(!hasForgottenSince(cursor));
        if (cursor.m_blocks == m_blocks.size())
            return std::nullopt;
        didShowEntries();
        return m_blocks[cursor.m_blocks++];
    }

    bool wasAddedAndShownToNobody(BlockID block) const { return block.index() >= m_firstBlockShownToNobody; }

    ALWAYS_INLINE void didEdit(ValueID value, OptionSet<ValueEdit> edits, TypeID typeBefore, TypeID expectedBefore)
    {
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

    // An entry that a reader may have seen takes no further edit: the next edit of the same value is an entry of its own.
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
