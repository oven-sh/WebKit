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
    case ModuleKind::CommonJs:
    case ModuleKind::Data:
    case ModuleKind::RuntimeSource:
    case ModuleKind::Runtime:
    case ModuleKind::External:
        return FlowOwnListing::MayHaveOthers;
    }

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

    std::optional<uint32_t> field = program.at(layout).findStaticField(key);
    return field ? program.at(layout).staticFields[*field].variable : VariableID();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
