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
        if (!inputOperationsThatHandOn.contains(made.opcode()))
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

static bool isMadeForCallThatBorrows(const BodyOfMakerInEnvironmentsHandedBack& body, ValueID closure)
{
    const BodyIndex& index = body.index;
    for (ValueID name = closure;;) {
        ValueID taker = index.onlyUsingValue(name);
        if (!taker)
            return false;
        BodyValue user = index.value(taker);
        Opcode opcode = user.opcode();
        if (opcode != Opcode::Call && opcode != Opcode::CallDirect) {
            if (!inputOperationsThatHandOn.contains(opcode) || user.operand(0) != name)
                return false;
            name = taker;
            continue;
        }
        std::span<const ValueID> arguments = user.argumentsOperands();
        size_t position = std::ranges::find(arguments, name) - arguments.begin();
        if (position == arguments.size())
            return false;
        const LinkTables& linked = body.facts.linked();
        if (opcode == Opcode::CallDirect)
            return linked.onlyCallsWhatArrivesAt(user.function(), static_cast<unsigned>(position));
        if (FunctionID callee = linked.calleeAt(body.function, taker))
            return linked.onlyCallsWhatArrivesAt(callee, body.program.at(callee).firstParameterIndex() + static_cast<unsigned>(position));
        return receiverIsWrittenAsTableInEnvironmentsHandedBack(body, user);
    }
}

static bool leavesCellToItsFrames(const BodyOfMakerInEnvironmentsHandedBack& body, const BodyValue& user, uint32_t operandIndex)
{
    switch (user.opcode()) {
    case Opcode::GetEnvironmentField:
    case Opcode::PutEnvironmentField:
        return !operandIndex;
    case Opcode::CallDirect:
        return true;
    case Opcode::NewClosure:
        return isMadeForCallThatBorrows(body, user.id());
    case Opcode::ReleaseHint:
        return true;
    default:
        return false;
    }
}

static bool someWayKeepsNothing(const BodyOfMakerInEnvironmentsHandedBack& body, ValueID environment)
{
    const BodyIndex& index = body.index;
    BlockID making = index.blockOf(environment);
    BitVector keeps(index.numberOfBlocks());
    uint32_t mentionsLeft = index.useCount(environment);
    bool countsEveryMention = mentionsLeft < BodyIndex::maxCountedUses;
    bool isHandedToFunctionEnteredItself = false;
    Vector<FunctionID, 4> functionsOfClosuresLent;
    std::span<const BlockID> below = index.blocksInDominatorOrder().subspan(index.dominatorNumber(making), index.lastDominatorNumberBelow(making) - index.dominatorNumber(making) + 1);
    for (BlockID id : below) {
        if (countsEveryMention && !mentionsLeft)
            break;
        BodyBlock block = index.block(id);
        for (BodyValue user : block.values()) {
            if (user.id().index() <= environment.index())
                continue;
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (operand != environment)
                    return;
                --mentionsLeft;
                bool leavesCell = leavesCellToItsFrames(body, user, operandIndex);
                isHandedToFunctionEnteredItself |= user.opcode() == Opcode::CallDirect;
                if (leavesCell && user.opcode() == Opcode::NewClosure && !functionsOfClosuresLent.contains(user.function()))
                    functionsOfClosuresLent.append(user.function());
                if (!leavesCell)
                    keeps.quickSet(id.index());
            });
        }
        block.terminator().forEachOperand([&](ValueID operand, uint32_t) {
            if (operand == environment)
                keeps.quickSet(id.index());
        });
    }
    LayoutID layout = index.value(environment).as<Opcode::NewObject>().layout();
    bool closuresLentAloneTakeIt = !functionsOfClosuresLent.isEmpty() && functionsOfClosuresLent.size() == body.numberOfFunctionsThatCapture[layout.index()];
    if (keeps.quickGet(making.index()) || (!isHandedToFunctionEnteredItself && !closuresLentAloneTakeIt))
        return false;

    BitVector seen(index.numberOfBlocks());
    Vector<BlockID, 16> worklist { making };
    seen.quickSet(making.index());
    while (!worklist.isEmpty()) {
        BodyBlock block = index.block(worklist.takeLast());
        BodyTerminator end = block.terminator();
        if (end.kind() == TerminatorKind::Return)
            return true;
        for (BodyTarget target : end.targets()) {
            if (!index.strictlyDominates(making, target.block()))
                return true;
            if (!keeps.quickGet(target.block().index()) && !seen.quickSet(target.block().index()))
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

Vector<LayoutID> findEnvironmentsHandedBack(const Program& program, const Facts& facts, BuildContext& build)
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
    Vector<Vector<LayoutID, 2>> found(makers.size());
    forEachIndexInParallel(build.parallelLoops(), makers.size(), [&](unsigned place) {
        const BodyIndex* index = facts.bodies().find(makers[place]);
        if (!index || index->hasIrreducibleFlow())
            return;
        for (ValueID environment : facts.summaries().at(makers[place]).environmentsMade) {
            LayoutID layout = index->value(environment).as<Opcode::NewObject>().layout();
            if (program.at(makers[place]).isModuleBody())
                continue;
            BlockID making = index->blockOf(environment);
            if (index->loopOf(making) || index->block(making).hasHandler())
                continue;
            if (!found[place].contains(layout) && someWayKeepsNothing({ program, facts, makers[place], *index, numberOfFunctionsThatCapture }, environment))
                found[place].append(layout);
        }
    });

    BitVector takenOrNothing = findEnvironmentsThatPositionsTakeOrNothing(program);
    Vector<LayoutID> environments;
    for (const auto& layouts : found) {
        for (LayoutID id : layouts) {
            if (program.at(id).kind == LayoutKind::Environment && !takenOrNothing.get(id.index()))
                environments.append(id);
        }
    }
    return environments;
}

void layEnvironmentsHandedBack(BuildContext& build, std::span<const LayoutID> environments, SlotPlanParts& parts)
{
    for (LayoutID id : environments) {
        PlannedLayout& planned = parts.layouts[id.index()];
        if (planned.storage != LayoutStorage::Environment || planned.slots.isEmpty() || planned.liesOnlyInFrames || planned.cellsAreHandedBack)
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
