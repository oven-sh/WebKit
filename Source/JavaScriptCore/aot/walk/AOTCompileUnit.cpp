/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCompileUnit.h"

#if ENABLE(AOT)

#include "AOTCurrentWork.h"
#include "AOTDiagnostics.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsNet.h"
#include "AOTWalkKnown.h"
#include "AOTMachineArena.h"
#include "AOTNumbering.h"
#include "AOTPhaseTimes.h"
#include "AOTRepresentation.h"
#include "AOTRepresentationRules.h"
#include "AOTResultRules.h"
#include "AOTScratchRealm.h"
#include "AOTStatistics.h"
#include "AOTTypeArena.h"
#include "AOTTypeIDs.h"
#include "AOTTypeText.h"
#include "AOTBodyIndex.h"
#include "AOTEffectsOfOperations.h"
#include "AOTFacts.h"
#include "AOTOpcodeSets.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/ScopedLambda.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

WalkOptions walkOptionsOf(const BuildContext& build)
{
#if ASSERT_ENABLED || ASAN_ENABLED
    constexpr bool isCheckingBuild = true;
#else
    constexpr bool isCheckingBuild = false;
#endif
    const AOT::CompileOptions& options = build.options();
    return {
        .takesNotes = AOT::hasFlag(options.reports, Wire::ReportSet::Flow) || AOT::hasFlag(options.reports, Wire::ReportSet::AccessCounts) || AOT::hasFlag(options.reports, Wire::ReportSet::Allocations),
        .verifiesCode = isCheckingBuild || options.verifiesCode,
        .hidesNames = options.obfuscates(),
        .checksProofs = !!sourcesOfProofsCheckedUnder(options.testing),
        .sourcesOfProofsChecked = sourcesOfProofsCheckedUnder(options.testing),
        .checksBounds = checksBoundsUnder(options.testing),
        .shouldAssumeIndexesInBounds = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeIndexesInBounds),
        .shouldVerifyAssumedBounds = build.validates() && !sourcesOfProofsCheckedUnder(options.testing) && !checksBoundsUnder(options.testing),
        .shouldDropLengthCheckedReads = AOT::hasFlag(options.testing, Wire::TestingSwitch::DropLengthCheckedReads),
        .shouldAssumeInvariantLoopBounds = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeInvariantLoopBounds),
        .shouldAssumeInvariantArrayStorage = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeInvariantArrayStorage),
        .shouldMisidentifyHoistedReadOperands = AOT::hasFlag(options.testing, Wire::TestingSwitch::MisidentifyHoistedReadOperands),
        .shouldAssumeUInt32Dividends = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeUInt32Dividends),
        .shouldMisroundLoopBounds = AOT::hasFlag(options.testing, Wire::TestingSwitch::MisroundLoopBounds),
        .shouldAssumeGuardsAndFacts = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeGuardsAndFacts),
        .checksHoistedCalls = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckHoistedCallsAtRunTime),
        .shouldAssumeInlineBuiltinsPure = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeInlineBuiltinsPure),
        .shouldAssumeNoNaNStores = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeNoNaNStores),
        .shouldSkipCapacityChecks = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipCapacityChecks),
        .shouldSkipLengthChecks = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipLengthChecks),
        .shouldIgnoreLengthStores = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreLengthStores),
        .shouldIgnorePrototypeReads = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnorePrototypeReads),
        .shouldAssumeEnumeratorCurrent = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeEnumeratorCurrent),
        .shouldIgnoreAlternativeOverloads = AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreAlternativeOverloads),
        .shouldIgnoreFreshArrayEscapes = AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreFreshArrayEscapes),
        .shouldIgnoreLoopArrayStores = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreLoopArrayStores),
        .shouldOverstateBoundArgumentCount = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::OverstateBoundArgumentCount),
        .shouldSkipBoundsProof = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipBoundsProof),
        .shouldMisfoldBranchInSweep = AOT::hasFlag(options.testing, Wire::TestingSwitch::MisfoldBranchInSweep),
        .shouldDropEdgeInSweep = AOT::hasFlag(options.testing, Wire::TestingSwitch::DropEdgeInSweep),
        .shouldAssumeCalleesRetainNothing = AOT::hasFlag(options.testing, Wire::TestingSwitch::AssumeCalleesRetainNothing),
        .forcesStraightCompilation = AOT::hasFlag(options.testing, Wire::TestingSwitch::ForceStraightCompilation),
        .checksEffects = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckEffectsAtRunTime),
        .checksWalk = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckWalkAtRunTime),
        .replaysFinalLoopVisits = options.replaysFinalLoopVisits,
        .repeatsFittingLoopVisits = options.repeatsFittingLoopVisits,
        .shouldDisableLaterUnitSpeculation = AOT::hasFlag(options.testing, Wire::TestingSwitch::DisableLaterUnitSpeculation),
        .understatedEffects = options.understatedEffects,
        .ignoresBranchHints = AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreBranchHints),
        .shufflesBlockFrequencies = AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleBlockFrequencies),
        .ignoresExpectedTypes = AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreExpectedTypes),
        .shufflesExpectedTypes = AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleExpectedTypes),
        .shouldFailSpeculations = AOT::hasFlag(options.testing, Wire::TestingSwitch::FailSpeculations),
        .shouldIgnoreIterationEffects = UnderstatedEffects::decode(options.understatedEffects).kind == KindOfEffectsRow::RestartMark,
        .shouldUnderstateLoopEffects = UnderstatedEffects::decode(options.understatedEffects).kind == KindOfEffectsRow::RowOfLoop,
        .netOfEffectsWatchesTurns = AOT::hasFlag(options.testing, Wire::TestingSwitch::CheckEffectsAtRunTime) || UnderstatedEffects::decode(options.understatedEffects).kind == KindOfEffectsRow::RestartMark,
        .shufflesChoices = AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleChoices),
        .judgesInt32Promises = !AOT::hasFlag(options.testing, Wire::TestingSwitch::IgnoreExpectedTypes) && !AOT::hasFlag(options.testing, Wire::TestingSwitch::ShuffleExpectedTypes) && !AOT::hasFlag(options.testing, Wire::TestingSwitch::WidenFlowNodes),
        .shuffleSeed = options.shuffleSeed,
        .forcedWalkMode = static_cast<WalkMode>(options.forcedWalkMode),
        .forcedWalkModePeriod = options.forcedWalkModePeriod,
        .loopVisitAbortPeriod = options.loopVisitAbortPeriod,
        .loopVisitAbortPhase = options.loopVisitAbortPhase,
        .shouldAssumeLaterValuesUnaliased = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeLaterValuesUnaliased),
        .shouldAssumeLocalArrayReceivers = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeLocalArrayReceivers),
        .shouldAssumeNumericAddends = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeNumericAddends),
        .shouldAssumeArithmeticUsers = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeArithmeticElementUsers),
        .shouldAssumePermanentlyPristineIterables = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumePermanentlyPristineIterables),
        .shouldAssumePristineDestructuring = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumePristineDestructuring),
        .shouldAssumeIterationsRestartable = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeIterationsRestartable),
        .shouldAllowMarkedObjects = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AllowMarkedObjects),
        .shouldSkipPairGuard = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipPairGuard),
        .shouldAssumePristineReceivers = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumePristineReceivers),
        .shouldSkipStorageChecks = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipStorageChecks),
        .shouldAssumeVariablesInitialized = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeVariablesInitialized),
        .shouldAssumeSlotsInitialized = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeSlotsInitialized),
        .shouldIgnoreVariableChecks = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreVariableChecks),
        .shouldDropLastCase = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::DropLastCase),
        .shouldSkipSuperMethodGuard = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipSuperMethodGuard),
        .shouldSkipHierarchyGuards = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipHierarchyGuards),
        .shouldSkipClassGuards = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipClassGuards),
        .shouldIgnoreSiblingPrototypes = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreSiblingPrototypes),
        .shouldSkipPrototypeChecks = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipPrototypeChecks),
        .shouldAssumeLayoutPrototype = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeLayoutPrototype),
        .shouldSkipSuperclassCheck = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipSuperclassCheck),
        .shouldSkipSuperPrototypeGuard = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::SkipSuperPrototypeGuard),
        .shouldIgnoreLoopEscapes = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreLoopEscapes),
        .shouldIgnoreLoopWrites = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreLoopWrites),
        .shouldIgnoreLoopUses = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::IgnoreLoopUses),
        .shouldAssumeInvariantOperands = AOT::hasFlag(options.injectedFaults, Wire::FaultInjection::AssumeInvariantOperands),
        .stringEncoders = &build.stringEncoders(),
    };
}

static std::span<const uint32_t> arenaIDsInUnitMentionKey(NumberSpace space, std::span<const uint32_t> key)
{
    switch (space) {
    case NumberSpace::Name:
    case NumberSpace::KeyCell:
    case NumberSpace::PropertyRun:
    case NumberSpace::Type:
        return key;
    case NumberSpace::GlobalBinding:
        return key.first(1);
    case NumberSpace::SiteCache:
        return key.size() < 2 ? std::span<const uint32_t>() : key.subspan(key.size() - 2, 1);
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::RequireEdge:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::CardOfWord:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::ReadGroupCache:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassGuard:
    case NumberSpace::ClassRank:
    case NumberSpace::HeirLookedAt:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::NameHeader:
    case NumberSpace::StringCharacters:
    case NumberSpace::MethodSelector:
    case NumberSpace::RegExp:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool unitMentionsThreadArenaID(const CodeBody& body)
{
    for (const Mention& mention : body.mentions) {
        std::span<const uint32_t> key = AOT::keyOf(mention, body.mentionWords.span());
        for (uint32_t word : arenaIDsInUnitMentionKey(mention.space, key)) {
            if (liesInThread(TypeID::fromBits(word)))
                return true;
        }
    }
    return false;
}

static bool isReachedPastBlockInCompileUnit(const Code& code, BlockID reached, BlockID avoided)
{
    if (reached == avoided)
        return false;
    BitVector seen(code.numberOfBlocks());
    Vector<BlockID, 32> toVisit;
    auto visit = [&](BlockID block) {
        if (block != avoided && !seen.set(block.index()))
            toVisit.append(block);
    };
    for (const EntryOfCode& entry : code.entries())
        visit(entry.block);
    while (!toVisit.isEmpty()) {
        BlockID block = toVisit.takeLast();
        if (block == reached)
            return true;
        const Terminator& end = code.at(block).terminator;
        for (uint32_t index = 0; index < end.numberOfEdges; ++index)
            visit(code.at(end.edge(index)).to);
        if (EdgeID handler = code.handlerOf(block))
            visit(code.at(handler).to);
    }
    return false;
}

static bool cellIsUsedBehindOperationInCompileUnit(const Code& code, ValueID cell, ValueID behind)
{
    static constexpr OpcodeSet operationsThatOnlyLetCellGo { Opcode::EndCellLifetime, Opcode::ReleaseArray, Opcode::ReleaseList, Opcode::ReleaseEnvironment };
    BlockID whereCellIsBorn = code.blockOf(cell);
    bool isUsed = false;
    auto look = [&](ValueID operand) {
        isUsed |= operand && !code.isConstant(operand) && underlyingCellOf(code, operand) == cell;
    };
    BitVector seen(code.numberOfBlocks());
    Vector<BlockID, 32> toVisit;
    auto visit = [&](BlockID block) {
        if (block != whereCellIsBorn && !seen.set(block.index()))
            toVisit.append(block);
    };
    auto lookAtEndOf = [&](BlockID id) {
        const Block& block = code.at(id);
        forEachValueReadAtEndOfSweptBlock(block, look);
        for (uint32_t index = 0; index < block.terminator.numberOfEdges; ++index) {
            const Edge& edge = code.at(block.terminator.edge(index));
            for (ValueID argument : edge.arguments())
                look(argument);
            visit(edge.to);
        }
        if (EdgeID handler = code.handlerOf(id)) {
            for (ValueID argument : code.at(handler).arguments())
                look(argument);
            visit(code.at(handler).to);
        }
    };
    bool standsBehind = false;
    for (ValueID each : code.valuesOf(code.blockOf(behind))) {
        if (standsBehind && !operationsThatOnlyLetCellGo.contains(code.at(each).opcode))
            forEachOperandNeededBySweep(code.at(each), look);
        standsBehind |= each == behind;
    }
    lookAtEndOf(code.blockOf(behind));
    while (!isUsed && !toVisit.isEmpty()) {
        BlockID block = toVisit.takeLast();
        for (ValueID each : code.valuesOf(block)) {
            if (!operationsThatOnlyLetCellGo.contains(code.at(each).opcode))
                forEachOperandNeededBySweep(code.at(each), look);
        }
        lookAtEndOf(block);
    }
    return isUsed;
}

static void reportFailureOfCompileUnit(BuildContext& build, CodeUnit unit, InternalErrorKind kind, const String& detail)
{
    build.diagnostics().add(InternalError { .kind = kind, .function = unit.function, .detail = detail });
}

static std::optional<uint64_t> instructionsOfCompileThreadSoFar(const BuildContext& build)
{
    if (!build.options().logging.logsPhaseTimes) [[likely]]
        return std::nullopt;
    return InstructionCounts::ofCurrentThread();
}

static bool isAllocationOfUnitForEffectsAgainstLink(const Code& code, ValueID value, Vector<ValueID, 8>& parametersAsked)
{
    if (!value)
        return false;
    value = underlyingCellOf(code, value);
    const Operation& made = code.at(value);
    if (made.opcode != Opcode::Parameter)
        return resultRuleOf(made.opcode) == ResultRule::Allocation || (!made.numberOfOperands() && declaredBoundInTable(made.opcode).writes.contains(AbstractHeap::AllocatorState));
    const Block& block = code.at(made.block());
    if (block.isEntry || block.isHandlerBlock() || !block.firstEdgeIn())
        return false;
    if (parametersAsked.contains(value))
        return true;
    parametersAsked.append(value);
    size_t place = block.placeOfParameter(value);
    for (EdgeID edge = block.firstEdgeIn(); edge; edge = code.at(edge).nextInto()) {
        std::span<const ValueID> arguments = code.at(edge).arguments();
        if (place >= arguments.size() || !isAllocationOfUnitForEffectsAgainstLink(code, arguments[place], parametersAsked))
            return false;
    }
    return true;
}

static String findPartThatLinkHasNotCounted(const Code& code, const Operation& operation, const Effects& counted)
{
    EffectTraits traits = operation.traits();
    bool isOperator = effectRuleOf(operation.opcode) == EffectRule::Operator || operation.opcode == Opcode::Strcat;
    if (traits.mayThrowByRule() && !counted.mayThrowByRule() && !isOperator && !code.at(operation.block()).handler)
        return "may throw"_s;
    if (traits.mayCollect() && !counted.mayCollect)
        return "may collect"_s;
    if (!traits.mayWriteObservableState())
        return { };
    Vector<ValueID, 8> parametersAsked;
    const Effects& declared = declaredBoundInTable(operation.opcode);
    bool takesNothing = !operation.numberOfOperands();
    bool fillsWhatUnitMakes = operation.origin().isPartOfConstruction || (takesNothing && !declared.mayCallUnknownCode) || familyOf(operation.opcode) == OperationFamily::Hint || (!takesNothing && isAllocationOfUnitForEffectsAgainstLink(code, operation.operand(0), parametersAsked));
    if (declared.mayCallUnknownCode) {
        EffectTraits traitsCounted(counted);
        if (traits.mayWriteOutsideObjects() && !traitsCounted.mayWriteOutsideObjects())
            return "may write outside of objects"_s;
        return traitsCounted.mayWriteObservableState() || fillsWhatUnitMakes ? String() : String("may write what is seen"_s);
    }
    for (AbstractHeap heap : allMembersOf<AbstractHeap>) {
        if (!observableHeaps.contains(heap) || !declared.writes.contains(heap) || counted.writes.contains(heap))
            continue;
        if (!fillsWhatUnitMakes || !perObjectHeaps.contains(heap))
            return makeString("writes "_s, nameOf(heap));
    }
    return { };
}

struct ClosuresThatCalledParametersHold {
    bool run(const Operation& operation) const
    {
        if (frames.get(operation.origin().inlineFrame) || (operation.opcode == Opcode::CallDirect && functions.contains(operation.function())))
            return true;
        return !operation.origin().inlineFrame && builtinCallPositions.contains(operation.origin().position);
    }

    Vector<FunctionID, 4> functions;
    Vector<uint32_t, 4> builtinCallPositions;
    BitVector frames;
};

static ClosuresThatCalledParametersHold findClosuresThatCalledParametersHold(const WalkInputs& inputs, TypeArena& types, const Code& code, uint32_t parametersCalled)
{
    ClosuresThatCalledParametersHold held;
    auto note = [&](TypeID type) {
        if (!type || !types.at(type).hasOnlyClosures())
            return;
        forEachFunctionOf(types, types.at(type), scopedLambda<void(FunctionID)>([&](FunctionID function) {
            if (!held.functions.contains(function))
                held.functions.append(function);
        }));
    };
    std::span<const LinkedParameter> parameters = inputs.facts.linked().parametersOf(code.unit().function);
    for (uint32_t parameter = 0; parameter < parameters.size() && parameter < 32; ++parameter) {
        if (parametersCalled >> parameter & 1)
            note(parameters[parameter].proven);
    }
    auto isCalledParameter = [&](ValueID value) {
        const Operation& operation = code.at(value);
        if (operation.opcode != Opcode::Parameter || !operation.block() || !code.at(operation.block()).isEntry)
            return false;
        size_t place = code.at(operation.block()).placeOfParameter(value);
        return place < 32 && (parametersCalled >> place & 1);
    };
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        const Operation& operation = code.at(ValueID(index));
        if (isCalledParameter(ValueID(index)))
            note(operation.type);
        else if (operation.opcode == Opcode::TypeTest && operation.block() && isCalledParameter(underlyingCellOf(code, operation.operand(0))))
            note(operation.testedType());
    }

    const BodyIndex* body = inputs.facts.bodies().find(code.unit().function);
    for (uint32_t block = 0; body && block < body->numberOfBlocks(); ++block) {
        for (BodyValue call : body->block(BlockID(block)).values()) {
            ValueID callee = call.opcode() == Opcode::Call ? body->stripCasts(call.calleeOperand()) : ValueID();
            if (!callee || !body->isParameter(callee) || body->blockOf(callee) != entryBlock)
                continue;
            uint32_t place = callee.index() - body->block(entryBlock).firstParameter().index();
            if (place < 32 && (parametersCalled >> place & 1))
                held.builtinCallPositions.append(call.position());
        }
    }

    std::span<const InlineFrame> copied = code.inlineFrames();
    for (uint32_t frame = 1; frame < copied.size(); ++frame) {
        bool isOfCalledParameter = !copied[frame].parent && (held.functions.contains(copied[frame].callee) || held.builtinCallPositions.contains(copied[frame].callPosition));
        if (isOfCalledParameter || held.frames.get(copied[frame].parent))
            held.frames.set(frame);
    }
    return held;
}

static String findOperationThatLinkHasNotCounted(const WalkInputs& inputs, TypeArena& types, const Code& code)
{
    CodeUnit unit = code.unit();
    if ((unit.role != CodeRole::Body && !code.isEntryThatWalksBody()) || unitDoesMoreThanBodyForKnownEffects(inputs.plan, unit.function))
        return { };
    const LinkTables& tables = inputs.facts.linked();
    const LinkedFunction& linked = tables.function(unit.function);
    Effects counted = linked.effectsOnReturn();
    counted.merge(linked.effectsOnThrow());
    const LinkedEffectsGivenArguments* given = code.isEntryThatWalksBody() ? nullptr : tables.known.findEffectsGivenArguments(unit.function);
    Effects countedGivenArguments = given ? given->onReturn : Effects::none();
    if (given)
        countedGivenArguments.merge(given->onThrow);
    if (inputs.options.understatedEffects) [[unlikely]] {
        UnderstatedEffects row = UnderstatedEffects::decode(inputs.options.understatedEffects);
        row.cut(KindOfEffectsRow::Summary, unit.function.index(), given ? countedGivenArguments : counted);
    }
    if (inputs.options.ignoresBranchHints || inputs.options.shufflesBlockFrequencies) [[unlikely]]
        given = nullptr;
    ClosuresThatCalledParametersHold leftToCaller;
    if (given && given->parametersCalled)
        leftToCaller = findClosuresThatCalledParametersHold(inputs, types, code, given->parametersCalled);
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        const Operation& operation = code.at(ValueID(index));
        if (!operation.block() || operation.traits().mayCallUnknownCode() || (code.isEntryThatWalksBody() && operation.opcode == Opcode::CreateRest))
            continue;
        if (operation.opcode == Opcode::CheckIterationRestart || operation.opcode == Opcode::CheckInlineeFallback || operation.opcode == Opcode::CheckReloadBypass)
            continue;
        String part = findPartThatLinkHasNotCounted(code, operation, counted);
        ASCIILiteral summary = "Link's effects of the function"_s;
        if (part.isNull() && given && code.at(operation.block()).frequency == BlockFrequency::Normal && !leftToCaller.run(operation)) {
            part = findPartThatLinkHasNotCounted(code, operation, countedGivenArguments);
            summary = "Link's effects of the function given its arguments"_s;
        }
        if (part.isNull())
            continue;
        return makeString('f', unit.function.index(), " v"_s, index, " ("_s, nameOf(operation.opcode), ") "_s, part, ", and "_s, summary, " say that it does not"_s);
    }
    return { };
}

namespace {

class JudgeOfCalls {
public:
    JudgeOfCalls(const WalkInputs& inputs, TypeArena& types, const Code& code, std::span<const Walker::ArrivalHeld> held, const PositionTypes* whatFunctionsHandBack)
        : m_inputs(inputs)
        , m_types(types)
        , m_code(code)
        , m_held(held)
        , m_whatFunctionsHandBack(whatFunctionsHandBack)
    {
    }

    String run()
    {
        if (!m_code.has(Mechanism::Arrivals, m_inputs.facts))
            return { };
        for (uint32_t index = 0; index < m_code.numberOfValues(); ++index) {
            const Operation& call = m_code.at(ValueID(index));
            if (call.opcode != Opcode::CallDirect || !call.block() || call.numberOfBoundArguments())
                continue;
            bool entersUnitThatBelievesCallers = call.role() == CodeRole::Body || call.role() == CodeRole::ParameterGuard || call.role() == CodeRole::CoroutineBody;
            if (!entersUnitThatBelievesCallers || !m_inputs.arrivals.takesWhatWasLeft(call.function()))
                continue;
            if (String refusal = judge(ValueID(index), call); !refusal.isNull())
                return refusal;
        }
        return { };
    }

private:
    struct LeftAtCallee {
        bool isAnything { false };
        Vector<TypeID> atPlaces;
    };

    static bool isPositionalPlace(const Function& function, uint32_t place)
    {
        return place >= function.firstParameterIndex() && place < function.firstParameterIndex() + function.numberOfPositionalParameters();
    }

    void listWhatThisUnitLeaves()
    {
        for (const Walker::ArrivalHeld& held : m_held) {
            if (held.kind == Walker::ArrivalHeld::Kind::MayBeEnteredByValue)
                continue;
            LeftAtCallee& left = m_left.add(held.callee.index(), LeftAtCallee()).iterator->value;
            left.isAnything |= held.kind == Walker::ArrivalHeld::Kind::Anything;
            if (left.isAnything)
                continue;
            const Function& function = m_inputs.program.at(held.callee);
            if (left.atPlaces.isEmpty())
                left.atPlaces.fill(TypeID(), function.numberOfEntryParameters());
            auto add = [&](uint32_t place, TypeID here) {
                TypeID& known = left.atPlaces[place];
                if (here && known != here)
                    known = known ? m_types.unionOf(known, here) : here;
            };
            if (held.kind == Walker::ArrivalHeld::Kind::Proven) {
                if (held.entryParameter < left.atPlaces.size())
                    add(held.entryParameter, held.proven);
                continue;
            }
            for (uint32_t place = held.entryParameter; place < left.atPlaces.size(); ++place)
                add(place, isPositionalPlace(function, place) ? TypeID(TypeTable::undefined) : TypeID(TypeTable::anything));
        }
        m_hasListedWhatIsLeft = true;
    }

    TypeID leftByThisUnitAt(FunctionID callee, uint32_t place)
    {
        if (!m_hasListedWhatIsLeft)
            listWhatThisUnitLeaves();
        auto found = m_left.find(callee.index());
        if (found == m_left.end())
            return { };
        if (found->value.isAnything)
            return TypeTable::anything;
        return place < found->value.atPlaces.size() ? found->value.atPlaces[place] : TypeID();
    }

    TypeID typeAfterPrecedingChecks(ValueID value, ValueID call) const
    {
        BlockID block = m_code.at(value).block();
        TypeID asked = m_code.at(value).type;
        auto askChecksBackFrom = [&](ValueID last) {
            for (ValueID id = last; id && id != value; id = m_code.at(id).previous) {
                const Operation& check = m_code.at(id);
                if (check.opcode != Opcode::CheckProof)
                    continue;
                ValueID checked = check.valueOperand();
                while (checked != value && !m_code.isConstant(checked) && conversionsOfForm.contains(m_code.at(checked).opcode))
                    checked = m_code.at(checked).operand(0);
                if (checked == value)
                    asked = m_types.intersectionOf(asked, check.checkedType());
            }
        };
        askChecksBackFrom(m_code.at(call).previous);
        if (m_code.at(call).block() != block)
            askChecksBackFrom(m_code.lastOperationOf(block));
        return asked;
    }

    bool isExcludedByPrecedingChecks(ValueID value, ValueID call) const
    {
        while (!isNeverType(typeAfterPrecedingChecks(value, call))) {
            if (m_code.isConstant(value) || !conversionsOfForm.contains(m_code.at(value).opcode))
                return false;
            value = m_code.at(value).operand(0);
        }
        return true;
    }

    TypeID typeThatTestsAndChecksLeaveOf(ValueID value, ValueID call) const
    {
        TypeID left = typeAfterPrecedingChecks(value, call);
        const Operation& record = m_code.at(value);
        bool namesValueOfOperand = conversionsOfForm.contains(record.opcode) || passThroughChecks.contains(record.opcode) || (record.opcode == Opcode::Narrowed && !record.narrowsHolderOfOperand() && !record.narrowsOtherResultOfOperand());
        ValueID behind = namesValueOfOperand ? record.operand(0) : ValueID();
        if (!behind || m_code.isConstant(behind) || !m_code.at(behind).type || !m_types.at(m_code.at(behind).type).isJavaScriptValue())
            return left;
        TypeID both = m_types.intersectionOf(left, typeThatTestsAndChecksLeaveOf(behind, call));
        return isNeverType(both) ? left : both;
    }

    bool liesWithin(ValueID value, TypeID bound, ValueID call)
    {
        const Operation& record = m_code.at(value);
        if (!record.type)
            return false;
        if (m_types.isSubset(record.type, bound))
            return true;
        if (m_code.isConstant(value) || !m_types.at(record.type).isJavaScriptValue())
            return false;
        if (m_types.isSubset(typeThatTestsAndChecksLeaveOf(value, call), bound))
            return true;
        static constexpr OpcodeSet callsByValue = opcodesWhere([](Opcode opcode) {
            return accessClassOf(opcode) == AccessClass::IndirectCall && resultRuleOf(opcode) == ResultRule::Unknown && opcode != Opcode::CallTable;
        });
        if (m_whatFunctionsHandBack && callsByValue.contains(record.opcode)) {
            Vector<TypeID, 8> operands;
            for (ValueID operand : record.operands())
                operands.append(operand ? m_code.at(operand).type : TypeID());
            TypeID handedBack = Typed::resultTypeOf(TypingContext { m_inputs.program, m_types, *m_whatFunctionsHandBack, TypeLevel::Proven }, record, OperandTypes { operands.span() });
            if (TypeID callee = operands[OperandIndex::Call::callee]; handedBack && callee && m_types.at(callee).isObject() && singleBuiltinOf(m_types, m_types.at(callee)))
                handedBack = m_types.wholeKindsAndBuiltinClassesOf(handedBack);
            if (handedBack && m_types.isSubset(handedBack, bound))
                return true;
        }
        if (conversionsOfForm.contains(record.opcode))
            return liesWithin(record.operand(0), bound, call);
        if (record.opcode == Opcode::Narrowed && !record.narrowsHolderOfOperand() && !record.narrowsOtherResultOfOperand()) {
            ValueID behind = record.operand(0);
            TypeID ofValueBehind = behind && !m_code.isConstant(behind) ? m_code.at(behind).type : TypeID();
            if (!ofValueBehind || !m_types.at(ofValueBehind).isJavaScriptValue())
                return false;
            TypeID takenAway = m_types.differenceOf(ofValueBehind, record.type);
            return isNeverType(m_types.intersectionOf(takenAway, record.type)) && liesWithin(behind, m_types.unionOf(bound, takenAway), call);
        }
        if (record.opcode != Opcode::Parameter || m_code.at(record.block()).isEntry || m_code.at(record.block()).isHandlerBlock())
            return false;
        if (!m_parametersBeingAsked.add(static_cast<uint64_t>(value.index()) << 32 | bound.index()).isNewEntry)
            return true;
        std::span<const ValueID> parameters = m_code.at(record.block()).parameters();
        size_t place = std::ranges::find(parameters, value) - parameters.begin();
        bool everyArrivalLiesWithin = true;
        bool hasArrival = false;
        m_code.allIncomingEdges(record.block()).forEach([&](const Edge& edge) {
            hasArrival = true;
            everyArrivalLiesWithin = everyArrivalLiesWithin && place < edge.arguments().size() && edge.arguments()[place] && liesWithin(edge.arguments()[place], bound, call);
        });
        return hasArrival && everyArrivalLiesWithin;
    }

    String judge(ValueID id, const Operation& call)
    {
        FunctionID callee = call.function();
        const Function& function = m_inputs.program.at(callee);
        bool isUnitOfCallee = callee == m_code.function();
        std::span<const ValueID> passedOperands = call.variadicOperands();
        uint32_t firstPositional = function.firstParameterIndex();
        uint32_t endOfPositional = firstPositional + function.numberOfPositionalParameters();
        for (uint32_t place = 0; place < function.numberOfEntryParameters(); ++place) {
            if (m_inputs.plan.promotedEnvironments().isRemoved(callee, place) || (call.role() == CodeRole::Body && m_inputs.plan.conventions().isScalarizedParameter(callee, place)))
                continue;
            bool isPositional = place >= firstPositional && place < endOfPositional;
            TypeID left = isUnitOfCallee ? m_inputs.arrivals.leftAt(m_types, callee, place) : leftByThisUnitAt(callee, place);
            if (left == TypeTable::anything || (isUnitOfCallee && !left))
                continue;
            ValueID operand = place < passedOperands.size() ? passedOperands[place] : ValueID();
            if (!left)
                return sentence(id, callee, place, operand, "and its unit has left nothing there"_s, { });
            if (operand && m_code.at(operand).type == left)
                continue;
            if (isUnitOfCallee && operand && m_code.at(operand).opcode == Opcode::Parameter) {
                const Block& entry = m_code.at(m_code.at(operand).block());
                if (entry.isEntry && place < entry.parameters().size() && entry.parameters()[place] == operand)
                    continue;
            }
            if (isNeverType(left)) {
                if (operand && isExcludedByPrecedingChecks(operand, id))
                    continue;
                return sentence(id, callee, place, operand, "and all that its unit has left there is never"_s, { });
            }
            TypeID believed = m_inputs.facts.typeBelievedOfEntryParameter(m_types, callee, place, left);
            if (!m_types.at(believed).isJavaScriptValue())
                continue;
            if (TypeID tested = call.role() == CodeRole::Body ? m_inputs.plan.conventions().typeBehindGuard(callee, place) : TypeID()) {
                TypeID passed = m_types.intersectionOf(believed, tested);
                believed = isNeverType(passed) ? tested : passed;
            } else if (TypeID provenByLink = isUnitOfCallee ? TypeID() : m_inputs.facts.typeOfEntryParameter(callee, place).proven; provenByLink && m_types.isSubset(provenByLink, left))
                continue;
            if (isUnitOfCallee && !walksInputBody(m_code.role()) && m_code.role() != CodeRole::ParameterGuard)
                believed = m_types.testablePartOf(believed);
            if (!operand) {
                if (place >= passedOperands.size() && isPositional && m_types.isSubset(TypeTable::undefined, believed))
                    continue;
                return sentence(id, callee, place, operand, "by what was left there the callee believes"_s, believed);
            }
            m_parametersBeingAsked.clear();
            if (!liesWithin(operand, believed, id))
                return sentence(id, callee, place, operand, "by what was left there the callee believes"_s, believed);
        }
        return { };
    }

    String sentence(ValueID call, FunctionID callee, uint32_t place, ValueID operand, ASCIILiteral what, TypeID believed) const
    {
        StringPrintStream out;
        out.print("f"_s, m_code.function().index(), " "_s, nameOf(m_code.role()), " v"_s, call.index(), " (CallDirect) hands "_s);
        if (operand)
            out.print(TypeInText { m_types, m_code.at(operand).type });
        else
            out.print("nothing"_s);
        out.print(" to entry parameter "_s, place, " of f"_s, callee.index(), ", which goes by what its callers leave, "_s, what);
        if (believed)
            out.print(" "_s, TypeInText { m_types, believed });
        return out.toString();
    }

    const WalkInputs& m_inputs;
    TypeArena& m_types;
    const Code& m_code;
    std::span<const Walker::ArrivalHeld> m_held;
    const PositionTypes* m_whatFunctionsHandBack;
    HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_parametersBeingAsked;
    HashMap<uint32_t, LeftAtCallee, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_left;
    bool m_hasListedWhatIsLeft { false };
};

} // anonymous namespace

String findCallThatHandsOverWhatItsUnitHasNotLeft(const WalkInputs& inputs, TypeArena& types, const Code& code, std::span<const Walker::ArrivalHeld> arrivalsHeld, const PositionTypes* whatFunctionsHandBack)
{
    return JudgeOfCalls(inputs, types, code, arrivalsHeld, whatFunctionsHandBack).run();
}

static String findStoreOfWhatCallerMayLend(const WalkInputs& inputs, TypeArena& types, const Code& code)
{
    CodeUnit unit = code.unit();
    if (!walksInputBody(unit.role) || !(code.opcodesOfValues().contains(Opcode::PutModuleVariable) || code.opcodesOfValues().contains(Opcode::PutEnvironmentField)))
        return { };
    if (inputs.options.shouldAssumeCalleesRetainNothing && (inputs.options.sourcesOfProofsChecked & bitOfSourceOfProofs(ProofSource::Lifetime)))
        return { };
    const LinkTables& tables = inputs.facts.linked();
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        const Operation& operation = code.at(ValueID(index));
        if (!operation.block() || (operation.opcode != Opcode::PutModuleVariable && operation.opcode != Opcode::PutEnvironmentField) || code.isConstant(operation.valueOperand()))
            continue;
        ValueID stored = underlyingCellOf(code, operation.valueOperand());
        const Operation& made = code.at(stored);
        if (made.opcode != Opcode::Parameter || !code.at(made.block()).isEntry)
            continue;
        if (FunctionID function = singleFunctionOf(types, types.at(code.at(operation.valueOperand()).type)); function && !closuresOfFunctionMayBeLent(inputs.program, function))
            continue;
        std::span<const ValueID> parameters = code.at(made.block()).parameters();
        size_t entryParameter = std::ranges::find(parameters, stored) - parameters.begin();
        if (tables.onlyCallsWhatArrivesAt(unit.function, static_cast<unsigned>(entryParameter)))
            return makeString('f', unit.function.index(), " v"_s, index, " ("_s, nameOf(operation.opcode), ") keeps entry parameter "_s, entryParameter, ", of which Link says that it is only called"_s);
    }
    return { };
}

static bool hasDominatingAccess(const Code& code, ValueID read)
{
    const Operation& asked = code.at(read);
    auto accessesSameVariable = [&](const Operation& operation) {
        if (asked.opcode == Opcode::GetModuleVariable)
            return (operation.opcode == Opcode::GetModuleVariable || operation.opcode == Opcode::PutModuleVariable) && operation.variable() == asked.variable();
        return (operation.opcode == Opcode::GetEnvironmentField || operation.opcode == Opcode::PutEnvironmentField) && operation.layout() == asked.layout() && operation.field() == asked.field() && operation.environmentOperand() == asked.environmentOperand();
    };
    auto hasAccessAtOrBefore = [&](ValueID last) {
        for (ValueID id = last; id; id = code.at(id).previous) {
            if (accessesSameVariable(code.at(id)))
                return true;
        }
        return false;
    };
    if (hasAccessAtOrBefore(code.at(read).previous))
        return true;
    BitVector seen;
    Vector<BlockID, 16> blocksWithoutIt { code.blockOf(read) };
    seen.add(code.blockOf(read).index());
    while (!blocksWithoutIt.isEmpty()) {
        BlockID block = blocksWithoutIt.takeLast();
        if (code.at(block).isEntry)
            return false;
        code.allIncomingEdges(block).forEach([&](const Edge& edge) {
            if (seen.add(edge.from.index()) && !hasAccessAtOrBefore(code.lastOperationOf(edge.from)))
                blocksWithoutIt.append(edge.from);
        });
    }
    return true;
}

static bool isDominatedByFirstStore(const WalkInputs& inputs, FunctionID unit, VariableID variable)
{
    ModuleID module = inputs.program.at(variable).module;
    if (inputs.program.at(unit).isModuleBody() || isBodyOfWrappedModule(inputs.program, unit) || inputs.program.at(unit).module != module)
        return false;
    BitVector seen;
    Vector<FunctionID, 16> toVisit { unit };
    while (!toVisit.isEmpty()) {
        FunctionID function = toVisit.takeLast();
        if (inputs.program.at(function).module != module || inputs.program.at(function).isModuleBody() || seen.set(function.index()))
            continue;
        const ThinSummary& summary = inputs.facts.summaries().at(function);
        for (const SummaryNode& node : summary.nodes) {
            if (node.kind == SummaryNode::Kind::Closure)
                toVisit.append(FunctionID(node.first));
            if (node.kind != SummaryNode::Kind::Variable)
                continue;
            if (node.first == variable.index())
                return true;
            if (FunctionID declared = inputs.program.variables[node.first].function)
                toVisit.append(declared);
        }
        for (const SummaryCall& call : summary.calls) {
            if (call.callee == SummaryCall::Callee::Binding && call.function)
                toVisit.append(call.function);
        }
    }
    return false;
}

static bool isSlotInitializedAtAllocation(const TypeArena& types, const Code& code, const Operation& read)
{
    const Operation& made = code.at(read.environmentOperand());
    if (made.opcode != Opcode::NewObject || made.layout() != read.layout())
        return false;
    auto fields = made.fields();
    std::span<const ValueID> values = made.valuesOperands();
    for (size_t index = 0; index < fields.size() && index < values.size(); ++index) {
        if (fields[index] == read.field())
            return values[index] && !(types.at(code.at(values[index]).type).primitives & Primitive::Absent);
    }
    return false;
}

static String findUnjustifiedSyntheticRead(const WalkInputs& inputs, TypeArena& types, const Code& code)
{
    if (code.syntheticVariableReads().empty())
        return { };
    BitVector valuesUsedOutsideChecks;
    auto take = [&](std::span<const ValueID> operands) {
        for (ValueID operand : operands) {
            if (operand)
                valuesUsedOutsideChecks.set(operand.index());
        }
    };
    for (uint32_t index = 0; index < code.numberOfValues(); ++index) {
        if (const Operation& operation = code.at(ValueID(index)); operation.block() && operation.opcode != Opcode::CheckProof)
            take(operation.operands());
    }
    for (uint32_t index = 0; index < code.numberOfBlocks(); ++index)
        take(code.at(BlockID(index)).terminator.operands());
    for (uint32_t index = 0; index < code.numberOfEdges(); ++index)
        take(code.at(EdgeID(index)).arguments());
    for (ValueID read : code.syntheticVariableReads()) {
        const Operation& operation = code.at(read);
        if (!operation.block() || !valuesUsedOutsideChecks.get(read.index()))
            continue;
        if (operation.opcode == Opcode::GetEnvironmentField) {
            TypeID written = inputs.program.at(operation.layout()).fields[operation.field()].declared.written();
            if (operation.isKnownInitialized() && written && (inputs.program.at(written).primitives & Primitive::Absent) && !isSlotInitializedAtAllocation(types, code, operation) && !hasDominatingAccess(code, read))
                return makeString('f', code.unit().function.index(), " v"_s, read.index(), " (GetEnvironmentField) stands for no value of the text, says that it follows its store, and nothing vouches for it: no operation on the slot stands in front of it on every way"_s);
            continue;
        }
        if (operation.opcode != Opcode::GetModuleVariable || operation.returnsRawSlot())
            continue;
        if (operation.variable().index() >= inputs.program.variables.size() || inputs.plan.variable(operation.variable()).function)
            continue;
        if (operation.isKnownInitialized() && hasDominatingAccess(code, read))
            continue;
        if (operation.isKnownInitialized() && inputs.program.at(operation.variable()).flags.contains(VariableFlags::HoldsInstance))
            continue;
        if (isDominatedByFirstStore(inputs, code.unit().function, operation.variable()))
            continue;
        if (uint16_t word = Walker::initialPrimitiveOf(inputs.program, operation.variable()); word && !(types.at(operation.type).primitives & word))
            return makeString('f', code.unit().function.index(), " v"_s, read.index(), " (GetModuleVariable) stands for no value of the text, is typed by a proof that leaves out the word that its variable is born with, and nothing vouches for it: no operation on the variable stands in front of it on every way"_s);
    }
    return { };
}

static bool verifyCodeOfCompileUnit(const WalkInputs& inputs, BuildContext& build, TypeArena& types, const PositionTypes& positions, const PositionTypes& whatFunctionsHandBack, const Code& code, std::span<const Walker::ArrivalHeld> arrivalsHeld)
{
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::OpcodeNotInForm };
    String brokenRule = verifyCode(CodeTextContext { inputs.program, types, &inputs.plan, &inputs.facts }, code, &kindOfBrokenRule);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::TypeOutOfDate;
        brokenRule = verifyTypesOfCode(TypingContext { inputs.program, types, positions }, inputs.plan.conventions(), inputs.arrivals, code);
    }
    if (brokenRule.isNull())
        brokenRule = findCallThatHandsOverWhatItsUnitHasNotLeft(inputs, types, code, arrivalsHeld, &whatFunctionsHandBack);
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::EffectsNotCounted;
        brokenRule = findOperationThatLinkHasNotCounted(inputs, types, code);
    }
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::OpcodeNotInForm;
        brokenRule = findStoreOfWhatCallerMayLend(inputs, types, code);
    }
    if (brokenRule.isNull()) {
        kindOfBrokenRule = InternalErrorKind::TypeOutOfDate;
        brokenRule = findUnjustifiedSyntheticRead(inputs, types, code);
    }
    if (brokenRule.isNull())
        return true;
    reportFailureOfCompileUnit(build, code.unit(), kindOfBrokenRule, brokenRule);
    return false;
}

static bool verifySweptCodeOfCompileUnit(const WalkInputs& inputs, BuildContext& build, TypeArena& types, const Code& code)
{
    InternalErrorKind kindOfBrokenRule { InternalErrorKind::OpcodeNotInForm };
    String brokenRule = verifySweptCode(CodeTextContext { inputs.program, types, &inputs.plan, &inputs.facts }, code, &kindOfBrokenRule);
    if (brokenRule.isNull())
        return true;
    reportFailureOfCompileUnit(build, code.unit(), kindOfBrokenRule, brokenRule);
    return false;
}

static bool notesAreTakenOfCompileUnit(CodeRole role)
{
    switch (role) {
    case CodeRole::Body:
    case CodeRole::CoroutineBody:
    case CodeRole::ParameterGuard:
        return true;
    case CodeRole::SlowBody:
    case CodeRole::GenericCallEntry:
    case CodeRole::GenericConstructEntry:
    case CodeRole::AllocatingConstructEntry:
    case CodeRole::ModuleStart:
    case CodeRole::TableEntry:
    case CodeRole::TableLookup:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static WalkInputs withRealmOfCompileWorker(const WalkInputs& inputs, ScratchRealm* realm)
{
    WalkInputs ofWorker = inputs;
    ofWorker.scratchRealm = realm;
    return ofWorker;
}

CompileWorker::CompileWorker(const CompileInputs& inputs, BuildContext& build, TypeTable& types)
    : m_inputs(inputs)
    , m_build(build)
    , m_types(types, ArenaKeeps::UntilReset)
    , m_arena(makeUniqueWithoutFastMallocCheck<Machine::Arena>())
    , m_scratchRealm(makeUniqueWithoutFastMallocCheck<ScratchRealm>())
    , m_walkInputs(withRealmOfCompileWorker(inputs.walk, m_scratchRealm.get()))
    , m_keptBetweenUnits(makeUnique<KeptBetweenUnits>(m_walkInputs, m_types))
    , m_walker(makeWalker(RoomOfFormerWalker::IsLeft))
{
}

CompileWorker::~CompileWorker() = default;

WalkerInRoom CompileWorker::makeWalker(RoomOfFormerWalker room)
{
    WalkerInRoom walker = m_keptBetweenUnits->makeWalker(m_walkInputs, m_types, m_inputs.walk.arrivals);
    if (room == RoomOfFormerWalker::IsTaken)
        walker->takeRoomOf(*m_walker);
    return walker;
}

static WalkMode walkModeFor(const WalkInputs& inputs, CodeUnit unit)
{
    const WalkOptions& options = inputs.options;
    if (!options.forcedWalkModePeriod || !walksInputBody(unit.role) || unit.role == CodeRole::SlowBody)
        return WalkMode::AsBuilt;
    if (unit.role == CodeRole::Body && inputs.program.at(unit.function).isCoroutine())
        return WalkMode::AsBuilt;
    bool isDrawn = !(drawBySeed(options.shuffleSeed, DrawnFor::WalkMode, unit.function.index(), static_cast<uint32_t>(unit.role)) % options.forcedWalkModePeriod);
    return isDrawn ? options.forcedWalkMode : WalkMode::AsBuilt;
}

bool CompileWorker::walkAndSweep(CodeUnit unit)
{
    return walkAndSweep(unit, walkModeFor(m_inputs.walk, unit));
}

bool CompileWorker::walkAndSweep(CodeUnit unit, WalkMode way)
{
    const WalkInputs& inputs = m_inputs.walk;
    CurrentWorkScope work(inputs.program, unit.function, "walk"_s);

    m_types.reset();
    if (m_walker->hasBegunUnit())
        m_walker = makeWalker(RoomOfFormerWalker::IsTaken);
    bool isWalked = m_walker->walk(unit, way);
    if (isWalked && m_walker->hasLastWalksOfLoopsToWalkAlone()) {
        WalkerInRoom first = std::exchange(m_walker, makeWalker(RoomOfFormerWalker::IsLeft));
        isWalked = m_walker->walkLastWalksOfLoopsAlone(*first);
    }
    bool withdrawsSecondCounters = isWalked && m_walker->hasUnprovenSecondCounter();
    if (withdrawsSecondCounters) {
        m_types.reset();
        m_walker = makeWalker(RoomOfFormerWalker::IsTaken);
        m_walker->refuseSecondCounters();
        isWalked = m_walker->walk(unit, way);
    }
    Code& code = m_walker->code();
    if (!isWalked) {
        m_build.diagnostics().append(m_walker->takeReports());
        reportFailureOfCompileUnit(m_build, unit, m_walker->kindOfFailure(), m_walker->failure());
        return false;
    }
    RELEASE_ASSERT(code.unit() == unit);

    if (inputs.options.verifiesCode) {
        work.setStep("verify"_s);
        if (!verifyCodeOfCompileUnit(inputs, m_build, m_types, m_walker->positions(), m_walker->positionsKnowingWhatFunctionsHandBack(), code, m_walker->arrivalsHeld())) {
            m_build.diagnostics().append(m_walker->takeReports());
            return false;
        }
    }

    work.setStep("sweep"_s);
    SweepTotals swept = Sweep(*m_roomsOfSweep).run(SweepInputs { inputs.program, inputs.facts, inputs.plan, m_types, inputs.options.shouldMisfoldBranchInSweep, inputs.options.shouldDropEdgeInSweep }, code);

    if (inputs.options.verifiesCode) {
        work.setStep("verify"_s);
        if (!verifySweptCodeOfCompileUnit(inputs, m_build, m_types, code)) {
            m_build.diagnostics().append(m_walker->takeReports());
            return false;
        }
    }

    if (inputs.options.verifiesCode) {
        work.setStep("verify"_s);
        if (String brokenRule = findMarkOfSweepThatNothingCovers(SweepInputs { inputs.program, inputs.facts, inputs.plan, m_types }, code); !brokenRule.isNull()) {
            m_build.diagnostics().append(m_walker->takeReports());
            reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::OpcodeNotInForm, brokenRule);
            return false;
        }
    }

    if (inputs.options.verifiesCode) {
        for (auto [first, second] : code.cellsMadeForOneOperation()) {
            if (!code.isNeeded(first) || !code.isNeeded(second) || !code.useCountOf(first) || code.at(first).opcode != code.at(second).opcode || isReachedPastBlockInCompileUnit(code, code.blockOf(second), code.blockOf(first)) || !cellIsUsedBehindOperationInCompileUnit(code, first, second))
                continue;
            m_build.diagnostics().append(m_walker->takeReports());
            reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::DuplicateCellAllocation, makeString("records "_s, first.index(), " and "_s, second.index()));
            return false;
        }
    }

    code.releaseAbandonedBlocks();
    for (const Walker::ArrivalHeld& held : m_walker->arrivalsHeld()) {
        if ((held.kind == Walker::ArrivalHeld::Kind::Anything || held.kind == Walker::ArrivalHeld::Kind::MayBeEnteredByValue) && inputs.facts.linked().entryByValueMayBeClosed(held.callee))
            m_functionsThatUnitMayEnterByValue.append(held.callee);
    }
    m_walker->handOverArrivals();
    m_build.diagnostics().append(m_walker->takeReports());
    Statistics::CountersOfThread counters = m_build.statistics().countersOfCurrentThread();
    counters.count(StatisticID::OperationsSwept, swept.operationsSwept);
    if (withdrawsSecondCounters)
        counters.count(StatisticID::SecondCountersWithdrawn);
    counters.count(StatisticID::BranchesFoldedToJumps, swept.branchesThatDecideNothing);
    counters.count(StatisticID::DeadValueProofChecksRemoved, swept.doorsDroppedWithTheirValues);
    counters.count(StatisticID::OverwrittenSlotStoresDropped, swept.overwrittenSlotStoresDropped);
    if (swept.allocationsSunk)
        counters.count(StatisticID::AllocationsSunk, swept.allocationsSunk);
    counters.count(StatisticID::ValuesWalked, m_walker->numberOfValuesWalked());
    counters.count(StatisticID::ValuesWalkedAndKept, m_walker->numberOfValuesWalkedAndKept());
    counters.count(StatisticID::LoopVisits, m_walker->loopVisitCount());
    counters.count(StatisticID::LoopVisitsUndone, m_walker->undoneLoopVisitCount());
    counters.count(StatisticID::LoopsRevisitedWithoutSumGuards, m_walker->unguardedSumLoopCount());
    counters.count(StatisticID::LoopsRevisitedWithoutCounterSpeculation, m_walker->numberOfRowsWhoseKeysAreReadOffLook());
    counters.count(StatisticID::LoopVisitsAborted, m_walker->abortedLoopVisitCount());
    counters.count(StatisticID::LoopEffectsPredictedFromSource, m_walker->predictedLoopEffectsCount());
    counters.count(StatisticID::LoopEffectPredictionsRefuted, m_walker->refutedLoopEffectsCount());
    counters.count(StatisticID::LoopVersioningAttemptsFailed, m_walker->failedLoopVersioningCount());
    counters.count(StatisticID::OperationsBuilt, code.numberOfValues());
    counters.count(StatisticID::CallsInlined, code.inlineFrames().size() - 1);
    if (uint32_t guesses = m_walker->speculationCount()) {
        counters.count(StatisticID::SpeculationsEmitted, guesses);
        counters.count(StatisticID::WalkLoopsVersioned, m_walker->versionedLoopCount());
        counters.count(StatisticID::IterationRestartsEmitted, m_walker->iterationRestartCount());
        counters.count(StatisticID::StoresUndoneAtIterationRestarts, m_walker->undoneStoreCount());
        counters.count(StatisticID::EmptySlotCasesEmitted, m_walker->emptySlotCaseCount());
    }
    if (uint32_t assignments = m_walker->expandedObjectAssignCount())
        counters.count(StatisticID::ObjectAssignCallsExpanded, assignments);
    if (uint32_t joins = m_walker->duplicatedJoinCount())
        counters.count(StatisticID::JoinsDuplicatedPerPredecessor, joins);
    if (uint32_t questions = m_walker->threadedConditionCount())
        counters.count(StatisticID::RepeatedConditionsJumpThreaded, questions);
    if (uint32_t callees = m_walker->fieldCalleeSpeculationCount())
        counters.count(StatisticID::CalleesSpeculatedFromFieldWrites, callees);
    if (uint32_t questions = m_walker->genericInstanceCheckCount())
        counters.count(StatisticID::PlainInstanceChecksLeftGeneric, questions);
    if (uint32_t questions = m_walker->genericMemberLookupCount())
        counters.count(StatisticID::MemberLookupChecksLeftGeneric, questions);
    if (uint32_t objects = m_walker->mergedFreshObjectCount())
        counters.count(StatisticID::FreshObjectsMergedAtJoins, objects);
    static_assert(static_cast<unsigned>(StatisticID::UntestableProofsSkipped) - static_cast<unsigned>(StatisticID::EntryParameterProofsChecked) == numberOfEnumerators<ProofSource>);
    for (unsigned source = 0; source < numberOfEnumerators<ProofSource>; ++source) {
        if (uint32_t checked = m_walker->numberOfProofsChecked(static_cast<ProofSource>(source)))
            counters.count(static_cast<StatisticID>(static_cast<unsigned>(StatisticID::EntryParameterProofsChecked) + source), checked);
    }
    if (inputs.options.checksProofs) {
        counters.count(StatisticID::UntestableProofsSkipped, m_walker->untestableProofCount());
        counters.count(StatisticID::FreshCellsSkippedAtReturns, m_walker->skippedFreshCellCount());
    }
    if (uint32_t answered = m_walker->foldedProofCheckCount())
        counters.count(StatisticID::ProofChecksFolded, answered);
    if (uint32_t told = m_walker->impliedProofCount())
        counters.count(StatisticID::ProofsImpliedByRepresentation, told);
    if (uint32_t doors = m_walker->unsatisfiableProofCheckCount())
        counters.count(StatisticID::UnsatisfiableProofChecks, doors);

    if (inputs.options.takesNotes && notesAreTakenOfCompileUnit(unit.role)) {
        WalkNotes notes = notesOfSweptCode(m_types, code, AOT::hasFlag(m_build.options().reports, Wire::ReportSet::AccessCounts), AOT::hasFlag(m_build.options().reports, Wire::ReportSet::Flow) && AOT::hasFlag(m_build.options().reports, Wire::ReportSet::AccessCounts));
        if (unit.role != CodeRole::ParameterGuard)
            counters.count(StatisticID::ValuesInDouble, notes.numberOfValuesInDouble);
        if (!notes.notes.isEmpty())
            m_notes.append(WTF::move(notes));
    }
    return true;
}

static bool storesOnlyWhatFitsByItsMaking(const WalkInputs& inputs, TypeArena& types, const BodyIndex& index)
{
    static constexpr OpcodeSet operationsThatStoreIntoPlaces { Opcode::PutModuleVariable, Opcode::PutEnvironmentField, Opcode::NewObject, Opcode::NewClosure, Opcode::TryReExportFields };
    if (!index.opcodes().intersects(operationsThatStoreIntoPlaces))
        return true;
    const LinkTables& linked = inputs.facts.linked();
    auto heldIn = [&](LayoutID layout, uint32_t field) -> TypeID {
        const LinkedCell* cell = linked.findCellOfField({ layout, field });
        return cell && cell->everyWriterIsKnown ? cell->proven : TypeID();
    };
    auto fits = [&](ValueID value, TypeID held) {
        if (!value || !held)
            return true;
        Type type = types.at(held);
        if (!type.isJavaScriptValue() || type.isAnything())
            return true;
        ValueID made = index.stripCasts(value);
        while (made && !index.isParameter(made) && (index.value(made).opcode() == Opcode::FinishClass || index.value(made).opcode() == Opcode::NewClass)) {
            BodyValue handsOn = index.value(made);
            made = index.stripCasts(handsOn.opcode() == Opcode::FinishClass ? handsOn.as<Opcode::FinishClass>().classObjectOperand() : handsOn.as<Opcode::NewClass>().constructorOperand());
        }
        if (!made || index.isParameter(made))
            return false;
        BodyValue definition = index.value(made);
        switch (definition.opcode()) {
        case Opcode::String:
            return !!(type.primitives & Primitive::String);
        case Opcode::BigInt:
            return !!(type.primitives & Primitive::BigInt);
        case Opcode::Double:
            return (type.primitives & Primitive::Number) == Primitive::Number && type.integers == Range::full() && type.boundsOfFractions.isEmpty();
        case Opcode::Undefined:
            return !!(type.primitives & Primitive::Undefined);
        case Opcode::Null:
            return !!(type.primitives & Primitive::Null);
        case Opcode::Absent:
            return true;
        case Opcode::Boolean:
            return !!(type.primitives & (definition.boolean() ? Primitive::True : Primitive::False));
        case Opcode::Integer:
            return type.primitives & Primitive::Integer && Range::one(definition.integer()).isSubsetOf(type.integers);
        case Opcode::NewClosure:
            return types.isSubset(types.forFunction(definition.as<Opcode::NewClosure>().function()), held);
        default:
            return false;
        }
    };
    bool allFit = true;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (!allFit || !operationsThatStoreIntoPlaces.contains(value.opcode()))
                continue;
            if (value.opcode() == Opcode::PutModuleVariable) {
                VariableID variable = value.variable();
                bool isTyped = variable.index() < linked.variables.size() && linked.variable(variable).everyWriterIsKnown && !inputs.plan.runTimeMayFill(variable);
                allFit = fits(value.valueOperand(), isTyped ? linked.variable(variable).proven : TypeID());
                continue;
            }
            if (value.opcode() == Opcode::PutEnvironmentField) {
                allFit = fits(value.valueOperand(), heldIn(value.layout(), value.field()));
                continue;
            }
            if (value.opcode() == Opcode::TryReExportFields) {
                auto reExport = value.as<Opcode::TryReExportFields>();
                uint32_t place = reExport.keyIsFirstCapture() ? 1 : 0;
                bool isTyped = reExport.function().index() + 1 < linked.firstCaptureOfFunction.size() && place < linked.firstCaptureOfFunction[reExport.function().index() + 1] - linked.firstCaptureOfFunction[reExport.function().index()];
                if (isTyped && linked.capture(reExport.function(), place).everyWriterIsKnown)
                    allFit = fits(reExport.sourceOperand(), linked.capture(reExport.function(), place).proven);
                continue;
            }
            if (value.opcode() == Opcode::NewClosure) {
                FunctionID function = value.as<Opcode::NewClosure>().function();
                uint32_t numberTyped = function.index() + 1 < linked.firstCaptureOfFunction.size() ? linked.firstCaptureOfFunction[function.index() + 1] - linked.firstCaptureOfFunction[function.index()] : 0;
                uint32_t place = 0;
                for (ValueID captured : value.as<Opcode::NewClosure>().capturesOperands()) {
                    if (place < numberTyped && linked.capture(function, place).everyWriterIsKnown)
                        allFit = allFit && fits(captured, linked.capture(function, place).proven);
                    ++place;
                }
                continue;
            }
            auto allocation = value.as<Opcode::NewObject>();
            if (!allocation.layout() || inputs.program.at(allocation.layout()).kind != LayoutKind::Environment)
                continue;
            std::span<const ValueID> values = value.variadicOperands();
            size_t place = 0;
            for (uint32_t field : allocation.fields()) {
                ValueID listed = place < values.size() ? index.stripCasts(values[place]) : ValueID();
                if (listed && (index.isParameter(listed) || index.value(listed).opcode() != Opcode::Undefined))
                    allFit = allFit && fits(listed, heldIn(allocation.layout(), field));
                ++place;
            }
        }
    });
    return allFit;
}

static bool entryBlockStoresVariablesInFrontOfTheirFirstReaders(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    std::span<const LinkedVariableStoredFirst> claims = inputs.facts.linked().variablesStoredFirstIn(unit.function);
    if (claims.empty())
        return true;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> stored;
    Vector<FunctionID, 4> used;
    bool isShown = true;
    bool isEntryBlock = true;
    UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> functionsFoundShown;
    auto look = [&](VariableID storedByOperation) {
        for (FunctionID function : used) {
            if (!isShown || functionsFoundShown.contains(function.index()))
                continue;
            bool isPassedBy = false;
            for (const LinkedVariableStoredFirst& claim : std::ranges::equal_range(claims, function, { }, &LinkedVariableStoredFirst::functionWhoseUseCounts)) {
                isPassedBy |= claim.variable == storedByOperation;
                isShown &= claim.variable == storedByOperation || stored.contains(claim.variable.index()) || inputs.plan.deferredDefinitions().findDeferred(claim.variable);
            }
            if (isShown && !isPassedBy)
                functionsFoundShown.add(function.index());
        }
    };
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            listFunctionsUsedByOperation(inputs.program, inputs.facts.linked(), unit.function, index, value, used);
            look(value.opcode() == Opcode::PutModuleVariable ? value.variable() : VariableID());
            if (!isEntryBlock || value.opcode() != Opcode::PutModuleVariable)
                continue;
            ValueID made = index.stripCasts(value.valueOperand());
            if (made && (index.isParameter(made) || index.value(made).opcode() != Opcode::Absent))
                stored.add(value.variable().index());
        }
        listFunctionsUsedBy(inputs.program, inputs.facts.linked(), unit.function, index, block.terminator(), true, 0, used);
        look(VariableID());
        isEntryBlock = false;
    });
    return isShown;
}

static bool readsVariableOfAnotherFileStoredFirst(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    if (!index.opcodes().contains(Opcode::GetModuleVariable))
        return false;
    ModuleID own = inputs.program.at(unit.function).module;
    bool reads = false;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (value.opcode() == Opcode::GetModuleVariable && inputs.program.at(value.variable()).module != own)
                reads |= inputs.facts.linked().variables[value.variable().index()].isFilled;
        }
    });
    return reads;
}

static bool readsScopesWhoseLivesMayHaveEnded(const WalkInputs& inputs, const BodyIndex& index)
{
    static constexpr OpcodeSet accessesToScopes { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
    return (inputs.options.sourcesOfProofsChecked & bitOfSourceOfProofs(ProofSource::Lifetime)) && index.opcodes().intersects(accessesToScopes);
}

static bool hasReadsVerifiedAtRunTime(const WalkInputs& inputs, const BodyIndex& index)
{
    static constexpr OpcodeSet readsByName { Opcode::PropGet, Opcode::PropHas, Opcode::PropHasOwn };
    return inputs.options.checksWalk && index.opcodes().intersects(readsByName);
}

static bool looksUnderKeyThatHasPreparation(const WalkInputs& inputs, FunctionID function, const BodyIndex& index)
{
    if (!index.opcodes().intersects(accessesUnderComputedKeys))
        return false;
    const LinkTables& linked = inputs.facts.linked();
    bool looks = false;
    index.body().forEachBlock([&](BodyBlock block) {
        for (BodyValue value : block.values()) {
            if (!accessesUnderComputedKeys.contains(value.opcode()))
                continue;
            PropertyKeyID key = linked.keyAt(function, value.id());
            looks |= key && linked.lookupUnderKeyMayRunPreparation(inputs.program, key);
        }
    });
    return looks;
}

static bool isWrittenStraight(const WalkInputs& inputs, CodeUnit unit, const BodyIndex& index)
{
    static constexpr OpcodeSet operationsThatTypesMakeNoCheaper = inputPassThroughOpcodes | OpcodeSet {
        Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt, Opcode::Builtin, Opcode::NativeBinding,
        Opcode::GetModuleVariable, Opcode::PutModuleVariable, Opcode::TypeTest, Opcode::IsLinkedBelow, Opcode::ThrowModuleFailure, Opcode::EvaluateModule, Opcode::RecordRequire, Opcode::RequireModule,
        Opcode::NewClosure, Opcode::NewRegExp, Opcode::NewClass, Opcode::NewClassToken, Opcode::ResolveHeritage, Opcode::DefineMethod, Opcode::FinishClass,
    };
    if ((unit.role != CodeRole::SlowBody && unit.role != CodeRole::Body) || (inputs.facts.isTakenToRunOnce(unit.function) && !isBodyWrittenInAnotherFile(inputs.program, unit.function)))
        return false;
    if (inputs.facts.beforeEvaluation().holdsIn(unit.function))
        return false;
    if (inputs.plan.promotedEnvironments().isMaker(unit.function) || inputs.plan.promotedEnvironments().numberOfRemovedCapturesOf(unit.function))
        return false;
    if (readsScopesWhoseLivesMayHaveEnded(inputs, index))
        return false;
    if (inputs.options.forcesStraightCompilation)
        return true;
    if (hasReadsVerifiedAtRunTime(inputs, index))
        return false;
    if (unit.role != CodeRole::Body)
        return !someLoopGivesSlowVersionSomethingToReckon(index) && !index.opcodes().contains(Opcode::Builtin) && !looksUnderKeyThatHasPreparation(inputs, unit.function, index);
    if (!inputs.program.at(unit.function).isModuleBody())
        return false;
    return !index.hasLoops() && (index.opcodes() | operationsThatTypesMakeNoCheaper) == operationsThatTypesMakeNoCheaper;
}

std::optional<CompiledCode> CompileWorker::compileStraight(CodeUnit unit)
{
    const BodyIndex* index = m_inputs.walk.facts.bodies().find(unit.function);
    if (!index || !isWrittenStraight(m_inputs.walk, unit, *index) || !hasStraightForm(m_inputs.generation, unit, *index))
        return std::nullopt;
    m_types.reset();
    if (!entryBlockStoresVariablesInFrontOfTheirFirstReaders(m_inputs.walk, unit, *index) || readsVariableOfAnotherFileStoredFirst(m_inputs.walk, unit, *index))
        return std::nullopt;
    if (!storesOnlyWhatFitsByItsMaking(m_inputs.walk, m_types, *index) || !m_inputs.walk.facts.summaries().at(unit.function).scopesStoredFirst.isEmpty())
        return std::nullopt;
    Code code;
    code.reset(unit);
    std::optional<CompiledCode> compiled = generateStraightCode(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, code, *index, m_totals);
    if (!compiled || !compiled->unit) {
        m_arena->reset();
        return std::nullopt;
    }
    if (const LinkTables& linked = m_inputs.walk.facts.linked(); !linked.calls.isEmpty()) {
        static const OpcodeSet opcodesThatMayRunOtherCode = opcodesWhere([](Opcode opcode) {
            return declaredBoundInTable(opcode).mayRunOtherCode() || operandsMayRunCode(opcode);
        });
        if (index->opcodes().intersects(opcodesThatMayRunOtherCode)) {
            index->body().forEachBlock([&](BodyBlock block) {
                for (BodyValue value : block.values()) {
                    if (!opcodesThatMayRunOtherCode.contains(value.opcode()))
                        continue;
                    if (FunctionID callee = linked.calleeAt(unit.function, value.id())) {
                        m_inputs.walk.arrivals.leaveAnything(callee);
                        if (linked.entryByValueMayBeClosed(callee))
                            m_functionsThatUnitMayEnterByValue.append(callee);
                    }
                }
            });
        }
    }
    m_build.statistics().count(StatisticID::UnitsCompiled);
    m_build.statistics().count(StatisticID::StraightUnitsCompiled);
    return compiled;
}

std::optional<CompiledCode> CompileWorker::compileEntryByHand(CodeUnit unit)
{
    const WalkInputs& inputs = m_inputs.walk;
    if (unit.role != CodeRole::GenericCallEntry || inputs.options.checksProofs)
        return std::nullopt;
    bool nobodyEnters = inputs.arrivals.entriesAreClosed(unit.function);

    m_types.reset();
    Code code;
    code.reset(unit);
    bool believesCallers = withoutMechanismsMissingDependencies(inputs.facts.mechanisms()).contains(Mechanism::Arrivals);
    std::optional<CompiledCode> compiled = generateEntryWrittenByHand(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, code, scopedLambda<TypeID(unsigned)>([&](unsigned entryParameter) -> TypeID {
        TypeID known = believesCallers ? inputs.arrivals.provenAt(m_types, unit.function, entryParameter) : TypeID();
        TypeID believed = inputs.facts.typeBelievedOfEntryParameter(m_types, unit.function, entryParameter, known);
        if (!m_types.at(believed).isJavaScriptValue())
            return { };
        TypeID asked = m_types.testablePartOf(believed);
        return m_types.isSubset(TypeTable::anything, asked) ? TypeID() : asked;
    }), m_totals, nobodyEnters);
    if (!compiled || !compiled->unit) {
        m_arena->reset();
        return std::nullopt;
    }
    m_build.statistics().count(StatisticID::UnitsCompiled);
    m_build.statistics().count(StatisticID::GenericEntryThunksEmitted);
    if (nobodyEnters)
        m_build.statistics().count(StatisticID::GenericEntriesProvenUnreachable);
    return compiled;
}

std::optional<CompiledCode> CompileWorker::compile(CodeUnit unit)
{
    CurrentWorkScope work(m_inputs.walk.program, unit.function, "walk"_s);

    m_arena->reset();
    if (std::optional<CompiledCode> straight = compileStraight(unit))
        return straight;
    if (std::optional<CompiledCode> entry = compileEntryByHand(unit))
        return entry;
    std::optional<uint64_t> instructionsAtStart = instructionsOfCompileThreadSoFar(m_build);
    if (!walkAndSweep(unit))
        return std::nullopt;
    std::optional<uint64_t> instructionsAfterSweep = instructionsOfCompileThreadSoFar(m_build);

    RELEASE_ASSERT(m_walker->arrivalsHeld().empty());
    work.setStep("emit"_s);
    const Code& swept = m_walker->code();
    uint32_t slices = m_walker->foldedSliceCount();
    Vector<Walker::CountOfUnit, 4> counts(m_walker->countsOfUnit());
    static constexpr uint32_t mostValuesWithWalker = 1 << 16;
    if (swept.numberOfValues() > mostValuesWithWalker) {
        m_walker = makeWalker(RoomOfFormerWalker::IsLeft);
        m_roomsOfSweep = makeUniqueWithoutFastMallocCheck<RoomsOfSweep>();
    }
    CompiledCode code = generateCode(CodeGenerationWorker { m_build, m_types, *m_arena }, m_inputs.generation, swept, m_totals);
    std::optional<uint64_t> instructionsAfterEncoding = instructionsOfCompileThreadSoFar(m_build);
    if (instructionsAtStart && instructionsAfterSweep && instructionsAfterEncoding) {
        m_instructionsOfSteps.walkAndSweep += *instructionsAfterSweep - *instructionsAtStart;
        m_instructionsOfSteps.emitToEncode += *instructionsAfterEncoding - *instructionsAfterSweep;
    }
    if (!code.unit) {
        RELEASE_ASSERT(m_build.diagnostics().hasErrors());
        return std::nullopt;
    }
    RELEASE_ASSERT(code.unit == unit);
    if (unitMentionsThreadArenaID(code.body)) {
        reportFailureOfCompileUnit(m_build, unit, InternalErrorKind::Unknown, "the code mentions an id from the thread's arena, which the code outlives"_s);
        return std::nullopt;
    }

    Statistics& statistics = m_build.statistics();
    statistics.count(StatisticID::UnitsCompiled);
    if (slices)
        statistics.count(StatisticID::SlicesFoldedToArrayLiterals, slices);
    for (const Walker::CountOfUnit& count : counts)
        statistics.count(count.statistic, count.amount);
    if (uint32_t blocks = swept.numberOfBlocksPlacedOtherwise()) [[unlikely]]
        statistics.count(StatisticID::BlocksPlacedOtherwise, blocks);
    return code;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
