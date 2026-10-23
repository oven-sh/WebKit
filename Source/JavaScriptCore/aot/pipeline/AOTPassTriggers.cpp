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
#include "AOTPlaceAllocationsAtTheirUse.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTRemoveRedundantBarriers.h"
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

namespace JSC { namespace AOT { namespace Typed {

static constexpr PassTriggers rowOf(PassID id)
{
    switch (id) {
    case PassID::CheckInt32Promises:
        return PassTriggers::anyOf({ Opcode::Convert }).orTerminators({ TerminatorKind::ReturnCall });
    case PassID::CheckRegExpLiterals:
        return PassTriggers::anyOf({ Opcode::NewRegExp });
    case PassID::AddGenericVersion:
        return PassTriggers::anyOf({ Opcode::Check, Opcode::Construct, Opcode::CallBuiltin });
    case PassID::ResolveBuiltins:
        return PassTriggers::anyOf({ }).orOpcodes(operationsResolvedToBuiltins).orTraits(BodyTrait::ParametersOfRuntimeFunction);
    case PassID::BindGlobalNames:
        return PassTriggers::anyOf({ }).orOpcodes(bindableAccesses);
    case PassID::CallThroughMethodTable:
        return PassTriggers::bothOf(PassTriggers::anyOf({ Opcode::Call }), PassTriggers::anyOf({ Opcode::PropGet }));
    case PassID::ResolveCalls:
        return PassTriggers::anyOf({ }).orOpcodes(callSiteOpcodes);
    case PassID::ResolveAccesses:
        return PassTriggers::anyOf({ }).orOpcodes(accessesToResolve);
    case PassID::SplitUnions:
        return PassTriggers::anyOf({ }).orOpcodes(accessesByNameOrKey);
    case PassID::PlaceTypeTests:
        return PassTriggers::anyOf({ }).orOpcodes(opcodesPlaceTypeTestsLooksFor);
    case PassID::ExpandIteration:
        return PassTriggers::anyOf({ }).orOpcodes(expandedOpcodes);
    case PassID::ExpandProvenIterations:
        return PassTriggers::anyOf({ }).orOpcodes(iterationOpcodes);
    case PassID::ExpandOwnKeys:
        return PassTriggers::anyOf({ }).orOpcodes(ownKeyExpansionOpcodes);
    case PassID::SelectTypedResults:
        return PassTriggers::anyOf({ }).orOpcodes(typedResultCallOpcodes);
    case PassID::InlineCalls:
        return PassTriggers::anyOf({ Opcode::CallDirect });
    case PassID::SimplifyCasts:
        return PassTriggers::anyOf({ }).orOpcodes(simplifiableCasts);
    case PassID::SinkAllocations:
        return PassTriggers::anyOf({ }).orOpcodes(opcodesInterpretedAs(Interpretation::Allocation));
    case PassID::EliminateRedundantReads:
        return PassTriggers::anyOf({ }).orOpcodes(opcodesReadingAccessedHeaps);
    case PassID::EliminateGuards:
        return PassTriggers::anyOf({ }).orOpcodes(opcodesOfGuardElimination);
    case PassID::SeparateUnlikelyLoopPaths:
        return PassTriggers::anyOf({ Opcode::LoopHint });
    case PassID::ConvertCountersToIntegers:
        return PassTriggers::anyOf({ }).orTraits(BodyTrait::BlockParameters);
    case PassID::RemoveDeadCountedLoops:
        return PassTriggers::anyOf({ Opcode::LoopHint });
    case PassID::FuseArrayChains:
        return PassTriggers::anyOf({ }).orOpcodes(arrayMethodCallOpcodes);
    case PassID::FuseGeneratorLoops:
        return PassTriggers::bothOf(PassTriggers::anyOf({ Opcode::NewGenerator }), PassTriggers::anyOf({ Opcode::StepGenerator }));
    case PassID::StartGeneratorsAtFirstStep:
        return PassTriggers::anyOf({ Opcode::NewGenerator });
    case PassID::CreateStringBuilders:
        return PassTriggers::anyOf({ }).orOpcodes(concatenationOpcodes);
    case PassID::FuseConcatenations:
        return PassTriggers::anyOf({ }).orOpcodes(concatenationOpcodes);
    case PassID::ExpandData:
        return PassTriggers::anyOf({ Opcode::Data });
    case PassID::FoldConstants:
        return PassTriggers::anyOf({ }).orOpcodes(foldableOpcodes);
    case PassID::SimplifyControlFlow:
        return PassTriggers::anyOf({ }).orTraits(BodyTrait::SeveralBlocks);
    case PassID::RemoveTrivialParameters:
        return PassTriggers::anyOf({ }).orTraits(BodyTrait::BlockParameters);
    case PassID::PropagateBranchConditions:
        return PassTriggers::anyOf({ }).orTerminators({ TerminatorKind::Branch });
    case PassID::HoistTestsIntoPredecessors:
        return PassTriggers::bothOf(PassTriggers::anyOf({ }).orOpcodes(operationsHoistedIntoPredecessors), PassTriggers::anyOf({ }).orTraits(BodyTrait::BlockParameters));
    case PassID::TestNarrowerTypesFirst:
        return PassTriggers::anyOf({ Opcode::TypeTest });
    case PassID::ThreadJumps:
        return PassTriggers::anyOf({ Opcode::TypeTest }).orTraits(BodyTrait::BlockParameters);
    case PassID::InferSwitches:
        return PassTriggers::anyOf({ Opcode::StrictEq, Opcode::ShapeTest }).orTerminators({ TerminatorKind::Switch });
    case PassID::MergeDuplicateBlocks:
        return PassTriggers::anyOf({ }).orTraits(BodyTrait::BlockParameters);
    case PassID::RemoveRedundantHandlers:
        return PassTriggers::anyOf({ }).orTraits(BodyTrait::Handlers);
    case PassID::HoistLoopInvariantReads:
        return PassTriggers::bothOf(PassTriggers::anyOf({ }).orOpcodes(hoistableReadOpcodes), PassTriggers::anyOf({ }).orTraits(BodyTrait::SeveralBlocks));
    case PassID::SimplifyBuiltinCalls:
        return PassTriggers::bothOf(
            PassTriggers::anyOf({ Opcode::CallBuiltin }).orOpcodes(simplifiedIntrinsics).orOpcodes(constantListSourceOpcodes),
            PassTriggers::anyOf({ Opcode::CallBuiltin }).orOpcodes(simplifiedIntrinsics).orOpcodes(constantListUserOpcodes));
    case PassID::ReduceIntegerDivisions:
        return PassTriggers::bothOf(PassTriggers::anyOf({ Opcode::Div }), PassTriggers::anyOf({ Opcode::CallBuiltin }).orOpcodes(int32ConvertingOperations));
    case PassID::PlanArrayCapacities:
        return PassTriggers::anyOf({ }).orOpcodes(arrayCapacityOpcodes);
    case PassID::PlanCollectionCapacities:
        return PassTriggers::bothOf(PassTriggers::anyOf({ Opcode::CallBuiltin }), PassTriggers::anyOf({ }).orTraits(BodyTrait::SeveralBlocks));
    case PassID::FoldConstantRegExps:
        return PassTriggers::anyOf({ Opcode::CallBuiltin });
    case PassID::ShareRegExpLiterals:
        return PassTriggers::anyOf({ Opcode::NewRegExp });
    case PassID::InsertObjectCoercibleChecks:
        return PassTriggers::anyOf({ }).orOpcodes(objectCoercibleCheckOpcodes);
    case PassID::EliminateTailRecursion:
        return PassTriggers::anyOf({ }).orTerminators({ TerminatorKind::ReturnCall });
    case PassID::ResolveAwaitedCalls:
        return PassTriggers::anyOf({ Opcode::AwaitBySavingFrame, Opcode::PromiseAllOfElements });
    case PassID::SinkConstants:
        return PassTriggers::anyOf({ }).orTraits(BodyTrait::SeveralBlocks);
    case PassID::ExpandLongArrayLiterals:
        return PassTriggers::anyOf({ Opcode::NewArray });
    case PassID::FinishBirths:
        return PassTriggers::anyOf({ Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::NewObject }).orTraits(BodyTrait::UnfinishedThis);
    case PassID::PlaceAllocationsAtTheirUse:
        return PassTriggers::anyOf({ }).orOpcodes(allocationsPlacedAtTheirUse);
    case PassID::RemoveRedundantBarriers:
        return PassTriggers::anyOf({ }).orOpcodes(operationsThatMayRunBarrier);
    default:
        return PassTriggers::always();
    }
}

static constexpr std::array<PassTriggers, numberOfPasses> createRows()
{
    std::array<PassTriggers, numberOfPasses> result { };
    for (unsigned index = 0; index < numberOfPasses; ++index)
        result[index] = rowOf(static_cast<PassID>(index));
    return result;
}

static constexpr std::array<PassTriggers, numberOfPasses> rows = createRows();

const PassTriggers& triggersOf(PassID id)
{
    return rows[static_cast<unsigned>(id)];
}

namespace {

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

static constexpr PassesByContent createPassesByContent()
{
    PassesByContent result;
    for (unsigned index = 0; index < numberOfPasses; ++index) {
        PassID pass = static_cast<PassID>(index);
        const PassTriggers& row = rows[index];
        if (row.isAlways) {
            result.always.add(pass);
            continue;
        }
        if (!row.needsOneOfOthersToo)
            result.needingOneList.add(pass);
        row.opcodes.forEach([&](Opcode opcode) {
            result.byOpcode[static_cast<unsigned>(opcode)].first.add(pass);
        });
        row.otherOpcodes.forEach([&](Opcode opcode) {
            result.byOpcode[static_cast<unsigned>(opcode)].other.add(pass);
        });
        row.terminators.forEach([&](TerminatorKind kind) {
            result.byTerminator[static_cast<unsigned>(kind)].first.add(pass);
        });
        row.otherTerminators.forEach([&](TerminatorKind kind) {
            result.byTerminator[static_cast<unsigned>(kind)].other.add(pass);
        });
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (row.traits.toRaw() >> bit & 1)
                result.byTrait[bit].first.add(pass);
            if (row.otherTraits.toRaw() >> bit & 1)
                result.byTrait[bit].other.add(pass);
        }
    }
    return result;
}

static constexpr PassesByContent passesByContent = createPassesByContent();

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
