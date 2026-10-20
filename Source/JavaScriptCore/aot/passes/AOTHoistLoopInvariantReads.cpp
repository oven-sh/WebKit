/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEffects.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTMemoryLocation.h"
#include "AOTNaturalLoops.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr AbstractHeapSet hoistableHeaps { AbstractHeap::ModuleVariables, AbstractHeap::EnvironmentFields };

namespace {

struct LoopClobbers {
    Vector<MemoryLocation> locations;
    LocationMasks writes;
};

struct HoistableLoop {
    BlockID header;
    BlockID preheader;
    BitVector body;
    LoopClobbers clobbers;
};

} // anonymous namespace

static MemoryAccess variableAccessOf(const Function& function, const Value& value)
{
    MemoryAccess access = memoryAccessOf(function, value);
    if (!access || !hoistableHeaps.contains(access.location.heap()))
        return { };
    return access;
}

static bool isVariableRead(const Function& function, const Value& value)
{
    return variableAccessOf(function, value).kind == MemoryAccess::Kind::Read;
}

static LocationStability stabilityOfRead(const Program& program, const Value& read, const MemoryAccess& access)
{
    LocationStability stability = stabilityOf(program, read, access);
    bool mayPrecedeStore = program.at(read.type).primitives & Primitive::Absent;
    if (stability == LocationStability::ChangedByOwnStores && mayPrecedeStore)
        return LocationStability::Unknown;
    return stability;
}

static bool isClobbered(const LoopClobbers& clobbers, const Program& program, const MemoryLocation& location)
{
    return clobbers.writes.overlaps(program, location);
}

static void addEffects(const EffectsOnEdges& effects, LoopClobbers& clobbers)
{
    Effects any = effects.onAnyEdge();
    if (any.mayRunOtherCode()) {
        clobbers.writes = LocationMasks::top();
        return;
    }
    clobbers.writes.merge(any.writes);
}

static void addClobbers(FunctionPassContext& context, ValueID id, LoopClobbers& clobbers)
{
    MemoryAccess access = variableAccessOf(context.function, context.function.at(id));
    if (access.kind == MemoryAccess::Kind::Write) {
        clobbers.locations.append(access.location);
        return;
    }
    addEffects(context.analyses.effectsOf(id), clobbers);
}

static LoopClobbers findClobbers(FunctionPassContext& context, const BitVector& body)
{
    const Function& function = context.function;
    LoopClobbers clobbers;
    for (size_t index : body) {
        BlockID block(index);
        for (ValueID id : function.at(block).values)
            addClobbers(context, id, clobbers);
        addEffects(context.analyses.effectsOfTerminator(block), clobbers);
    }
    std::ranges::sort(clobbers.locations);
    return clobbers;
}

static std::optional<BlockID> findPreheader(FunctionPassContext& context, const NaturalLoop& loop)
{
    const Function& function = context.function;
    if (function.at(loop.header()).isHandlerBlock())
        return std::nullopt;

    std::optional<Edge> entry = context.analyses.naturalLoops().findSingleEntry(loop);
    if (!entry || function.at(entry->from).terminator.kind != TerminatorKind::Jump)
        return std::nullopt;
    return entry->from;
}

static std::optional<HoistableLoop> describeHoistableLoop(FunctionPassContext& context, const NaturalLoop& loop)
{
    const Function& function = context.function;
    BlockID header = loop.header();
    bool hasRead = std::ranges::any_of(function.at(header).values, [&](ValueID id) {
        return isVariableRead(function, function.at(id));
    });
    if (!hasRead)
        return std::nullopt;

    std::optional<BlockID> preheader = findPreheader(context, loop);
    if (!preheader)
        return std::nullopt;

    BitVector body(function.blocks.size());
    for (unsigned i = 0; i < loop.size(); ++i)
        body.quickSet(loop.at(i).index());
    LoopClobbers clobbers = findClobbers(context, body);
    return HoistableLoop { header, *preheader, WTF::move(body), WTF::move(clobbers) };
}

static bool hasInvariantOperands(const Value& value, const BitVector& body, const Vector<BlockID>& definingBlock)
{
    bool result = true;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        BlockID block = definingBlock[operand.index()];
        result &= block && !body.get(block.index());
    });
    return result;
}

static bool canHoist(FunctionPassContext& context, const HoistableLoop& loop, const LoopClobbers& clobbersBeforeRead, const Vector<BlockID>& definingBlock, const BitVector& keptReads, ValueID id)
{
    const Program& program = context.program;
    const Value& value = context.function.at(id);
    MemoryAccess access = variableAccessOf(context.function, value);
    if (access.kind != MemoryAccess::Kind::Read || keptReads.get(id.index()))
        return false;

    const MemoryLocation& location = access.location;
    if (std::ranges::binary_search(loop.clobbers.locations, location))
        return false;

    LocationStability stability = stabilityOfRead(program, value, access);
    if (stability != LocationStability::UnchangedDuringActivation && isClobbered(clobbersBeforeRead, program, location))
        return false;
    if (stability == LocationStability::Unknown && isClobbered(loop.clobbers, program, location))
        return false;

    return hasInvariantOperands(value, loop.body, definingBlock);
}

static unsigned hoistReads(FunctionPassContext& context, const HoistableLoop& loop, Vector<BlockID>& definingBlock)
{
    Function& function = context.function;
    Block& header = function.at(loop.header);
    Block& preheader = function.at(loop.preheader);

    unsigned numberOfHoistedReads = 0;
    LoopClobbers clobbersBeforeRead;
    BlockValues remaining;
    remaining.reserveInitialCapacity(header.values.size());
    BitVector keptReads = context.build.validates() ? findReadsKeptForValidation(function) : BitVector();
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    for (ValueID id : header.values) {
        if (!canHoist(context, loop, clobbersBeforeRead, definingBlock, keptReads, id)) {
            addClobbers(context, id, clobbersBeforeRead);
            remaining.append(id);
            continue;
        }
        preheader.values.append(id);
        definingBlock[id.index()] = loop.preheader;
        ++numberOfHoistedReads;
        appendKeptReadForValidation(builder, remaining, id);
    }
    if (numberOfHoistedReads)
        header.values = WTF::move(remaining);
    while (definingBlock.size() < function.values.size())
        definingBlock.append(loop.header);
    return numberOfHoistedReads;
}

static Vector<BlockID> computeDefiningBlocks(FunctionPassContext& context)
{
    const Function& function = context.function;
    Vector<BlockID> definingBlock;
    definingBlock.fill(BlockID(), function.values.size());
    for (BlockID blockID : context.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(blockID);
        for (ValueID id : block.parameters)
            definingBlock[id.index()] = blockID;
        for (ValueID id : block.values)
            definingBlock[id.index()] = blockID;
    }
    return definingBlock;
}

static Vector<const NaturalLoop*> findLoopsInnermostFirst(FunctionPassContext& context)
{
    CFG& cfg = context.analyses.cfg();
    NaturalLoops& naturalLoops = context.analyses.naturalLoops();
    Vector<const NaturalLoop*> loops;
    loops.reserveInitialCapacity(naturalLoops.numLoops());
    for (unsigned i = 0; i < naturalLoops.numLoops(); ++i)
        loops.append(&naturalLoops.loop(i));
    std::ranges::sort(loops, [&](const NaturalLoop* a, const NaturalLoop* b) {
        return cfg.reversePostOrderIndex(a->header()) > cfg.reversePostOrderIndex(b->header());
    });
    return loops;
}

PassOutcome hoistLoopInvariantReads(FunctionPassContext& context)
{
    Vector<const NaturalLoop*> loops = findLoopsInnermostFirst(context);
    if (loops.isEmpty())
        return PassOutcome::Unchanged;

    Vector<BlockID> definingBlock = computeDefiningBlocks(context);
    unsigned numberOfHoistedReads = 0;
    for (const NaturalLoop* loop : loops) {
        if (std::optional<HoistableLoop> hoistable = describeHoistableLoop(context, *loop))
            numberOfHoistedReads += hoistReads(context, *hoistable, definingBlock);
    }
    if (!numberOfHoistedReads)
        return PassOutcome::Unchanged;

    context.analyses.didChangeValues();
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
