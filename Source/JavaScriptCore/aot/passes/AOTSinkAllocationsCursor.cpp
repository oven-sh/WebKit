/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInterpreter.h"

#if ENABLE(AOT)

#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

void ObjectValues::setField(uint32_t index, ValueID value)
{
    if (index >= fields.size())
        fields.grow(static_cast<size_t>(index) + 1);
    fields[index] = value;
}

ObjectValues& ObjectValueMap::ensure(VirtualObjectID object)
{
    size_t position = std::ranges::lower_bound(m_entries, object, { }, &Entry::object) - m_entries.begin();
    if (position == m_entries.size() || m_entries[position].object != object)
        m_entries.insert(position, Entry { object, { } });
    return m_entries[position].values;
}

const ObjectValues* ObjectValueMap::find(VirtualObjectID object) const
{
    const Entry* entry = std::ranges::lower_bound(m_entries, object, { }, &Entry::object);
    if (entry == m_entries.end() || entry->object != object)
        return nullptr;
    return &entry->values;
}

void ObjectValueMap::remove(VirtualObjectID object)
{
    size_t position = std::ranges::lower_bound(m_entries, object, { }, &Entry::object) - m_entries.begin();
    if (position < m_entries.size() && m_entries[position].object == object)
        m_entries.removeAt(position);
}

void ObjectValueMap::move(VirtualObjectID from, VirtualObjectID to)
{
    if (from == to)
        return;

    ObjectValues values;
    if (const ObjectValues* source = find(from))
        values = *source;
    remove(from);
    ensure(to) = WTF::move(values);
}

RewriteCursor::RewriteCursor(AllocationSinking& sinking, IRBuilder& builder)
    : m_sinking(sinking)
    , m_builder(builder)
    , m_function(builder.function())
{
    m_builder.setHandler(std::nullopt);
}

void RewriteCursor::beginBlock(BlockID original, std::optional<Target>&& handler, ObjectInterpreter& interpreter)
{
    m_originalBlock = original;
    m_originalHandler = WTF::move(handler);
    m_frequency = m_function.at(original).frequency;
    m_handlerArgumentsAreStale = false;
    m_builder.moveToEndOf(original);
    if (!m_originalHandler)
        return;

    if (original == entryBlock) {
        cut(interpreter);
        return;
    }
    std::optional<Target> target = handlerTargetFor(interpreter);
    m_function.edit(original, { BlockEdit::Edges, BlockEdit::Arguments }).handler = WTF::move(target);
}

void RewriteCursor::finishBlock(Terminator&& terminator)
{
    m_builder.setOrigin(terminator.origin());
    m_builder.emitTerminator(WTF::move(terminator));
}

ValueID RewriteCursor::emit(Value&& value)
{
    return m_builder.append(WTF::move(value));
}

void RewriteCursor::keep(ValueID id)
{
    m_function.edit(block(), BlockEdit::Values).values.append(id);
}

ValueID RewriteCursor::takeConditionOf(const Terminator& terminator)
{
    if (terminator.kind != TerminatorKind::Branch)
        return ValueID();

    ValueID condition = terminator.conditionOperand();
    BlockValues& values = m_function.edit(block(), BlockEdit::Values).values;
    if (!m_sinking.inputs().conditionsOnlyBranchedOn.get(condition.index()) || values.isEmpty() || values.last() != condition)
        return ValueID();
    values.removeLast();
    return condition;
}

void RewriteCursor::willKeepThrowingValue(ObjectInterpreter& interpreter)
{
    if (m_originalHandler && m_handlerArgumentsAreStale)
        cut(interpreter);
}

void RewriteCursor::cut(ObjectInterpreter& interpreter)
{
    BlockID next = addCoveredBlock(interpreter);
    m_builder.emitJump(next);
    moveTo(next);
}

BlockID RewriteCursor::addUncoveredBlock()
{
    BlockID block = m_builder.addBlock(m_frequency);
    m_function.edit(block, BlockEdit::Rest).shape = m_function.at(m_originalBlock).shape;
    return block;
}

BlockID RewriteCursor::addCoveredBlock(ObjectInterpreter& interpreter)
{
    BlockID block = addUncoveredBlock();
    std::optional<Target> target = handlerTargetFor(interpreter);
    m_function.edit(block, { BlockEdit::Edges, BlockEdit::Arguments }).handler = WTF::move(target);
    return block;
}

void RewriteCursor::moveTo(BlockID block)
{
    m_builder.moveToEndOf(block);
    m_handlerArgumentsAreStale = !m_function.at(block).handler;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
