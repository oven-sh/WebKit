/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLinkUnions.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLinkFlow.h"
#include "AOTLinkTables.h"
#include "AOTSort.h"
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static uint32_t sumOfUsesInLinkUnionUses(uint32_t first, uint32_t second)
{
    return first > UINT32_MAX - second ? UINT32_MAX : first + second;
}

// `function kindOf(n: Decl | Expr) { return n.kind }`: places as they are written in reached code, never how often they run.
// A family has its number by its size and its members, and a key that is counted has its number by the program alone, so the list is the same at any number of threads.
void countUsesThroughFamilies(const LinkFlow& flow, LinkTables& tables)
{
    tables.usesThroughFamilies.shrink(0);
    if (!withoutMechanismsMissingDependencies(flow.mechanisms).contains(Mechanism::TypedOperations))
        return;

    const TypeTable& types = flow.program.types;
    Vector<LinkedUseThroughFamily> uses;
    for (size_t index : flow.reached) {
        FunctionID function(index);
        for (const SummaryAccess& access : flow.summaries.at(function).accesses) {
            if (!access.key || access.isToContents() || !types.hasStableNumber(access.key))
                continue;
            const LinkType& subject = flow.typeAt(flow.nodeOf(function, access.subject));
            if (subject.objects != LinkType::Objects::Family)
                continue;
            bool isStore = access.kind == SummaryAccess::Kind::Store || access.kind == SummaryAccess::Kind::Define;
            uses.append(LinkedUseThroughFamily { subject.family, access.key, isStore ? 0u : 1u, isStore ? 1u : 0u });
        }
    }
    AOT::sort(uses, [](const LinkedUseThroughFamily& first, const LinkedUseThroughFamily& second) {
        return std::tuple(first.family.index(), first.key.index()) < std::tuple(second.family.index(), second.key.index());
    });
    for (const LinkedUseThroughFamily& use : uses) {
        if (tables.usesThroughFamilies.isEmpty() || tables.usesThroughFamilies.last().family != use.family || tables.usesThroughFamilies.last().key != use.key) {
            tables.usesThroughFamilies.append(use);
            continue;
        }
        LinkedUseThroughFamily& summed = tables.usesThroughFamilies.last();
        summed.reads = sumOfUsesInLinkUnionUses(summed.reads, use.reads);
        summed.stores = sumOfUsesInLinkUnionUses(summed.stores, use.stores);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
