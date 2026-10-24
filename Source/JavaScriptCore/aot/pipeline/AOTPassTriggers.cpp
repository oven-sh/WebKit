/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPassTriggers.h"

#if ENABLE(AOT)

#include "AOTArrayCapacities.h"
#include "AOTArrayChains.h"
#include "AOTBindGlobalNames.h"
#include "AOTEliminateGuards.h"
#include "AOTEliminateRedundantReads.h"
#include "AOTExpandIteration.h"
#include "AOTExpandOwnKeysInternal.h"
#include "AOTFoldConstants.h"
#include "AOTHoistLoopInvariantReads.h"
#include "AOTHoistTestsIntoPredecessors.h"
#include "AOTInsertObjectCoercibleChecks.h"
#include "AOTInt32Conversions.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTRepresentationRules.h"
#include "AOTResolveAccesses.h"
#include "AOTResolveBuiltinsInternal.h"
#include "AOTResolveCalls.h"
#include "AOTSelectTypedResultsInternal.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTSimplifyCastsInternal.h"
#include "AOTSplitUnions.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTVirtualObject.h"
#include <array>
#include <bit>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// In a body that holds nothing of the first list, or nothing of the second where there is one, the pass leaves the function as it is and reports nothing, whatever its other inputs say, on its first run as on a later one.
struct TriggersOfPass {
    PassID pass;
    BodyContents first;
    std::optional<BodyContents> second;
};

// The rows the other way round: for each thing that a body may hold, the passes that have it in the one list or in the other.
struct PassesByContent {
    struct Lists {
        PassesByTrigger first;
        PassesByTrigger other;
    };

    std::array<Lists, numberOfEnumerators<Opcode>> byOpcode { };
    std::array<Lists, numberOfEnumerators<TerminatorKind>> byTerminator { };
    std::array<Lists, 8> byTrait { };
    PassesByTrigger always;
    PassesByTrigger needingOneList;
};

} // anonymous namespace

// A pass without a row may have something to do in any body.
static constexpr TriggersOfPass triggersOfPasses[] {
    { PassID::CheckInt32Promises, { .opcodes = { Opcode::Convert }, .terminators = { TerminatorKind::ReturnCall } } },
    { PassID::CheckRegExpLiterals, { .opcodes = { Opcode::NewRegExp } } },
    { PassID::AddGenericVersion, { .opcodes = { Opcode::Check, Opcode::Construct, Opcode::CallBuiltin } } },
    { PassID::ResolveBuiltins, { .opcodes = operationsResolvedToBuiltins, .traits = BodyTrait::ParametersOfRuntimeFunction } },
    { PassID::BindGlobalNames, { .opcodes = bindableAccesses } },
    { PassID::CallThroughMethodTable, { .opcodes = { Opcode::Call } }, { { .opcodes = { Opcode::PropGet } } } },
    { PassID::ResolveCalls, { .opcodes = callSiteOpcodes } },
    { PassID::ResolveAccesses, { .opcodes = accessesToResolve } },
    { PassID::SplitUnions, { .opcodes = accessesByNameOrKey } },
    { PassID::PlaceTypeTests, { .opcodes = opcodesPlaceTypeTestsLooksFor } },
    { PassID::ExpandIteration, { .opcodes = expandedOpcodes } },
    { PassID::ExpandProvenIterations, { .opcodes = iterationOpcodes } },
    { PassID::ExpandOwnKeys, { .opcodes = ownKeyExpansionOpcodes } },
    { PassID::SelectTypedResults, { .opcodes = typedResultCallOpcodes } },
    { PassID::InlineCalls, { .opcodes = { Opcode::CallDirect } } },
    { PassID::SimplifyCasts, { .opcodes = simplifiableCasts } },
    { PassID::SinkAllocations, { .opcodes = opcodesInterpretedAs(Interpretation::Allocation) } },
    { PassID::EliminateRedundantReads, { .opcodes = opcodesReadingAccessedHeaps } },
    { PassID::EliminateGuards, { .opcodes = opcodesOfGuardElimination } },
    { PassID::SeparateUnlikelyLoopPaths, { .opcodes = { Opcode::LoopHint } } },
    { PassID::ConvertCountersToIntegers, { .traits = BodyTrait::BlockParameters } },
    { PassID::RemoveDeadCountedLoops, { .opcodes = { Opcode::LoopHint } } },
    { PassID::FuseArrayChains, { .opcodes = arrayMethodCallOpcodes } },
    { PassID::FuseGeneratorLoops, { .opcodes = { Opcode::NewGenerator } }, { { .opcodes = { Opcode::StepGenerator } } } },
    { PassID::StartGeneratorsAtFirstStep, { .opcodes = { Opcode::NewGenerator } } },
    { PassID::CreateStringBuilders, { .opcodes = concatenationOpcodes } },
    { PassID::FuseConcatenations, { .opcodes = concatenationOpcodes } },
    { PassID::ExpandData, { .opcodes = { Opcode::Data } } },
    { PassID::FoldConstants, { .opcodes = foldableOpcodes } },
    { PassID::SimplifyControlFlow, { .traits = BodyTrait::SeveralBlocks } },
    { PassID::RemoveTrivialParameters, { .traits = BodyTrait::BlockParameters } },
    { PassID::PropagateBranchConditions, { .opcodes = { Opcode::ToBoolean }, .terminators = { TerminatorKind::Branch } } },
    { PassID::HoistTestsIntoPredecessors, { .opcodes = operationsHoistedIntoPredecessors }, { { .traits = BodyTrait::BlockParameters } } },
    { PassID::TestNarrowerTypesFirst, { .opcodes = { Opcode::TypeTest } } },
    { PassID::ThreadJumps, { .opcodes = { Opcode::TypeTest }, .traits = BodyTrait::BlockParameters } },
    { PassID::RemoveRedundantHandlers, { .traits = BodyTrait::Handlers } },
    { PassID::HoistLoopInvariantReads, { .opcodes = hoistableOpcodes }, { { .traits = BodyTrait::SeveralBlocks } } },
    { PassID::SimplifyBuiltinCalls, { .opcodes = OpcodeSet { Opcode::CallBuiltin } | simplifiedIntrinsics | constantListSourceOpcodes }, { { .opcodes = OpcodeSet { Opcode::CallBuiltin } | simplifiedIntrinsics | constantListUserOpcodes } } },
    { PassID::ReduceIntegerDivisions, { .opcodes = { Opcode::Div } }, { { .opcodes = OpcodeSet { Opcode::CallBuiltin } | int32ConvertingOperations } } },
    { PassID::PlanArrayCapacities, { .opcodes = arrayCapacityOpcodes } },
    { PassID::PlanCollectionCapacities, { .opcodes = { Opcode::CallBuiltin } }, { { .traits = BodyTrait::SeveralBlocks } } },
    { PassID::FoldConstantRegExps, { .opcodes = { Opcode::CallBuiltin } } },
    { PassID::ShareRegExpLiterals, { .opcodes = { Opcode::NewRegExp } } },
    { PassID::InsertObjectCoercibleChecks, { .opcodes = objectCoercibleCheckOpcodes } },
    { PassID::EliminateTailRecursion, { .terminators = { TerminatorKind::ReturnCall } } },
    { PassID::ResolveAwaitedCalls, { .opcodes = { Opcode::AwaitBySavingFrame, Opcode::PromiseAllOfElements } } },
    { PassID::SinkConstants, { .traits = BodyTrait::SeveralBlocks } },
    { PassID::ExpandLongArrayLiterals, { .opcodes = { Opcode::NewArray } } },
    { PassID::FinishBirths, { .opcodes = { Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::NewObject }, .traits = BodyTrait::UnfinishedThis } },
    { PassID::RemoveRedundantBarriers, { .opcodes = operationsThatMayRunBarrier } },
};

static constexpr PassesByContent passesByContent = [] {
    PassesByContent result;
    auto add = [&](PassID pass, const BodyContents& contents, PassesByTrigger PassesByContent::Lists::* list) {
        contents.opcodes.forEach([&](Opcode opcode) {
            (result.byOpcode[static_cast<unsigned>(opcode)].*list).add(pass);
        });
        contents.terminators.forEach([&](TerminatorKind kind) {
            (result.byTerminator[static_cast<unsigned>(kind)].*list).add(pass);
        });
        for (unsigned traits = contents.traits.toRaw(); traits; traits &= traits - 1)
            (result.byTrait[std::countr_zero(traits)].*list).add(pass);
    };
    PassesByTrigger passesWithRow;
    for (const TriggersOfPass& row : triggersOfPasses) {
        passesWithRow.add(row.pass);
        add(row.pass, row.first, &PassesByContent::Lists::first);
        if (row.second)
            add(row.pass, *row.second, &PassesByContent::Lists::other);
        else
            result.needingOneList.add(row.pass);
    }
    for (PassID pass : allMembersOf<PassID>) {
        if (!passesWithRow.contains(pass))
            result.always.add(pass);
    }
    return result;
}();

bool hasTriggers(PassID id)
{
    return !passesByContent.always.contains(id);
}

PassesByTrigger passesThatMayFireIn(const BodyContents& contents)
{
    PassesByTrigger first;
    PassesByTrigger other = passesByContent.needingOneList;
    auto add = [&](const PassesByContent::Lists& lists) {
        first |= lists.first;
        other |= lists.other;
    };
    contents.opcodes.forEach([&](Opcode opcode) {
        add(passesByContent.byOpcode[static_cast<unsigned>(opcode)]);
    });
    contents.terminators.forEach([&](TerminatorKind kind) {
        add(passesByContent.byTerminator[static_cast<unsigned>(kind)]);
    });
    for (unsigned traits = contents.traits.toRaw(); traits; traits &= traits - 1)
        add(passesByContent.byTrait[std::countr_zero(traits)]);
    first &= other;
    first |= passesByContent.always;
    return first;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
