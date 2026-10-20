/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowResultFamilies.h"

#if ENABLE(AOT)

#include "AOTThreads.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// Awaiting an object, or resolving a promise with it as import() does, reads the name `then`. An export of that name that may be called is recorded as read where it is called.
static constexpr OptionSet<FlowObjectUse> namedReadUses { FlowObjectUse::ReadByName, FlowObjectUse::Awaited };

static bool hasTrackedNamespace(ModuleKind kind)
{
    switch (kind) {
    case ModuleKind::SourceText:
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
        return true;
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static NamespaceUse namespaceUseOf(const FlowFamilyInputs& inputs, ModuleID module)
{
    if (!hasTrackedNamespace(inputs.program().at(module).kind))
        return NamespaceUse::Unknown;
    if (!inputs.records().createdNamespaces().get(module.index()))
        return NamespaceUse::NeverCreated;

    FlowObjectID namespaceObject = inputs.objects().findNamespace(module);
    if (!namespaceObject)
        return NamespaceUse::Unknown;

    const FlowObjectFacts& facts = inputs.factsOf(namespaceObject);
    if (facts.mutations.containsAny(reachMutations) || !facts.uses.containsOnly(namedReadUses))
        return NamespaceUse::Unknown;
    return NamespaceUse::NamedReadsOnly;
}

FlowEvaluationResults computeEvaluationResults(const FlowFamilyInputs& inputs)
{
    FlowEvaluationResults results;
    results.modules.grow(inputs.program().modules.size());
    forEachIndexInParallel(inputs.loops(), results.modules.size(), [&](unsigned index) {
        if (inputs.isEnabled(FlowResultKind::ObjectUses, index))
            results.modules[index].namespaceUse = namespaceUseOf(inputs, ModuleID(index));
    });
    return results;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
