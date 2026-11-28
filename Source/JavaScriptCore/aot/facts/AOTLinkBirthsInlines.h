/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

void LinkFlow::noteBirthsOf(FunctionID id)
{
    // IT RUNS IN bringAlive(), ON SEVERAL THREADS: two bits of one word that are set at once lose one. The cells are gathered, and set under the lock.
    Vector<uint32_t, 32> cells;
    const ThinSummary& summary = summaries.at(id);
    if (!summary.halvesOfAccessorsNotLaid.isEmpty()) {
        LockerOfWhatIsShared locker(*this);
        for (const HalfOfAccessorInSummary& half : summary.halvesOfAccessorsNotLaid)
            (half.isGetter ? gettersThatMayBeAbsent : settersThatMayBeAbsent).set(m_firstFieldOfLayout[half.field.layout.index()] + half.field.field);
    }
    for (FieldReference field : summary.fieldsEmptyAtEndAlone)
        cells.append(m_firstFieldOfLayout[field.layout.index()] + field.field);

    LayoutID made = program.at(id).constructedClass;
    if (made && program.at(made).constructor == id && !program.at(made).fields.isEmpty()) {
        const KeysThatRowOfConstructorsFills& row = m_keysThatRowsOfConstructorsFill[made.index()];
        forEachFieldLeftEmptyByRowOfConstructors(made, row.untilEnds, row.leftEmptyUntilEnds, [&](uint32_t field) {
            cells.append(m_firstFieldOfLayout[made.index()] + field);
        });
    }
    if (cells.isEmpty() && summary.fieldsHeldOfLayoutsLeftEmpty.isEmpty())
        return;
    LockerOfWhatIsShared locker(*this);
    for (uint32_t cell : cells)
        fieldsNotOfBirth.set(cell);
    summary.forEachLayoutLeftEmpty([&](LayoutID layout, std::span<const uint32_t> fieldsHeld) {
        Vector<uint32_t>& fieldsOfBirthSoFar = m_fieldsOfBirthSoFar.ensure(layout.index(), [&] {
            return Vector<uint32_t>(program.at(layout).fields.size(), [](size_t field) { return static_cast<uint32_t>(field); });
        }).iterator->value;
        fieldsOfBirthSoFar.removeAllMatching([&](uint32_t field) {
            if (std::ranges::binary_search(fieldsHeld, field) || std::ranges::binary_search(summary.fieldsFilledAtEndAlone, FieldReference { layout, field }))
                return false;
            fieldsNotOfBirth.set(m_firstFieldOfLayout[layout.index()] + field);
            return true;
        });
    });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
