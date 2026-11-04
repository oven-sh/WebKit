/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// THE FIELDS OF THE BIRTH: LinkedCell::isOfBirth. Every construction in sight has defined the field when it is through with the object: AOTScanBirthsInlines.h.
// NO PROOF, in either reading: it is what the plan lets the mark of the cell answer for (PlacedSlot::isFilledAtBirth), and any answer is safe: an object that lacks such a field keeps its mark, and typed code that meets it goes the closed way.
// It asks the summaries and the program alone, so it hangs on no order of the worklist. It grows as functions are reached, like LinkFlow::fieldsThatMayBeEmpty.

#pragma once

#if ENABLE(AOT)

// For the foot of AOTLinkLayouts.cpp alone, which declares RowOfConstructorsInLinkLayouts.

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
    for (FieldReference field : summary.fieldsLeftEmpty) {
        if (summary.fieldsFilledAtEndAlone.isEmpty() || !std::ranges::binary_search(summary.fieldsFilledAtEndAlone, field))
            cells.append(m_firstFieldOfLayout[field.layout.index()] + field.field);
    }
    for (FieldReference field : summary.fieldsEmptyAtEndAlone)
        cells.append(m_firstFieldOfLayout[field.layout.index()] + field.field);

    LayoutID made = program.at(id).constructedClass;
    if (made && program.at(made).constructor == id && !program.at(made).fields.isEmpty()) {
        const Vector<PropertyKeyID>& filled = m_listsOfKeysThatRowsOfConstructorsFill[m_keysThatRowsOfConstructorsFill[made.index()].untilEnds];
        const Layout& layout = program.at(made);
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            if (!layout.fields[field].key || !std::ranges::binary_search(filled, layout.fields[field].key))
                cells.append(m_firstFieldOfLayout[made.index()] + field);
        }
    }
    if (cells.isEmpty())
        return;
    LockerOfWhatIsShared locker(*this);
    for (uint32_t cell : cells)
        fieldsNotOfBirth.set(cell);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
