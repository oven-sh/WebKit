/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLedgerFields.h"

#if ENABLE(AOT)

#include "AOTSort.h"

namespace JSC { namespace AOT { namespace Typed {

LedgerOfFields::LedgerOfFields(const Program& program)
    : m_program(program)
    , m_firstFieldOfLayout(FillWith { }, program.layouts.size() + 1, 0u)
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index)
        m_firstFieldOfLayout[index + 1] = m_firstFieldOfLayout[index] + program.layouts[index].fields.size();
    m_open = Vector<OptionSet<Actor>>(FillWith { }, m_firstFieldOfLayout.last(), actorsThatCanWrite(PlaceKind::Field));
    m_isSignedFor = BitVector(m_firstFieldOfLayout.last());
}

void LedgerOfFields::sign(FieldReference declaration, OptionSet<Actor> open, std::span<const Write> writes)
{
    uint32_t field = numberOf(declaration);
    if (m_isSignedFor.set(field))
        m_open[field].add(open);
    else
        m_open[field] = open;
    for (const Write& write : writes)
        m_noted.append({ field, write });
}

void LedgerOfFields::note(Vector<WriteToField>&& writes)
{
    m_noted.reserveCapacity(m_noted.size() + writes.size());
    for (const WriteToField& noted : writes)
        m_noted.append({ numberOf(noted.declaration), noted.write });
}

void LedgerOfFields::close(const ParallelLoops&)
{
    AOT::stableSort(m_noted, { }, &NumberedWrite::field);
    m_firstWriteOfField = Vector<uint32_t>(FillWith { }, m_open.size() + 1, 0u);
    for (const NumberedWrite& noted : m_noted)
        ++m_firstWriteOfField[noted.field + 1];
    for (size_t field = 0; field < m_open.size(); ++field)
        m_firstWriteOfField[field + 1] += m_firstWriteOfField[field];
    m_writes = Vector<Write>(m_noted.size(), [&](size_t index) {
        return m_noted[index].write;
    });
    m_noted.clear();
}

Writers LedgerOfFields::writersOf(FieldReference declaration) const
{
    uint32_t field = numberOf(declaration);
    return { m_writes.subspan(m_firstWriteOfField[field], m_firstWriteOfField[field + 1] - m_firstWriteOfField[field]), m_open[field] };
}

std::optional<Writers> LedgerOfFields::writersOf(const Place& place) const
{
    if (place.kind() != PlaceKind::Field)
        return std::nullopt;
    return writersOf(declarationOf(m_program, { place.layout(), place.field() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
