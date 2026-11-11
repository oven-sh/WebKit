/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTCallingConventions.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT {

std::array<uint32_t, 4> keyOfClassGuardUnderMethodTableRow(const Typed::MethodFamily& family, const Typed::MethodTableRow& row)
{
    return { row.guardedClass.index(), static_cast<uint32_t>(ClassSide::Prototype), static_cast<uint32_t>(NumberSpace::Name), family.key.index() };
}

void encodeMethodTableDependents(TableEncoder& encoder)
{
    const Typed::CallingConventions& conventions = encoder.plan.conventions();
    const Typed::ShapeTable& shapes = encoder.plan.shapes();
    std::span<const Typed::MethodSelector> selectors = conventions.methodSelectors();

    Vector<MethodTableDependentData> dependents;
    for (uint32_t selector = 0; selector < selectors.size(); ++selector) {
        const Typed::MethodFamily& family = conventions.familyOf(selectors[selector]);
        if (!family.entersLookup())
            continue;
        Mention mentionOfSelector { .space = NumberSpace::MethodSelector, .numberOfWords = 1, .firstWord = 0, .addend = 0 };
        std::optional<uint32_t> distanceToWordOfSelector = encoder.numbers.valueOf(mentionOfSelector, std::span<const uint32_t>(&selector, 1), true);
        if (!distanceToWordOfSelector)
            continue;
        for (const Typed::MethodTableRow& row : family.rows) {
            if (!row.guardedClass)
                continue;
            std::array<uint32_t, 4> guard = keyOfClassGuardUnderMethodTableRow(family, row);
            std::optional<uint32_t> guardBit = encoder.numbers.numberOf(NumberSpace::ClassGuard, std::span<const uint32_t>(guard));
            if (!guardBit) {
                encoder.fail("RowOfMethodTableStandsOnClassGuardWithoutBit"_s, row.layout);
                continue;
            }
            Typed::ShapeRange shapesOfRow = shapes.shapesOf(row.layout);
            for (uint32_t shape = shapesOfRow.first; shape < shapesOfRow.end(); ++shape) {
                if (!shapes.isAssigned(shape))
                    continue;
                dependents.append(MethodTableDependentData { *guardBit, distanceToMethodTableEntry(*distanceToWordOfSelector, shape), selector });
            }
        }
    }
    AOT::sort(dependents);
    encoder.part.setTable(ProgramTableIndex::methodTableDependents, dependents);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
