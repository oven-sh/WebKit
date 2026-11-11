/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEffects.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<size_t capacity>
struct DistinctBounds {
    std::array<uint8_t, numberOfOpcodes> rowOf { };
    std::array<Effects, capacity> rows { };
    size_t numberOfRows { 0 };
};

constexpr auto boundsWithRoomForAll = [] {
    DistinctBounds<numberOfOpcodes> result;
    for (Opcode member : allMembersOf<Opcode>) {
        EffectBoundBuilder builder;
        forEachDeclaredEffect(member, builder);
        Effects bound = withDeclaredKindOfThrow(member, builder.effects());
        size_t row = 0;
        while (row < result.numberOfRows && result.rows[row] != bound)
            ++row;
        if (row == result.numberOfRows)
            result.rows[result.numberOfRows++] = bound;
        result.rowOf[static_cast<unsigned>(member)] = static_cast<uint8_t>(row);
    }
    return result;
}();
static_assert(boundsWithRoomForAll.numberOfRows <= 256);

constexpr auto distinctBounds = [] {
    DistinctBounds<boundsWithRoomForAll.numberOfRows> result;
    result.rowOf = boundsWithRoomForAll.rowOf;
    for (; result.numberOfRows < result.rows.size(); ++result.numberOfRows)
        result.rows[result.numberOfRows] = boundsWithRoomForAll.rows[result.numberOfRows];
    return result;
}();

} // anonymous namespace

constinit const std::array<uint8_t, numberOfOpcodes>& rowsOfDeclaredBoundsByOpcode = distinctBounds.rowOf;
constinit const std::span<const Effects> declaredBoundsInRows(distinctBounds.rows);

constinit const OpcodeSet opcodesWithPureBoundInTable = [] {
    OpcodeSet opcodes;
    for (Opcode member : allMembersOf<Opcode>) {
        if (distinctBounds.rows[distinctBounds.rowOf[static_cast<unsigned>(member)]].isPure())
            opcodes.add(member);
    }
    return opcodes;
}();

constinit const OpcodeSet opcodesWithSuspendingBoundInTable = [] {
    OpcodeSet opcodes;
    for (Opcode member : allMembersOf<Opcode>) {
        if (distinctBounds.rows[distinctBounds.rowOf[static_cast<unsigned>(member)]].maySuspend)
            opcodes.add(member);
    }
    return opcodes;
}();

namespace {

class ExactBoundDetector {
public:
    constexpr void reads(const MemoryLocation& location) { m_boundIsExact &= location.isEntireHeap(); }
    constexpr void writes(const MemoryLocation& location) { m_boundIsExact &= location.isEntireHeap(); }
    constexpr void readsAtPayload(AbstractHeap) { m_boundIsExact = false; }
    constexpr void writesAtPayload(AbstractHeap) { m_boundIsExact = false; }
    constexpr void readsAllHeaps() { }
    constexpr void throws() { }
    constexpr void collects() { }
    constexpr void suspends() { }
    constexpr void terminates() { }
    constexpr void unknownCode() { }
    constexpr void asOverload() { m_boundIsExact = false; }

    constexpr bool boundIsExact() const { return m_boundIsExact; }

private:
    bool m_boundIsExact { true };
};

} // anonymous namespace

constinit const OpcodeSet opcodesWithExactBoundsInTable = [] {
    OpcodeSet result;
    for (Opcode member : allMembersOf<Opcode>) {
        ExactBoundDetector detector;
        forEachDeclaredEffect(member, detector);
        if (detector.boundIsExact())
            result.add(member);
    }
    return result;
}();

namespace EffectsAlgebra {

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

constexpr Effects withKindOfThrow(Effects effects, ThrowEffect kind)
{
    effects.mayThrow = kind;
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

constexpr ThrowEffect neverThrows = ThrowEffect(false);
constexpr ThrowEffect stackOverflow = ThrowEffect::onlyStackOverflow();
constexpr ThrowEffect lackOfMemory = ThrowEffect::onlyForLackOfMemory();
constexpr ThrowEffect throwsByRule = ThrowEffect(true);

constexpr Effects noEffects = Effects::none();
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
static_assert(masksAt(AbstractHeap::ObjectFields, 0).isEmpty());
static_assert(masksAt(AbstractHeap::OrderWords, 1).maskOf(AbstractHeap::OrderWords) == LocationMasks::entireHeap);


static_assert(Effects() != unknownCode);
static_assert(withSuspension(unknownCode) == Effects());

static_assert(noEffects.isPure());
static_assert(!unknownCode.isPure());
static_assert(!readOf(objectFields).isPure());
static_assert(!withCollection(noEffects).isPure());
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

static_assert(merged(unknownCode, withSuspension(noEffects)) == Effects());
static_assert(merged(readOf(objectFields), withThrow(writeOf(moduleVariables))) == withThrow(merged(writeOf(moduleVariables), readOf(objectFields))));
static_assert(merged(noEffects, allocation) == allocation);

static_assert(Effects().intersectedWith(noEffects) == noEffects);
static_assert(noEffects.intersectedWith(Effects()) == noEffects);
static_assert(Effects().intersectedWith(allocation) == allocation);
static_assert(allocation.intersectedWith(Effects()) == allocation);
static_assert(allocation.intersectedWith(allocation) == allocation);
static_assert(unknownCode.intersectedWith(allocation) == allocation);
static_assert(Effects().intersectedWith(unknownCode) == unknownCode);
static_assert(Effects().intersectedWith(Effects()) == Effects());
static_assert(withThrow(readOf(unionOf(objectFields, shapeWords))).intersectedWith(withCollection(readOf(shapeWords))) == readOf(shapeWords));
static_assert(allocation.intersectedWith(runtimeStateUpdate) == withCollection(noEffects));
static_assert(readOf(objectFields).intersectedWith(writeOf(objectFields)) == noEffects);
static_assert(readOf(fieldsAB).intersectedWith(readOf(fieldB)) == readOf(fieldB));
static_assert(readOf(fieldA).intersectedWith(readOf(fieldB)) == noEffects);
static_assert(writeOf(fieldsAB).intersectedWith(writeOf(fieldB)) == writeOf(fieldB));
static_assert(writeOf(fieldA).intersectedWith(writeOf(fieldB)) == noEffects);
static_assert(writeOf(fieldA).intersectedWith(unknownCode) == writeOf(fieldA));
static_assert(withThrow(noEffects).intersectedWith(unknownCode) == withThrow(noEffects));
static_assert(withCollection(noEffects).intersectedWith(unknownCode) == withCollection(noEffects));
static_assert(withUnknownCall(noEffects).intersectedWith(unknownCode) == withUnknownCall(noEffects));
static_assert(withTermination(noEffects).intersectedWith(unknownCode) == withTermination(noEffects));
static_assert(withSuspension(noEffects).intersectedWith(unknownCode) == noEffects);
static_assert(withSuspension(noEffects).intersectedWith(Effects()) == withSuspension(noEffects));
static_assert(unknownCode.intersectedWith(withKindOfThrow(noEffects, lackOfMemory)) == withKindOfThrow(noEffects, lackOfMemory));
static_assert(withKindOfThrow(noEffects, stackOverflow).intersectedWith(unknownCode) == withKindOfThrow(noEffects, stackOverflow));
static_assert(withThrow(allocation).intersectedWith(withKindOfThrow(allocation, lackOfMemory)).isRemovableWhenUnused());

static_assert(noEffects.isSubsetOf(allocation));
static_assert(allocation.isSubsetOf(unknownCode));
static_assert(!unknownCode.isSubsetOf(allocation));
static_assert(!withThrow(allocation).isSubsetOf(allocation));
static_assert(neverThrows.isSubsetOf(stackOverflow) && stackOverflow.isSubsetOf(lackOfMemory) && lackOfMemory.isSubsetOf(throwsByRule));
static_assert(!throwsByRule.isSubsetOf(lackOfMemory) && !lackOfMemory.isSubsetOf(stackOverflow) && !stackOverflow.isSubsetOf(neverThrows));
static_assert(!withThrow(allocation).isSubsetOf(withKindOfThrow(allocation, lackOfMemory)));
static_assert(writeOf(fieldA).isSubsetOf(writeOf(fieldsAB)));
static_assert(!writeOf(fieldsAB).isSubsetOf(writeOf(fieldA)));
static_assert(!writeOf(fieldA).isSubsetOf(readOf(fieldA)));
static_assert(unknownCode.isSubsetOf(Effects()));
static_assert(!Effects().isSubsetOf(unknownCode));

static_assert(!withTermination(noEffects).isRemovableWhenUnused());
static_assert(!withTermination(noEffects).alwaysCompletes());
static_assert(withThrow(noEffects).alwaysCompletes());
static_assert(!unknownCode.alwaysCompletes());
static_assert(unknownCode.mayBeTerminated);

static_assert(EffectTraits(noEffects).isPure());
static_assert(!EffectTraits(readOf(objectFields)).isPure() && EffectTraits(readOf(objectFields)).isUnobservable());
static_assert(EffectTraits(allocation).isRemovableWhenUnused() && !EffectTraits(allocation).mayWriteObservableState() && EffectTraits(allocation).mayTouchMemory());
static_assert(!EffectTraits(writeOf(objectFields)).isRemovableWhenUnused());
static_assert(!EffectTraits(withThrow(allocation)).isRemovableWhenUnused());
static_assert(EffectTraits(withThrow(noEffects)).alwaysCompletes() && EffectTraits(withThrow(noEffects)).mayExitSideways());
static_assert(!EffectTraits(withTermination(noEffects)).alwaysCompletes());
static_assert(EffectTraits(withSuspension(noEffects)).mayRunOtherCode() && EffectTraits(withUnknownCall(noEffects)).mayRunOtherCode());
static_assert(EffectTraits(writeOf(objectFields)).mayWriteObservableState() && !EffectTraits(writeOf(objectFields)).mayWriteOutsideObjects());
static_assert(EffectTraits(writeOf(moduleVariables)).mayWriteOutsideObjects() && EffectTraits(unknownCode).mayWriteOutsideObjects());
static_assert(!EffectTraits(allocation).mayWriteOutsideObjects() && !EffectTraits(runtimeStateUpdate).mayWriteOutsideObjects());
static_assert(EffectTraits::fromBits(EffectTraits(unknownCode).bits()) == EffectTraits(unknownCode));

} // namespace EffectsAlgebra

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
