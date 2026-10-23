/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowNamespaces.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

FlowOwnListing exportListingOf(const Program& program, ModuleID id)
{
    const Module& module = program.at(id);
    switch (module.kind) {
    case ModuleKind::SourceText:
        break;
    default:
        return FlowOwnListing::MayHaveOthers;
    }

    // The loader takes the names from what a CommonJS file exports once that file has run.
    if (module.exportRule != SyntheticExportRule::None)
        return FlowOwnListing::MayHaveOthers;
    bool hasDynamicStarExport = module.requests.containsIf([](const ModuleRequest& request) {
        return request.isDynamicStarExport;
    });
    return hasDynamicStarExport ? FlowOwnListing::MayHaveOthers : FlowOwnListing::Complete;
}

VariableID findStaticFieldVariable(const Program& program, FunctionID classConstructor, PropertyKeyID key)
{
    LayoutID layout = program.at(classConstructor).constructedClass;
    if (!layout || !key)
        return { };

    for (const StaticField& field : program.at(layout).staticFields) {
        if (field.key == key)
            return field.variable;
    }
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
