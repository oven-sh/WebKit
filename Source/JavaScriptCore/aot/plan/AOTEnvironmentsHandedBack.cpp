/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnvironmentsHandedBack.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCellFormat.h"
#include "AOTFacts.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTOpcodeSets.h"
#include "AOTSlotPlan.h"
#include "AOTThinSummary.h"
#include "AOTThreads.h"
#include "MarkedSpace.h"
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static BitVector findEnvironmentsThatPositionsTakeOrNothing(const Program& program)
{
    BitVector environments;
    auto note = [&](TypeID written) {
        if (!written || holdsOnlyPrimitives(written))
            return;
        Type type = program.at(written);
        if (type.environment && type.primitives)
            environments.set(type.environment.index());
    };
    for (const Layout& layout : program.layouts) {
        if (layout.kind != LayoutKind::Environment && layout.kind != LayoutKind::CoroutineFrame)
            continue;
        for (const Field& field : layout.fields)
            note(field.declared.written());
    }
    for (const Function& function : program.functions) {
        for (const DeclaredCapture& capture : function.captures)
            note(capture.written());
    }
    return environments;
}

namespace {

struct BodyOfMakerInEnvironmentsHandedBack {
    const Program& program;
    const Facts& facts;
    FunctionID function;
    const BodyIndex& index;
    const Vector<uint32_t>& numberOfFunctionsThatCapture;
    uint32_t numberOfEnvironmentsJudged { 0 };
    Vector<uint32_t, 64> lastJudgedThatBlockKeeps { };
    Vector<uint32_t, 64> lastJudgedWhoseWaysPassBlock { };
    Vector<uint32_t, 64> depthsBelowEntry { };
    Vector<uint32_t, 64> leastDepthsAboveWhatWaysOnReach { };
};

struct EnvironmentJudgedInEnvironmentsHandedBack {
    ValueID environment;
    bool isHandedToFunctionEnteredItself { false };
    bool isTakenByMoreThanClosuresMadeForCalls { false };
    Vector<BlockID, 2> blocksThatKeep { };
    Vector<FunctionID, 2> functionsOfClosuresLent { };
    Vector<FunctionID, 2> functionsOfClosuresMadeForCalls { };
};

struct EnvironmentFoundInEnvironmentsHandedBack {
    LayoutID layout;
    bool isHandedBack { false };
    bool isTakenByClosuresMadeForCallsAlone { false };
};

struct CallThatClosureIsMadeFor {
    ValueID call;
    size_t position { 0 };
};

} // anonymous namespace

static TypeID typeWrittenOfValueInEnvironmentsHandedBack(const BodyOfMakerInEnvironmentsHandedBack& body, ValueID value)
{
    const BodyIndex& index = body.index;
    while (value) {
        if (index.isParameter(value)) {
            BodyBlock entry = index.block(entryBlock);
            for (unsigned parameter = 0; parameter < entry.numberOfParameters(); ++parameter) {
                if (entry.parameter(parameter) != value)
                    continue;
                const DeclaredType* declared = body.program.at(body.function).declaredEntryParameter(parameter);
                return declared ? declared->written() : TypeID();
            }
            return { };
        }
        BodyValue made = index.value(value);
        if (made.opcode() == Opcode::Convert)
            return made.as<Opcode::Convert>().declared().written();
        if (!inputPassThroughOpcodes.contains(made.opcode()))
            return { };
        value = made.operand(0);
    }
    return { };
}

static bool receiverIsWrittenAsTableInEnvironmentsHandedBack(const BodyOfMakerInEnvironmentsHandedBack& body, const BodyValue& call)
{
    TypeID written = typeWrittenOfValueInEnvironmentsHandedBack(body, call.operand(OperandIndex::Call::thisValue));
    if (!written || holdsOnlyPrimitives(written))
        return false;
    Type type = body.program.at(written);
    type.primitives &= ~Primitive::UndefinedOrNull;
    const ClassPart* onlyClass = singleBuiltinClassOf(body.program, type);
    return onlyClass && (onlyClass->builtinClass == BuiltinClasses::Map || onlyClass->builtinClass == BuiltinClasses::Set);
}

static std::optional<CallThatClosureIsMadeFor> findCallThatClosureIsMadeFor(const BodyIndex& index, ValueID closure)
{
    for (ValueID name = closure;;) {
        ValueID taker = index.onlyUsingValue(name);
        if (!taker)
            return std::nullopt;
        BodyValue user = index.value(taker);
        Opcode opcode = user.opcode();
        if (opcode != Opcode::Call && opcode != Opcode::CallDirect) {
            if (!inputPassThroughOpcodes.contains(opcode) || user.operand(0) != name)
                return std::nullopt;
            name = taker;
            continue;
        }
        std::span<const ValueID> arguments = user.argumentsOperands();
        size_t position = std::ranges::find(arguments, name) - arguments.begin();
        if (position == arguments.size())
            return std::nullopt;
        return CallThatClosureIsMadeFor { taker, position };
    }
}

static bool callBorrowsClosureMadeForIt(const BodyOfMakerInEnvironmentsHandedBack& body, CallThatClosureIsMadeFor taking)
{
    BodyValue call = body.index.value(taking.call);
    const LinkTables& linked = body.facts.linked();
    if (call.opcode() == Opcode::CallDirect)
        return linked.onlyCallsWhatArrivesAt(call.function(), static_cast<unsigned>(taking.position));
    if (FunctionID callee = linked.calleeAt(body.function, taking.call))
        return linked.onlyCallsWhatArrivesAt(callee, body.program.at(callee).firstParameterIndex() + static_cast<unsigned>(taking.position));
    return receiverIsWrittenAsTableInEnvironmentsHandedBack(body, call);
}

static bool leavesCellToItsFrames(const BodyOfMakerInEnvironmentsHandedBack& body, const BodyValue& user, uint32_t operandIndex, std::optional<CallThatClosureIsMadeFor> callThatClosureIsMadeFor)
{
    switch (user.opcode()) {
    case Opcode::GetEnvironmentField:
    case Opcode::PutEnvironmentField:
        return !operandIndex;
    case Opcode::CallDirect:
        return true;
    case Opcode::NewClosure:
        return callThatClosureIsMadeFor && callBorrowsClosureMadeForIt(body, *callThatClosureIsMadeFor);
    default:
        return false;
    }
}

static void findHowFarWaysOnLeadOutOfBlocksAbove(BodyOfMakerInEnvironmentsHandedBack& body)
{
    const BodyIndex& index = body.index;
    body.depthsBelowEntry.fill(0, index.numberOfBlocks());
    body.leastDepthsAboveWhatWaysOnReach.fill(UINT32_MAX, index.numberOfBlocks());
    for (BlockID id : index.blocksInDominatorOrder()) {
        BlockID above = index.immediateDominator(id);
        body.depthsBelowEntry[id.index()] = above ? body.depthsBelowEntry[above.index()] + 1 : 1;
    }
    for (uint32_t block = index.numberOfBlocks(); block--;) {
        if (!body.depthsBelowEntry[block])
            continue;
        BodyTerminator end = index.block(BlockID(block)).terminator();
        uint32_t least = end.kind() == TerminatorKind::Return ? 0 : UINT32_MAX;
        for (BodyTarget target : end.targets()) {
            BlockID above = index.immediateDominator(target.block());
            least = std::min(least, above ? body.depthsBelowEntry[above.index()] : 0);
            if (target.block().index() > block)
                least = std::min(least, body.leastDepthsAboveWhatWaysOnReach[target.block().index()]);
        }
        body.leastDepthsAboveWhatWaysOnReach[block] = least;
    }
}

static ALWAYS_INLINE void findMentionsOfEnvironmentsJudged(const BodyOfMakerInEnvironmentsHandedBack& body, std::span<EnvironmentJudgedInEnvironmentsHandedBack> judged)
{
    const BodyIndex& index = body.index;
    ValueID first = judged.front().environment;
    ValueID last = judged.back().environment;
    auto judgedThatIs = [&](ValueID operand) -> EnvironmentJudgedInEnvironmentsHandedBack* {
        if (operand < first || operand > last)
            return nullptr;
        auto found = std::ranges::lower_bound(judged, operand, { }, &EnvironmentJudgedInEnvironmentsHandedBack::environment);
        return found != judged.end() && found->environment == operand ? &*found : nullptr;
    };
    BlockID firstMaking = index.blockOf(first);
    for (BlockID id : index.blocksInDominatorOrder()) {
        if (id < firstMaking)
            continue;
        BodyBlock block = index.block(id);
        for (BodyValue user : block.values()) {
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                EnvironmentJudgedInEnvironmentsHandedBack* mentioned = judgedThatIs(operand);
                if (!mentioned)
                    return;
                std::optional<CallThatClosureIsMadeFor> callThatClosureIsMadeFor = user.opcode() == Opcode::NewClosure ? findCallThatClosureIsMadeFor(index, user.id()) : std::nullopt;
                bool leavesCell = leavesCellToItsFrames(body, user, operandIndex, callThatClosureIsMadeFor);
                mentioned->isHandedToFunctionEnteredItself |= user.opcode() == Opcode::CallDirect;
                if (callThatClosureIsMadeFor)
                    mentioned->functionsOfClosuresMadeForCalls.append(user.function());
                else
                    mentioned->isTakenByMoreThanClosuresMadeForCalls |= !leavesCell || user.opcode() == Opcode::CallDirect;
                if (leavesCell && user.opcode() == Opcode::NewClosure)
                    mentioned->functionsOfClosuresLent.append(user.function());
                if (!leavesCell)
                    mentioned->blocksThatKeep.append(id);
            });
        }
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            if (EnvironmentJudgedInEnvironmentsHandedBack* mentioned = judgedThatIs(operand)) {
                mentioned->blocksThatKeep.append(id);
                mentioned->isTakenByMoreThanClosuresMadeForCalls = true;
            }
        });
    }
}

static bool closuresMadeForCallsAloneTake(const BodyOfMakerInEnvironmentsHandedBack& body, EnvironmentJudgedInEnvironmentsHandedBack& judged)
{
    if (judged.isTakenByMoreThanClosuresMadeForCalls)
        return false;
    std::ranges::sort(judged.functionsOfClosuresMadeForCalls);
    size_t numberOfFunctions = std::ranges::unique(judged.functionsOfClosuresMadeForCalls).begin() - judged.functionsOfClosuresMadeForCalls.begin();
    return numberOfFunctions && numberOfFunctions == body.numberOfFunctionsThatCapture[body.index.value(judged.environment).as<Opcode::NewObject>().layout().index()];
}

static bool someWayKeepsNothing(BodyOfMakerInEnvironmentsHandedBack& body, EnvironmentJudgedInEnvironmentsHandedBack& judged)
{
    const BodyIndex& index = body.index;
    BlockID making = index.blockOf(judged.environment);
    LayoutID layout = index.value(judged.environment).as<Opcode::NewObject>().layout();
    std::ranges::sort(judged.functionsOfClosuresLent);
    size_t numberOfFunctionsLent = std::ranges::unique(judged.functionsOfClosuresLent).begin() - judged.functionsOfClosuresLent.begin();
    bool closuresLentAloneTakeIt = numberOfFunctionsLent && numberOfFunctionsLent == body.numberOfFunctionsThatCapture[layout.index()];
    if (judged.blocksThatKeep.contains(making) || (!judged.isHandedToFunctionEnteredItself && !closuresLentAloneTakeIt))
        return false;

    if (body.depthsBelowEntry.isEmpty())
        findHowFarWaysOnLeadOutOfBlocksAbove(body);
    if (judged.blocksThatKeep.isEmpty())
        return body.leastDepthsAboveWhatWaysOnReach[making.index()] < body.depthsBelowEntry[making.index()];
    if (body.lastJudgedThatBlockKeeps.isEmpty()) {
        body.lastJudgedThatBlockKeeps.fill(0, index.numberOfBlocks());
        body.lastJudgedWhoseWaysPassBlock.fill(0, index.numberOfBlocks());
    }
    uint32_t number = ++body.numberOfEnvironmentsJudged;
    uint32_t endOfBlocksThatKeep = 0;
    for (BlockID id : judged.blocksThatKeep) {
        body.lastJudgedThatBlockKeeps[id.index()] = number;
        endOfBlocksThatKeep = std::max(endOfBlocksThatKeep, id.index() + 1);
    }
    Vector<BlockID, 16> worklist { making };
    body.lastJudgedWhoseWaysPassBlock[making.index()] = number;
    while (!worklist.isEmpty()) {
        BlockID id = worklist.takeLast();
        if (!index.loopOf(id) && id.index() >= endOfBlocksThatKeep) {
            if (body.leastDepthsAboveWhatWaysOnReach[id.index()] < body.depthsBelowEntry[making.index()])
                return true;
            continue;
        }
        BodyTerminator end = index.block(id).terminator();
        if (end.kind() == TerminatorKind::Return)
            return true;
        for (BodyTarget target : end.targets()) {
            if (!index.strictlyDominates(making, target.block()))
                return true;
            uint32_t to = target.block().index();
            if (body.lastJudgedThatBlockKeeps[to] != number && std::exchange(body.lastJudgedWhoseWaysPassBlock[to], number) != number)
                worklist.append(target.block());
        }
    }
    return false;
}

static constexpr bool collectorTakesLinkForNoCellIn(SlotRepresentation representation)
{
    SlotTraits traits = traitsOf(representation);
    return traits.width == sizeof(uint64_t) && !traits.isNarrowReference;
}
static_assert(collectorTakesLinkForNoCellIn(SlotRepresentation::JSValue) && collectorTakesLinkForNoCellIn(SlotRepresentation::Cell) && collectorTakesLinkForNoCellIn(SlotRepresentation::OptionalDouble));
static_assert(!collectorTakesLinkForNoCellIn(SlotRepresentation::Cell32) && !collectorTakesLinkForNoCellIn(SlotRepresentation::Int32));

static const PlacedSlot* findSlotThatHoldsLinkWhileCellWaits(const PlannedLayout& planned)
{
    for (FieldIndex field = 0; field < planned.numberOfFields(); ++field) {
        const PlacedSlot& slot = planned.slots[field];
        if (slot.storage == SlotStorage::Inline && !(slot.offset % sizeof(uint64_t)) && collectorTakesLinkForNoCellIn(slot.representation))
            return &slot;
    }
    return nullptr;
}

EnvironmentsHandedBack findEnvironmentsHandedBack(const Program& program, const Facts& facts, BuildContext& build)
{
    if (program.admitsInspector || !withoutMechanismsMissingDependencies(facts.mechanisms()).contains(Mechanism::CellsInFrames))
        return { };

    Vector<FunctionID> makers;
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        if (!facts.summaries().at(FunctionID(index)).environmentsMade.isEmpty() && !program.functions[index].isCoroutine())
            makers.append(FunctionID(index));
    }
    if (makers.isEmpty())
        return { };
    Vector<uint32_t> numberOfFunctionsThatCapture;
    numberOfFunctionsThatCapture.fill(0, program.layouts.size());
    for (const Function& function : program.functions) {
        for (const DeclaredCapture& capture : function.captures) {
            TypeID written = capture.written();
            if (LayoutID environment = written && !holdsOnlyPrimitives(written) ? program.at(written).environment : LayoutID())
                ++numberOfFunctionsThatCapture[environment.index()];
        }
    }
    Vector<Vector<EnvironmentFoundInEnvironmentsHandedBack, 2>> found(makers.size());
    forEachIndexInParallel(build.parallelLoops(), makers.size(), [&](unsigned place) {
        const BodyIndex* index = facts.bodies().find(makers[place]);
        if (!index || index->hasIrreducibleFlow() || program.at(makers[place]).isModuleBody())
            return;
        Vector<EnvironmentJudgedInEnvironmentsHandedBack, 2> judged;
        for (ValueID environment : facts.summaries().at(makers[place]).environmentsMade) {
            BlockID making = index->blockOf(environment);
            if (!index->loopOf(making) && !index->block(making).hasHandler())
                judged.append(EnvironmentJudgedInEnvironmentsHandedBack { environment });
        }
        if (judged.isEmpty())
            return;
        std::ranges::sort(judged, { }, &EnvironmentJudgedInEnvironmentsHandedBack::environment);
        BodyOfMakerInEnvironmentsHandedBack body { program, facts, makers[place], *index, numberOfFunctionsThatCapture };
        findMentionsOfEnvironmentsJudged(body, judged.mutableSpan());
        for (EnvironmentJudgedInEnvironmentsHandedBack& environment : judged) {
            bool isHandedBack = someWayKeepsNothing(body, environment);
            bool isTakenByClosuresMadeForCallsAlone = closuresMadeForCallsAloneTake(body, environment);
            if (isHandedBack || isTakenByClosuresMadeForCallsAlone)
                found[place].append({ index->value(environment.environment).as<Opcode::NewObject>().layout(), isHandedBack, isTakenByClosuresMadeForCallsAlone });
        }
    });

    BitVector takenOrNothing = findEnvironmentsThatPositionsTakeOrNothing(program);
    EnvironmentsHandedBack environments;
    for (const auto& layouts : found) {
        for (const EnvironmentFoundInEnvironmentsHandedBack& environment : layouts) {
            if (program.at(environment.layout).kind != LayoutKind::Environment || takenOrNothing.get(environment.layout.index()))
                continue;
            if (environment.isHandedBack)
                environments.all.append(environment.layout);
            if (environment.isTakenByClosuresMadeForCallsAlone)
                environments.takenByClosuresMadeForCallsAlone.append(environment.layout);
        }
    }
    return environments;
}

void layEnvironmentsHandedBack(BuildContext& build, std::span<const LayoutID> environments, SlotPlanParts& parts)
{
    for (LayoutID id : environments) {
        PlannedLayout& planned = parts.layouts[id.index()];
        if (planned.storage != LayoutStorage::Environment || planned.slots.isEmpty() || planned.cellsAreHandedBack)
            continue;
        if (const PlacedSlot* holderOfLink = findSlotThatHoldsLinkWhileCellWaits(planned))
            planned.offsetOfLinkToNextCellHandedBack = holderOfLink->offset;
        else {
            uint64_t endOfFields = offsetOfFirstEnvironmentVariable;
            for (const PlacedSlot& slot : planned.slots) {
                if (slot.hasStorage())
                    endOfFields = std::max<uint64_t>(endOfFields, slot.end());
            }
            endOfFields = WTF::roundUpToMultipleOf<sizeof(uint64_t)>(endOfFields);
            uint64_t size = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(endOfFields + sizeof(uint64_t));
            if (size > build.limit(LimitID::MaxFieldOffset))
                continue;
            planned.offsetOfLinkToNextCellHandedBack = static_cast<uint32_t>(endOfFields);
            planned.size = static_cast<uint32_t>(size);
        }
        planned.cellsAreHandedBack = true;
        build.statistics().count(StatisticID::AllocateInFramesChanges);
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
