/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCatalog.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTFieldTable.h"
#include "AOTPropertyCachesFront.h"
#include "AOTSlotPlan.h"
#include "AOTWasmModules.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT {

static_assert(RowDisplacementTable::maxColumn <= FieldTableEntry::maxName);

StubOptions StubOptions::from(const RecordedOptions& recorded, const Typed::Program& program, bool hasOneInstance, bool matchesRegExps, bool checksProofs)
{
    return {
        .useWideReferences = recorded.useWideReferences(),
        .imageHasWebAssembly = mayHaveWebAssemblyModules(program),
        .hasOneInstance = hasOneInstance,
        .matchesRegExps = matchesRegExps,
        .checksProofs = checksProofs,
        .isPositionIndependent = recorded.form() == ImageForm::PositionIndependent,
        .maxNameInFieldTable = RowDisplacementTable::maxColumn,
        .targetFeatures = CPUFeatureSet::fromBits(static_cast<uint32_t>(recorded.targetFeatures)),
    };
}

StubPolicy stubPolicyOf(const Typed::SlotPlan& slots)
{
    const Typed::FieldTableProbePlan& probes = slots.fieldTableProbes();
    StubPolicy policy;
    policy.readsTestOutOfLineValuesSecond = probes.readsTestOutOfLineValuesSecond;
    policy.readsTestDoublesSecond = probes.readsTestDoublesSecond;
    return policy;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
