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
    m_isSignedFor = Vector<bool>(FillWith { }, m_firstFieldOfLayout.last(), false);
}

void LedgerOfFields::sign(Notes& notes, FieldReference declaration, OptionSet<Actor> open, std::span<const Write> writes)
{
    uint32_t field = numberOf(declaration);
    if (std::exchange(m_isSignedFor[field], true))
        m_open[field].add(open);
    else
        m_open[field] = open;
    for (const Write& write : writes)
        notes.append({ field, write });
}

void LedgerOfFields::close(const ParallelLoops& loops, Vector<Notes>&& lists)
{
    ParallelArray<NumberedWrite> noted = ParallelArray<NumberedWrite>::concatenationOf(loops, WTF::move(lists));
    uint32_t size = noted.size();
    constexpr uint32_t writesPerShare = 1 << 12;
    auto forEachPositionInParallel = [&](const Invocable<void(uint32_t)> auto& functor) {
        forEachIndexInParallel(loops, (size + writesPerShare - 1) / writesPerShare, [&](unsigned share) {
            for (uint32_t position = share * writesPerShare; position < std::min(size, (share + 1) * writesPerShare); ++position)
                functor(position);
        });
    };
    Vector<uint64_t> order = createVectorWithCapacityInParallel<uint64_t>(loops, size);
    order.grow(size);
    forEachPositionInParallel([&](uint32_t position) {
        order[position] = static_cast<uint64_t>(noted[position].field) << 32 | position;
    });
    parallelSort(loops, order.mutableSpan(), std::less<uint64_t>());
    m_firstWriteOfField = Vector<uint32_t>(FillWith { }, m_open.size() + 1, size);
    forEachPositionInParallel([&](uint32_t position) {
        for (uint64_t field = position ? (order[position - 1] >> 32) + 1 : 0; field <= order[position] >> 32; ++field)
            m_firstWriteOfField[field] = position;
    });
    m_writes = ParallelArray<Write>(loops, size, [&](unsigned position) {
        return noted[static_cast<uint32_t>(order[position])].write;
    });
}

Writers LedgerOfFields::writersOf(FieldReference declaration) const
{
    uint32_t field = numberOf(declaration);
    return { m_writes.span().subspan(m_firstWriteOfField[field], m_firstWriteOfField[field + 1] - m_firstWriteOfField[field]), m_open[field] };
}

std::optional<Writers> LedgerOfFields::writersOf(const Place& place) const
{
    if (place.kind() != PlaceKind::Field)
        return std::nullopt;
    return writersOf(declarationOf(m_program, { place.layout(), place.field() }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
