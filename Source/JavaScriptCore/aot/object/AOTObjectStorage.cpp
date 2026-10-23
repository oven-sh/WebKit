/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTCompileOptions.h"
#include "AOTHeapHooks.h"
#include "AOTInstance.h"
#include "AOTObjectInternal.h"
#include "AOTObjectModel.h"
#include "AOTProgramIdentifiers.h"
#include "AOTProgramStrings.h"
#include "AOTRuntimeValidation.h"
#include "AOTState.h"
#include "HeapAnalyzer.h"
#include "HeapCellInlines.h"
#include "JSCInlines.h"
#include <bit>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo Object::s_info = { "Object"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Object) };

CompiledStructure* Object::createStructure(VM& vm, const CompiledStructure::ObjectParameters& parameters)
{
    unsigned flags = parameters.hasOtherPrototype ? StructureFlags | IsPristine : StructureFlags;
    return CompiledStructure::createForObject(vm, TypeInfo(CompiledObjectType, flags), info(), parameters);
}

Object* Object::create(VM& vm, Structure* structure, const LayoutData& layout, size_t size, uint32_t orderWord)
{
    ASSERT(size >= layout.size);
    Object* object = new (NotNull, allocateCell<Object>(vm, size)) Object(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), size - sizeof(JSCell) });
    bool isBornUnfinished = false;
    for (const FieldData& field : layout.fields()) {
        SlotTraits traits = traitsOf(field.slot.representation());
        if (!field.isOutOfLine() && traits.hasAbsent && traits.absentBits)
            object->clear(field);
        isBornUnfinished |= field.isFilledAtBirth;
    }
    object->setPerCellBit(isBornUnfinished);
    object->at<uint32_t>(offsetOfOrderWord) = orderWord;
    object->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
    object->JSCell::finishCreation(vm);
    return object;
}

Object* Object::createAsAtBirth(VM& vm, Structure* structure, std::span<const uint8_t> bodyAtBirth, bool isBornUnfinished)
{
    Object* object = new (NotNull, allocateCell<Object>(vm, bodyAtBirth.size() + sizeof(JSCell))) Object(vm, structure);
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), bodyAtBirth.size() }, bodyAtBirth);
    object->setPerCellBit(isBornUnfinished);
    object->JSCell::finishCreation(vm);
    return object;
}

Object* Object::createWithBody(VM& vm, Structure* structure, std::span<const uint8_t> body)
{
    Object* object = new (NotNull, allocateCell<Object>(vm, body.size() + sizeof(JSCell))) Object(vm, structure);
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), body.size() }, body);
    object->JSCell::finishCreation(vm);
    object->setPerCellBit(object->orderWord() & orderWordHasFieldOutsideSlot || !object->hasEveryFieldOfBirth());
    return object;
}

Instance& Object::instance() const
{
    return *Instance::of(structure()->realm());
}

State& Object::state() const
{
    return *instance().state;
}

VM& Object::vm() const
{
    return *instance().vm;
}

const FieldData* Object::fieldNamed(State& state, PropertyName name) const
{
    return state.objects->layouts.field(*state.instance.vm, layout(), name.uid());
}

void* Object::slotOf(const FieldData& field) const
{
    if (!field.isOutOfLine()) [[likely]]
        return &at<uint8_t>(field.slot.offset);
    Extension* extension = this->extension();
    return extension ? extension->slotOrNull(field.slot.offset) : nullptr;
}

bool Object::isInSlot(const FieldData& field) const
{
    const void* slot = slotOf(field);
    return slot && !slotHoldsAbsent(field.slot.representation(), slot);
}

JSValue Object::read(const FieldData& field) const
{
    SlotRepresentation representation = field.slot.representation();
    if (representation == SlotRepresentation::DeferredClosure)
        return isInSlot(field) ? const_cast<Object*>(this)->materializeDeferredClosure(vm(), field) : JSValue();
    const void* slot = slotOf(field);
    State& state = this->state();
    JSValue value = slot ? readSlot(representation, slot, *state.strings) : JSValue();
    if (!value && field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field) && !livesInExtraProperties(*state.instance.vm, *this, field))
        return jsUndefined();
    return value;
}

bool Object::tryStore(VM& vm, const FieldData& field, JSValue value)
{
    // A write-once slot that has no absent state, or that defers a closure, holds a placeholder until its only store, which may come by name.
    bool holdsPlaceholder = !field.canLeaveSlot() || field.slot.representation() == SlotRepresentation::DeferredClosure;
    if (field.flags & fieldIsStoredOnce && !holdsPlaceholder)
        return false;
    return tryInitialize(vm, field, value);
}

// The first value of a field of an object that nobody has seen yet: the one store of a field that is stored once.
bool Object::tryInitialize(VM& vm, const FieldData& field, JSValue value)
{
    std::optional<AcceptedValue> accepted = state().objects->layouts.accept(field, value);
    return accepted && store(vm, field, *accepted);
}

bool Object::store(VM& vm, const FieldData& field, AcceptedValue accepted)
{
    return store(vm, field, accepted.value());
}

ALWAYS_INLINE void Object::willStoreMarker(VM& vm, const FieldData& field, JSValue marker)
{
    if (field.keepsStateOfBirth && !state().objects->layouts.fieldAccepts(field, marker)) [[unlikely]]
        crashOnLostStateOfBirth(vm, *this, field, nameOf(ObjectMutation::StoreOfOtherType));
    if (field.holdsRawValueFromBirth)
        setPerCellBit(true);
}

bool Object::store(VM& vm, const FieldData& field, JSValue value)
{
    // A field that is in the key order and has no storage reads as undefined.
    if (value.isUndefined() && field.flags & fieldMayExistWithoutStorage && !slotOf(field)) {
        ASSERT(!layout().keysAreNeverEnumerated);
        willStoreMarker(vm, field, value);
        return true;
    }
    ensureStorageFor(vm, field);
    void* slot = slotOf(field);
    if (!slot)
        return false;
    SlotRepresentation representation = field.slot.representation();
    if (!tryWriteSlot(*state().strings, representation, slot, value))
        return false;
    if (value.isUndefinedOrNull())
        willStoreMarker(vm, field, value);
    if (traitsOf(representation).holdsCell)
        vm.writeBarrier(this, value);
    return true;
}

void Object::ensureStorageFor(VM& vm, const FieldData& field)
{
    if (!field.isOutOfLine())
        return;
    ASSERT(field.slot.offset + widthOf(field.slot.representation()) <= layout().extensionSize());
    ensureExtensionGranules(vm, granuleBitOf(field.slot.offset));
}

// Marking begins and ends while the mutator is stopped, so it does neither before the caller has returned.
void Object::ensureExtensionGranules(VM& vm, uint64_t granuleBits)
{
    ensureExtensionGranules(vm, granuleBits, vm.heap.mutatorShouldBeFenced());
}

// While the collector may be visiting the Extension, granules do not move inside it: a granule that belongs between others goes into a copy, and the old Extension stays as it is.
void Object::ensureExtensionGranules(VM& vm, uint64_t granuleBits, bool collectorMayVisit)
{
    ASSERT(layout().hasExtension());
    Extension* current = extension();
    uint64_t held = current ? current->granuleBits() : 0;
    uint64_t missing = granuleBits & ~held;
    if (!missing || (current && current->tryAppend(missing)))
        return;
    if (current && !collectorMayVisit && current->tryInsert(missing))
        return;
    Extension* replacement = Extension::create(vm, held | missing);
    const uint64_t* from = current ? current->granules() : nullptr;
    uint64_t* to = replacement->granules();
    for (uint64_t remaining = held | missing; remaining; remaining &= remaining - 1, ++to) {
        if (held & remaining & -remaining)
            *to = *from++;
    }
    setExtension(vm, replacement);
}

void Object::setExtension(VM& vm, Extension* extension)
{
    std::optional<SlotData> slot = compiledStructure().extensionSlot();
    RELEASE_ASSERT(slot);
    WTF::storeStoreFence();
    if (slot->representation() == SlotRepresentation::Cell)
        at<Extension*>(slot->offset) = extension;
    else {
        RELEASE_ASSERT(slot->representation() == SlotRepresentation::Cell32);
        at<uint32_t>(slot->offset) = extension ? MarkedBlockCage::encode(cageBaseOf(this), reinterpret_cast<uint8_t*>(extension) - offsetOfEmptyExtensionInCage) : 0;
    }
    vm.writeBarrier(this);
}

void Object::clear(const FieldData& field)
{
    if (void* slot = slotOf(field))
        clearSlot(field.slot.representation(), slot);
}

template<typename Visitor>
void Object::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSCell::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<Object>(cell), visitor);
    // As the visit in AOTHeapCellVisits.h does. Only the collector's own visitor sets marks.
    if constexpr (std::same_as<Visitor, SlotVisitor>) {
        if (uncheckedDowncast<Object>(cell)->layout().hasInnerObjects && !cell->isPreciseAllocation() && cell->markedBlock().hasInnerCells()) [[unlikely]]
            HeapHooks::noteInnerCellsWithoutMark(cell);
    }
    // A type test reads the cell that a slot points to, which races with a running mutator.
    VM& vm = cell->vm();
    if (validatesRuntime(vm) && visitor.mutatorsAreStopped()) [[unlikely]]
        HeapHooks::validateSlotsOf(vm, cell);
}

DEFINE_VISIT_CHILDREN(Object);

size_t Object::estimatedSize(JSCell* cell, VM&)
{
    const LayoutData& layout = uncheckedDowncast<Object>(cell)->layout();
    if (!layout.mayShareCell() || cell->isPreciseAllocation() || !cell->markedBlock().hasInnerCells()) [[likely]]
        return cell->cellSize();
    return cell->markedBlock().isInnerCell(cell) || layout.hasInnerObjects ? layout.size : cell->cellSize();
}

void Object::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    const ProgramIdentifiers& identifiers = *cell->vm().aotState().identifiers;
    uintptr_t cageBase = cageBaseOf(object);
    for (const FieldData& field : object->layout().fields()) {
        SlotRepresentation representation = field.slot.representation();
        if (!traitsOf(representation).holdsCell)
            continue;
        const void* slot = object->slotOf(field);
        JSCell* target = slot ? cellInSlot(representation, slot, cageBase) : nullptr;
        if (!target)
            continue;
        if (UniquedStringImpl* name = identifiers.tryGetIdentifierConcurrently(field.name))
            analyzer.analyzePropertyNameEdge(object, target, name);
    }
}

Extension* Extension::create(VM& vm, uint64_t granuleBits)
{
    RELEASE_ASSERT(granuleBits);
    void* storage = vm.heap.cagedAuxiliarySpace<SubspaceAccess::OnMainThread>()->allocate(vm, extensionCellSizeFor(std::popcount(granuleBits)), nullptr, AllocationFailureMode::Assert);
    static_assert(minExtensionSize == MarkedBlock::atomSize);
    zeroSpan(std::span { static_cast<uint8_t*>(storage), static_cast<HeapCell*>(storage)->cellSize() });
    Extension* extension = static_cast<Extension*>(storage);
    extension->m_granuleBits = granuleBits;
    vm.mutatorFence();
    return extension;
}

bool Extension::tryAppend(uint64_t granuleBits)
{
    ASSERT(m_granuleBits && granuleBits && !(m_granuleBits & granuleBits));
    if (std::countr_zero(granuleBits) < std::bit_width(m_granuleBits))
        return false;
    uint64_t bitsAfter = m_granuleBits | granuleBits;
    if (static_cast<uint32_t>(std::popcount(bitsAfter)) > extensionCapacityFor(std::popcount(m_granuleBits)))
        return false;
    m_granuleBits = bitsAfter;
    return true;
}

bool Extension::tryInsert(uint64_t granuleBits)
{
    ASSERT(m_granuleBits && granuleBits && !(m_granuleBits & granuleBits));
    uint64_t bitsAfter = m_granuleBits | granuleBits;
    uint32_t from = std::popcount(m_granuleBits);
    uint32_t to = std::popcount(bitsAfter);
    if (to > extensionCapacityFor(from))
        return false;
    // From the last granule down, until those that are left stay where they are.
    uint64_t* words = granules();
    for (uint64_t remaining = bitsAfter; from != to;) {
        uint64_t last = 1ull << (std::bit_width(remaining) - 1);
        remaining &= ~last;
        --to;
        words[to] = m_granuleBits & last ? words[--from] : 0;
    }
    m_granuleBits = bitsAfter;
    return true;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
