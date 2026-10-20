/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffects.h"

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTIR.h"
#include <wtf/BitVector.h>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

void Effects::dump(PrintStream& out) const
{
    if (isPure()) {
        out.print("none"_s);
        return;
    }
    if (withoutSuspension() == unknownCode()) {
        out.print("unknownCode"_s);
        if (maySuspend)
            out.print(" suspends"_s);
        return;
    }
    CommaPrinter separator(" "_s);
    if (!reads.isEmpty())
        out.print(separator, "reads("_s, reads, ")"_s);
    if (!writes.isEmpty())
        out.print(separator, "writes("_s, writes, ")"_s);
    if (mayThrow)
        out.print(separator, "throws"_s);
    if (mayCollect)
        out.print(separator, "collects"_s);
    if (mayCallUnknownCode)
        out.print(separator, "callsUnknownCode"_s);
    if (maySuspend)
        out.print(separator, "suspends"_s);
    if (mayBeTerminated)
        out.print(separator, "terminates"_s);
}

void EffectsOnEdges::dump(PrintStream& out) const
{
    out.print(normal);
    if (*this != onBothEdges(normal))
        out.print(" | on throw: "_s, handler);
}

EffectsOnEdges effectsOf(const EffectContext& context, const Function& function, const Value& value)
{
    return resolveEffects(context, describeEffects(context.program, function, value));
}

EffectsOnEdges effectsOf(const EffectContext& context, const Function& function, const Terminator& terminator)
{
    return resolveEffects(context, describeEffects(context.program, function, terminator));
}

static BitVector mappedEntryParameters(const Function& callee, const Value& call)
{
    unsigned numberOfParameters = callee.numberOfEntryParameters();
    bool isCalledByValue = Value::opcodesWithCallees.contains(call.opcode);
    BitVector mapped(numberOfParameters);
    call.forEachOperand([&](ValueID, uint32_t index) {
        if (call.operandUses(index).containsAny(callingUses))
            return;
        std::optional<uint32_t> parameter = isCalledByValue ? entryParameterOf(callee, call, index) : call.variadicOperandIndex(index);
        if (parameter && *parameter < numberOfParameters)
            mapped.quickSet(*parameter);
    });
    return mapped;
}

static void addMemory(EffectsOnEdges& effects, const OperandMemory& memory)
{
    effects.normal.reads.merge(memory.reads);
    effects.normal.writes.merge(memory.writes);
    if (!effects.mayThrow())
        return;
    effects.handler.reads.merge(memory.reads);
    effects.handler.writes.merge(memory.writes);
}

static EffectsOnEdges calleeEffectsBeyondOperands(const EffectContext& context, FunctionID id, const Value& call)
{
    const Function& callee = context.program.at(id);
    CalleeSummary summary(context, id);
    BitVector mapped = mappedEntryParameters(callee, call);
    EffectsOnEdges effects = summary->elsewhere;
    for (unsigned parameter = 0; parameter < callee.numberOfEntryParameters(); ++parameter) {
        if (!mapped.quickGet(parameter))
            addMemory(effects, summary->parameter(parameter).memory);
    }
    return effects;
}

static EffectsOnEdges callEffectsBeyondOperands(const EffectContext& context, const Function& function, const Value& call, const EffectsOnEdges& effects)
{
    CalleeList callees = calleesOfCall(context.program, call);
    if (!callees.isComplete || !callees.builtins.isEmpty())
        return effects;
    EffectDescription description = describeEffects(context.program, function, call);
    description.functions.clear();
    for (FunctionID callee : callees.functions)
        description.own.merge(calleeEffectsBeyondOperands(context, callee, call));
    return resolveEffects(context, description);
}

static EffectsOnEdges confinedEffectsBeyondOperands(const EffectContext& context, const Function& function, const Value& value)
{
    EffectDescription description = describeEffects(context.program, function, value);
    description.own.normal.writes.remove(perObjectHeaps);
    description.own.handler.writes.remove(perObjectHeaps);
    return resolveEffects(context, description);
}

EffectsOnEdges effectsBeyondOperands(const EffectContext& context, const Function& function, const Value& value, const EffectsOnEdges& effects)
{
    if (effects.onAnyEdge().mayRunOtherCode())
        return effects;
    if (effectRuleOf(value.opcode) == EffectRule::Call)
        return callEffectsBeyondOperands(context, function, value, effects);
    switch (confinementOf(value.opcode)) {
    case OperandConfinement::Unknown:
        return effects;
    case OperandConfinement::WritesOnlyNamedOperands:
        return confinedEffectsBeyondOperands(context, function, value);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

LocationOverlap overlapOfWrites(const Program& program, const Function& function, const Value& writer, const Effects& effectsOfWriter, const MemoryLocation& location)
{
    MemoryAccess access = memoryAccessOf(function, writer);
    switch (access.kind) {
    case MemoryAccess::Kind::NotExact:
        return effectsOfWriter.writes.overlaps(program, location) ? LocationOverlap::May : LocationOverlap::None;
    case MemoryAccess::Kind::Read:
        return LocationOverlap::None;
    case MemoryAccess::Kind::Write:
        return overlapOf(program, access.location, location);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

namespace {

constexpr LocationMasks masksAt(AbstractHeap heap, uint64_t mask)
{
    LocationMasks masks;
    masks.add(heap, mask);
    return masks;
}

constexpr LocationMasks unionOf(LocationMasks target, const LocationMasks& source)
{
    target.merge(source);
    return target;
}

constexpr Effects readOf(const LocationMasks& masks)
{
    Effects effects = Effects::none();
    effects.reads = masks;
    return effects;
}

constexpr Effects writeOf(const LocationMasks& masks)
{
    Effects effects = Effects::none();
    effects.writes = masks;
    return effects;
}

constexpr Effects withThrow(Effects effects)
{
    effects.mayThrow = true;
    return effects;
}

constexpr Effects withCollection(Effects effects)
{
    effects.mayCollect = true;
    return effects;
}

constexpr Effects withUnknownCall(Effects effects)
{
    effects.mayCallUnknownCode = true;
    return effects;
}

constexpr Effects withSuspension(Effects effects)
{
    effects.maySuspend = true;
    return effects;
}

constexpr Effects withTermination(Effects effects)
{
    effects.mayBeTerminated = true;
    return effects;
}

constexpr bool mergeChanges(Effects target, const Effects& source)
{
    return target.merge(source);
}

constexpr Effects merged(Effects target, const Effects& source)
{
    target.merge(source);
    return target;
}

constexpr LocationMasks top = LocationMasks::top();
constexpr LocationMasks objectFields = LocationMasks(AbstractHeapSet { AbstractHeap::ObjectFields });
constexpr LocationMasks shapeWords = LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords });
constexpr LocationMasks moduleVariables = LocationMasks(AbstractHeapSet { AbstractHeap::ModuleVariables });
constexpr LocationMasks allocatorState = LocationMasks(AbstractHeapSet { AbstractHeap::AllocatorState });
constexpr LocationMasks runtimeState = LocationMasks(AbstractHeapSet { AbstractHeap::RuntimeState });

constexpr LocationMasks fieldA = masksAt(AbstractHeap::ObjectFields, 1);
constexpr LocationMasks fieldB = masksAt(AbstractHeap::ObjectFields, 2);
constexpr LocationMasks fieldsAB = masksAt(AbstractHeap::ObjectFields, 3);

constexpr Effects none = Effects::none();
constexpr Effects unknownCode = Effects::unknownCode();
constexpr Effects allocation = withCollection(merged(readOf(allocatorState), writeOf(allocatorState)));
constexpr Effects runtimeStateUpdate = withCollection(merged(readOf(runtimeState), writeOf(runtimeState)));

static_assert(sizeof(Effects) <= 136);
static_assert(std::is_aggregate_v<Effects>);

static_assert(top.isTop());
static_assert(!LocationMasks().isTop());
static_assert(!top.without({ AbstractHeap::ShapeWords }).isTop());
static_assert(LocationMasks().isEmpty());
static_assert(!shapeWords.isEmpty());
static_assert(top.without(allHeaps).isEmpty());
static_assert(!top.without({ AbstractHeap::ShapeWords }).isEmpty());
static_assert(shapeWords.contains(AbstractHeap::ShapeWords));
static_assert(!shapeWords.contains(AbstractHeap::ObjectFields));
static_assert(top.contains(AbstractHeap::ModuleVariables) && top.contains(AbstractHeap::AllocatorState) && top.contains(AbstractHeap::RuntimeState));
static_assert(!top.without({ AbstractHeap::ShapeWords }).contains(AbstractHeap::ShapeWords));
static_assert(shapeWords.isSubsetOf(top));
static_assert(!top.isSubsetOf(shapeWords));
static_assert(LocationMasks().isSubsetOf(LocationMasks()));
static_assert(unionOf(objectFields, shapeWords).overlaps(shapeWords));
static_assert(!objectFields.overlaps(shapeWords));
static_assert(!LocationMasks().overlaps(top));
static_assert(unionOf(objectFields, shapeWords) == LocationMasks(AbstractHeapSet { AbstractHeap::ShapeWords, AbstractHeap::ObjectFields }));
static_assert(unionOf(objectFields, shapeWords) != objectFields);
static_assert(unionOf(objectFields, shapeWords).intersectedWith(unionOf(shapeWords, runtimeState)) == shapeWords);
static_assert(objectFields.intersectedWith(shapeWords).isEmpty());

static_assert(!fieldA.overlaps(fieldB));
static_assert(fieldA.overlaps(fieldsAB));
static_assert(fieldA.overlaps(objectFields));
static_assert(fieldA.isSubsetOf(fieldsAB));
static_assert(!fieldsAB.isSubsetOf(fieldA));
static_assert(fieldsAB.isSubsetOf(objectFields));
static_assert(!objectFields.isSubsetOf(fieldsAB));
static_assert(unionOf(fieldA, fieldB) == fieldsAB);
static_assert(fieldsAB.intersectedWith(fieldB) == fieldB);
static_assert(fieldA.intersectedWith(fieldB).isEmpty());
static_assert(!fieldA.intersectedWith(fieldB).contains(AbstractHeap::ObjectFields));
static_assert(fieldA.without({ AbstractHeap::ObjectFields }).isEmpty());
static_assert(!fieldA.containsEntire(AbstractHeap::ObjectFields));
static_assert(objectFields.containsEntire(AbstractHeap::ObjectFields));
static_assert(!readOf(fieldA).interferesWith(writeOf(fieldB)));
static_assert(readOf(fieldA).interferesWith(writeOf(fieldsAB)));
static_assert(!writeOf(fieldA).interferesWith(writeOf(fieldB)));
static_assert(masksAt(AbstractHeap::ObjectFields, 0).isEmpty());
static_assert(masksAt(AbstractHeap::OrderWords, 1).maskOf(AbstractHeap::OrderWords) == LocationMasks::entireHeap);

static_assert(Effects() != unknownCode);
static_assert(Effects().withoutSuspension() == unknownCode);
static_assert(withSuspension(unknownCode) == Effects());
static_assert(withSuspension(allocation).withoutSuspension() == allocation);

static_assert(none.isPure());
static_assert(!unknownCode.isPure());
static_assert(!readOf(objectFields).isPure());
static_assert(!withCollection(none).isPure());
static_assert(!allocation.isPure());

static_assert(allocation.isRemovableWhenUnused());
static_assert(withCollection(writeOf(allocatorState)).isRemovableWhenUnused());
static_assert(runtimeStateUpdate.isRemovableWhenUnused());
static_assert(merged(allocation, runtimeStateUpdate).isRemovableWhenUnused());
static_assert(!withThrow(allocation).isRemovableWhenUnused());
static_assert(!withSuspension(allocation).isRemovableWhenUnused());
static_assert(!withUnknownCall(allocation).isRemovableWhenUnused());
static_assert(readOf(top).isRemovableWhenUnused());
static_assert(!writeOf(objectFields).isRemovableWhenUnused());
static_assert(!writeOf(unionOf(objectFields, runtimeState)).isRemovableWhenUnused());
static_assert(!writeOf(unionOf(objectFields, allocatorState)).isRemovableWhenUnused());

static_assert(readOf(objectFields).interferesWith(writeOf(objectFields)));
static_assert(writeOf(objectFields).interferesWith(readOf(objectFields)));
static_assert(!readOf(objectFields).interferesWith(writeOf(moduleVariables)));
static_assert(!writeOf(moduleVariables).interferesWith(readOf(objectFields)));
static_assert(writeOf(objectFields).interferesWith(writeOf(objectFields)));
static_assert(!writeOf(objectFields).interferesWith(writeOf(moduleVariables)));
static_assert(!readOf(objectFields).interferesWith(readOf(objectFields)));
static_assert(withThrow(none).interferesWith(withThrow(none)));
static_assert(withThrow(none).interferesWith(withSuspension(none)));
static_assert(withUnknownCall(none).interferesWith(withThrow(none)));
static_assert(!withThrow(none).interferesWith(readOf(objectFields)));
static_assert(!readOf(objectFields).interferesWith(withThrow(none)));
static_assert(withThrow(none).interferesWith(writeOf(objectFields)));
static_assert(writeOf(objectFields).interferesWith(withThrow(none)));
static_assert(writeOf(objectFields).interferesWith(withSuspension(none)));
static_assert(withSuspension(none).interferesWith(readOf(objectFields)));
static_assert(readOf(objectFields).interferesWith(withSuspension(none)));
static_assert(withUnknownCall(none).interferesWith(readOf(objectFields)));
static_assert(readOf(objectFields).interferesWith(withUnknownCall(none)));
static_assert(!withSuspension(none).interferesWith(withCollection(none)));
static_assert(!withUnknownCall(none).interferesWith(none));
static_assert(!withCollection(none).interferesWith(withCollection(none)));
static_assert(!withCollection(none).interferesWith(writeOf(objectFields)));
static_assert(!withCollection(none).interferesWith(withThrow(none)));
static_assert(!none.interferesWith(unknownCode));
static_assert(readOf(objectFields).interferesWith(unknownCode));
static_assert(!allocation.interferesWith(unknownCode));
static_assert(!unknownCode.interferesWith(allocation));
static_assert(!withCollection(writeOf(allocatorState)).interferesWith(unknownCode));
static_assert(!unknownCode.interferesWith(withCollection(writeOf(allocatorState))));
static_assert(!allocation.interferesWith(Effects()));
static_assert(!Effects().interferesWith(allocation));
static_assert(!allocation.interferesWith(allocation));
static_assert(!allocation.interferesWith(withThrow(none)));
static_assert(!withSuspension(none).interferesWith(allocation));
static_assert(!allocation.interferesWith(runtimeStateUpdate));
static_assert(runtimeStateUpdate.interferesWith(unknownCode));
static_assert(unknownCode.interferesWith(runtimeStateUpdate));
static_assert(withCollection(writeOf(runtimeState)).interferesWith(unknownCode));
static_assert(unknownCode.interferesWith(withCollection(writeOf(runtimeState))));
static_assert(runtimeStateUpdate.interferesWith(runtimeStateUpdate));
static_assert(readOf(runtimeState).interferesWith(unknownCode));
static_assert(withThrow(allocation).interferesWith(unknownCode));
static_assert(merged(allocation, writeOf(objectFields)).interferesWith(readOf(objectFields)));
static_assert(readOf(objectFields).interferesWith(merged(allocation, writeOf(objectFields))));
static_assert(merged(allocation, readOf(objectFields)).interferesWith(unknownCode));

static_assert(!mergeChanges(unknownCode, allocation));
static_assert(!mergeChanges(allocation, allocation));
static_assert(!mergeChanges(allocation, none));
static_assert(mergeChanges(allocation, withThrow(none)));
static_assert(mergeChanges(allocation, readOf(objectFields)));
static_assert(mergeChanges(allocation, writeOf(objectFields)));
static_assert(mergeChanges(unknownCode, withSuspension(none)));
static_assert(merged(unknownCode, withSuspension(none)) == Effects());
static_assert(merged(readOf(objectFields), withThrow(writeOf(moduleVariables))) == withThrow(merged(writeOf(moduleVariables), readOf(objectFields))));
static_assert(merged(none, allocation) == allocation);

static_assert(Effects().intersectedWith(none) == none);
static_assert(Effects().intersectedWith(allocation) == allocation);
static_assert(withThrow(readOf(unionOf(objectFields, shapeWords))).intersectedWith(withCollection(readOf(shapeWords))) == readOf(shapeWords));

static_assert(none.isSubsetOf(allocation));
static_assert(allocation.isSubsetOf(unknownCode));
static_assert(!unknownCode.isSubsetOf(allocation));
static_assert(!withThrow(allocation).isSubsetOf(allocation));
static_assert(writeOf(fieldA).isSubsetOf(writeOf(fieldsAB)));
static_assert(!writeOf(fieldsAB).isSubsetOf(writeOf(fieldA)));
static_assert(!writeOf(fieldA).isSubsetOf(readOf(fieldA)));
static_assert(unknownCode.isSubsetOf(Effects()));
static_assert(!Effects().isSubsetOf(unknownCode));

static_assert(!withTermination(none).isRemovableWhenUnused());
static_assert(!withTermination(none).alwaysCompletes());
static_assert(withThrow(none).alwaysCompletes());
static_assert(!unknownCode.alwaysCompletes());
static_assert(withTermination(none).interferesWith(writeOf(objectFields)));
static_assert(unknownCode.mayBeTerminated);

static_assert(EffectsOnEdges::onBothEdges(allocation).handler == none);
static_assert(EffectsOnEdges::onBothEdges(withThrow(allocation)).handler == withThrow(allocation));
static_assert(EffectsOnEdges::none().isSubsetOf(EffectsOnEdges::unknownCode()));
static_assert(EffectsOnEdges::unknownCode().onAnyEdge() == unknownCode);

} // anonymous namespace

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
