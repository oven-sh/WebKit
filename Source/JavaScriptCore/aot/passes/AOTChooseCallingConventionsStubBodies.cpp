/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTChooseCallingConventionsInternal.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCallGraph.h"
#include "AOTConvention.h"
#include "AOTDerivedFunctions.h"
#include "AOTStubBodyConvention.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static_assert(StubBodyConvention::maxNumberOfOperandGPRs <= numberOfDirectArgumentGPRs);
static_assert(StubBodyConvention::maxNumberOfOperandFPRs <= numberOfDirectArgumentFPRs);
static_assert([] {
    for (unsigned index = 0; index < StubBodyConvention::maxNumberOfOperandGPRs; ++index) {
        if (StubBodyConvention::operandGPR(index) != directArgumentGPR(index))
            return false;
    }
    for (unsigned index = 0; index < StubBodyConvention::maxNumberOfOperandFPRs; ++index) {
        if (StubBodyConvention::operandFPR(index) != directArgumentFPR(index))
            return false;
    }
    return true;
}());
static_assert(StubBodyConvention::resultGPR(0) == GPRInfo::returnValueGPR);
static_assert(StubBodyConvention::resultFPR() == FPRInfo::returnValueFPR);

static bool hasOnlyReturningDirectCalls(const ConventionInputs& context, std::span<const TailCallSite> sites, const DirectCallUse& use, FunctionID id)
{
    if (context.inputs.callGraph.entrySet(id) != EntrySet::DirectCallers)
        return false;
    if (context.inputs.derivedFunctions->find(derivedFunctionKey(FunctionRole::GenericEntrypoint, id)))
        return false;
    if (!use.isCalledAndReturnedTo.get(id.index()) || use.isTailCalled.get(id.index()))
        return false;
    return !std::ranges::binary_search(sites, id, { }, &TailCallSite::caller);
}

static bool fitsStubBodyConvention(const Program& program, const Function& function, ConventionRegisters registers)
{
    if (function.results.size() != 1)
        return false;
    ArgumentLocations locations = computeArgumentLocations(program, function, FunctionConvention(), registers);
    if (locations.numberOfStackWords())
        return false;

    unsigned numberOfGPRs = 0;
    unsigned numberOfFPRs = 0;
    for (const ArgumentLocation& location : locations.parameters) {
        switch (location.kind) {
        case ArgumentLocation::Kind::GPR:
            ++numberOfGPRs;
            break;
        case ArgumentLocation::Kind::FPR:
            ++numberOfFPRs;
            break;
        case ArgumentLocation::Kind::None:
        case ArgumentLocation::Kind::Stack:
            break;
        }
    }
    return numberOfGPRs <= StubBodyConvention::maxNumberOfOperandGPRs && numberOfFPRs <= StubBodyConvention::maxNumberOfOperandFPRs;
}

static bool canBecomeStubBody(const ConventionInputs& context, std::span<const TailCallSite> sites, const DirectCallUse& use, ConventionRegisters registers, const RuntimeFunctionData& data, FunctionID id)
{
    if (!id || !data.flags.contains(RuntimeFunctionFlag::UsesStubConvention) || data.implements.empty())
        return false;
    const Function& function = context.program().at(id);
    if (!function.hasBody() || function.role != FunctionRole::Library || !context.isEnabledFor(id))
        return false;
    return hasOnlyReturningDirectCalls(context, sites, use, id) && fitsStubBodyConvention(context.program(), function, registers);
}

void chooseStubBodies(const ConventionInputs& context, std::span<const TailCallSite> sites, const DirectCallUse& use, ConventionRegisters registers)
{
    const Program& program = context.program();
    for (const RuntimeFunctionEntry& entry : program.runtimeFunctions) {
        const RuntimeFunctionData& data = program.builtins().at(entry.id);
        if (!canBecomeStubBody(context, sites, use, registers, data, entry.function))
            continue;
        Function& edited = context.editor.at(entry.function);
        edited.role = FunctionRole::StubBody;
        edited.creator = CreatorRecord::StubBody { .builtin = data.implements[0] };
        edited.capabilities = capabilitiesOf(FunctionRole::StubBody);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
