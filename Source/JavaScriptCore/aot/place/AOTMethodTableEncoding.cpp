/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramTableEncoder.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

// The image builder turns the functions into code offsets, in sections of their own. The run time reads only the dependents.
void encodeMethodTable(TableEncoder& encoder)
{
    const MethodTable& table = encoder.placement.methodTable();

    Vector<uint32_t> functions(table.entries.size(), [&](size_t index) {
        return table.entries[index] ? table.entries[index].index() : MethodTableData::noFunction;
    });
    Vector<uint32_t> lookups(table.selectors.size(), [&](size_t index) {
        return table.selectors[index].lookup.index();
    });
    Vector<MethodTableDependentData> dependents(table.dependents.size(), [&](size_t index) {
        const MethodTableDependent& dependent = table.dependents[index];
        return MethodTableDependentData { numberInStaticsOf(encoder.placement.guardBit(dependent.guard)), dependent.entry, dependent.selector };
    });
    std::ranges::sort(dependents);

    encoder.setTable(ProgramTableIndex::methodTableFunctions, functions);
    encoder.setTable(ProgramTableIndex::methodLookupFunctions, lookups);
    encoder.setTable(ProgramTableIndex::methodTableDependents, dependents);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
