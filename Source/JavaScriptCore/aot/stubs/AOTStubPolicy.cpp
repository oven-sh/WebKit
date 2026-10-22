/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTStubCatalog.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCompileOptions.h"
#include "AOTFlowResults.h"
#include "AOTIRProgram.h"
#include "AOTIdentifierNumbering.h"
#include "AOTPropertyCachesFront.h"
#include "AOTRegExpRequests.h"
#include "AOTRegExpSummary.h"
#include "AOTStringUses.h"
#include "AOTWasmModules.h"
#include "YarrFlags.h"
#include <array>
#include <span>

namespace JSC { namespace AOT {

StubOptions StubOptions::from(const RecordedOptions& recorded, const Typed::Program& program, const Typed::FlowResults& flow)
{
    return {
        .useWideReferences = recorded.useWideReferences(),
        .forceGCSlowPaths = recorded.has(TestingSwitch::ForceGCSlowPaths),
        .imageHasWebAssembly = mayHaveWebAssemblyModules(program),
        .hasOneInstance = flow.programInstances() == Typed::ProgramInstances::AtMostOne,
        .validates = recorded.validates,
        .countsAllocations = recorded.countsBranches,
        .layoutSeed = recorded.layoutSeed,
        .targetFeatures = CPUFeatureSet::fromBits(static_cast<uint32_t>(recorded.targetFeatures)),
    };
}

RegExpLiteralConstants StubPolicy::regExpLiteral(std::optional<uint32_t> record, bool isGlobal) const
{
    RegExpLiteralConstants constants;
    if (record && *record < regExpLiterals.size())
        constants = regExpLiterals[*record];
    constants.isGlobal = isGlobal;
    constants.usesLastIndex = constants.usesLastIndex || isGlobal;
    return constants;
}

StubPolicy chooseStubPolicy(const KeyUses& uses, const IdentifierNumbering& numbering, BuildContext&)
{
    static constexpr std::array accessesWithBoundStubs { Typed::Access::Read, Typed::Access::Store, Typed::Access::Define };

    StubPolicy policy;
    for (size_t key = 0; key < uses.byKey.size(); ++key) {
        const KeyUse& use = uses.byKey[key];
        for (Typed::Access access : accessesWithBoundStubs) {
            size_t accessIndex = static_cast<size_t>(access);
            if (use.usesByAccess[accessIndex] < StubPolicy::minUsesForOwnStub)
                continue;
            if (std::optional<uint32_t> number = numbering.numberOf(PropertyKeyID(key)); number && RowDisplacementTable::hasColumn(*number))
                policy.namesWithOwnStubByAccess[accessIndex].set(*number);
        }
    }
    return policy;
}

void addRegExpLiterals(StubPolicy& policy, const RegExpRequests& requests, bool legacyStaticsMayBeRead, BuildContext& context)
{
    std::span<const RegExpRequest> allRequests = requests.all();
    policy.regExpLiterals.grow(allRequests.size());
    for (size_t record = 0; record < allRequests.size(); ++record) {
        const RegExpRequest& request = allRequests[record];
        RegExpLiteralConstants& constants = policy.regExpLiterals[record];
        const RegExpSummary* summary = context.regExpSummaries().summaryIfComputed(request.pattern, request.flags);
        if (summary && summary->metadata == RegExpMetadata::Known) {
            constants.minimumSize = summary->minimumSize;
            constants.numberOfGroups = summary->numSubpatterns;
            constants.hasNamedGroups = summary->hasNamedGroups();
        }
        // The flags of a request are the ones that change the code, and g is none of them.
        constants.usesLastIndex = request.flags.contains(Yarr::Flags::Sticky);
        constants.isGlobal = false;
        constants.matchesAtCodePointBoundaries = request.flags.containsAny({ Yarr::Flags::Unicode, Yarr::Flags::UnicodeSets });
        constants.recordsMatch = legacyStaticsMayBeRead;
    }
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
