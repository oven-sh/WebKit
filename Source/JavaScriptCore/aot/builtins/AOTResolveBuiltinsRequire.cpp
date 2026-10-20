/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResolveBuiltinsInternal.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include <ranges>

namespace JSC { namespace AOT { namespace Typed {

static bool canFindEdgeReady(FunctionPassContext& context, BlockID block)
{
    if (context.runsAtMostOnce(block))
        return false;
    return context.analyses.blockFrequencies().classOf(block) != BlockFrequency::Rare;
}

static Vector<ValueSite> collectStaticRequires(FunctionPassContext& context)
{
    Vector<ValueSite> sites;
    forEachValueSite(context.analyses, context.function, [&](const ValueSite& site) {
        if (context.function.at(site.value).opcode == Opcode::RequireModule && canFindEdgeReady(context, site.block))
            sites.append(site);
    });
    return sites;
}

static bool holdsModuleObject(const Module& module)
{
    return module.kind == ModuleKind::CommonJs && module.requireResult == RequireResult::ModuleExports;
}

static void expandStaticRequire(SiteExpansion& expansion, const ValueSite& site)
{
    const Program& program = expansion.context().program;
    IRBuilder& builder = expansion.builder();

    expansion.begin(site);
    ModuleID module = expansion.original().module();

    Value test = IRBuilder::make(Opcode::IsRequireEdgeReady);
    test.setModule(module);
    test.setReferrer(expansion.original().referrer());
    ValueID isReady = expansion.append(WTF::move(test));
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(isReady, 0, expansion.frequency(), BlockFrequency::Rare);

    Value read = IRBuilder::make(Opcode::GetRequiredValue, { branch.provenOnTaken });
    read.setModule(module);
    ValueID required = expansion.append(WTF::move(read));
    if (holdsModuleObject(program.at(module))) {
        Value exports = IRBuilder::make(Opcode::PropGet, { required });
        exports.setKey(program.keyOf(BuiltinNames::exports));
        required = expansion.append(WTF::move(exports));
    }
    expansion.finishArm(required);

    builder.moveToEndOf(branch.notTaken);
    expansion.finishArm(expansion.appendOriginal());
    expansion.end();
}

unsigned expandStaticRequires(FunctionPassContext& context)
{
    Vector<ValueSite> sites = collectStaticRequires(context);
    if (sites.isEmpty())
        return 0;

    IRBuilder builder(context.analyses.context(), context.function, context.types, &context.analyses);
    SiteExpansion expansion(context, builder);
    for (const ValueSite& site : sites | std::views::reverse)
        expandStaticRequire(expansion, site);
    return static_cast<unsigned>(sites.size());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
