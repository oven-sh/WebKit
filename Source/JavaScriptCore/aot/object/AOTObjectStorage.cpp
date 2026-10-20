/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTProgramStrings.h"
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
    return CompiledStructure::createForObject(vm, TypeInfo(CompiledObjectType, StructureFlags), info(), parameters);
}

Object* Object::create(VM& vm, Structure* structure, const LayoutData& layout, size_t size, uint32_t orderWord)
{
    ASSERT(size >= layout.size);
    Object* object = new (NotNull, allocateCell<Object>(vm, size)) Object(vm, structure);
    zeroSpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), size - sizeof(JSCell) });
    for (const FieldData& field : layout.fields()) {
        SlotTraits traits = traitsOf(field.slot.representation());
        if (!field.isOutOfLine() && traits.hasAbsent && traits.absentBits)
            object->clear(field);
    }
    object->at<uint32_t>(offsetOfOrderWord) = orderWord;
    object->at<uint32_t>(offsetOfShapeWord) = shapeWordOf(layout.shape);
    object->JSCell::finishCreation(vm);
    return object;
}

Object* Object::createWithBody(VM& vm, Structure* structure, std::span<const uint8_t> body)
{
    Object* object = new (NotNull, allocateCell<Object>(vm, body.size() + sizeof(JSCell))) Object(vm, structure);
    memcpySpan(std::span { reinterpret_cast<uint8_t*>(object) + sizeof(JSCell), body.size() }, body);
    object->JSCell::finishCreation(vm);
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
        return const_cast<Object*>(this)->materializeDeferredClosure(vm(), field);
    const void* slot = slotOf(field);
    if (slot && representation == SlotRepresentation::Int32OrDouble && *static_cast<const int32_t*>(slot) == doubleInExtensionInt32)
        return jsNumber(purifyNaN(*static_cast<const double*>(extension()->slot(layout().overflowOffsetOf(field)))));
    State& state = this->state();
    JSValue value = slot ? readSlot(representation, slot, *state.strings) : JSValue();
    if (!value && field.flags & fieldMayExistWithoutStorage && isInKeyOrder(state, field))
        return jsUndefined();
    return value;
}

bool Object::tryStore(VM& vm, const FieldData& field, JSValue value)
{
    // A slot without an absent state holds a placeholder until its one store, which may come by name. Flow has proven that no second one follows.
    // For a deferred closure that store brings the closure that the first read would have created.
    if (field.flags & fieldIsStoredOnce && field.canLeaveSlot())
        return false;
    return state().objects->layouts.fieldAccepts(field, value) && store(vm, field, value);
}

bool Object::store(VM& vm, const FieldData& field, JSValue value)
{
    ensureStorageFor(vm, field);
    void* slot = slotOf(field);
    if (!slot)
        return false;
    SlotRepresentation representation = field.slot.representation();
    if (tryWriteSlot(*state().strings, representation, slot, value)) {
        if (traitsOf(representation).holdsCell)
            vm.writeBarrier(this, value);
        return true;
    }
    if (representation != SlotRepresentation::Int32OrDouble || !value.isNumber())
        return false;
    storeDoubleInExtension(vm, layout().overflowOffsetOf(field), value.asNumber());
    *static_cast<int32_t*>(slotOf(field)) = doubleInExtensionInt32;
    return true;
}

void Object::storeDoubleInExtension(VM& vm, uint32_t overflowOffset, double number)
{
    ensureExtensionGranules(vm, granuleBitOf(overflowOffset));
    *static_cast<double*>(extension()->slot(overflowOffset)) = number;
}

void Object::ensureStorageFor(VM& vm, const FieldData& field)
{
    if (!field.isOutOfLine())
        return;
    ASSERT(field.slot.offset + widthOf(field.slot.representation()) <= layout().extensionSize());
    ensureExtensionGranules(vm, granuleBitOf(field.slot.offset));
}

// The collector may be visiting the Extension, so granules never move inside it: a granule that belongs between others goes into a copy, and the old Extension stays as it is.
void Object::ensureExtensionGranules(VM& vm, uint64_t granuleBits)
{
    ASSERT(layout().hasExtension());
    Extension* current = extension();
    uint64_t held = current ? current->granuleBits() : 0;
    uint64_t missing = granuleBits & ~held;
    if (!missing || (current && current->tryAppend(missing)))
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
}

DEFINE_VISIT_CHILDREN(Object);

size_t Object::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

void Object::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Object* object = uncheckedDowncast<Object>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    UniquedStringImpl* const* identifiers = object->instance().image.programIdentifiers;
    uintptr_t cageBase = cageBaseOf(object);
    for (const FieldData& field : object->layout().fields()) {
        SlotRepresentation representation = field.slot.representation();
        if (!traitsOf(representation).holdsCell)
            continue;
        const void* slot = object->slotOf(field);
        JSCell* target = slot ? cellInSlot(representation, slot, cageBase) : nullptr;
        UniquedStringImpl* name = identifiers[field.name];
        if (!target || !name)
            continue;
        analyzer.analyzePropertyNameEdge(object, target, name);
    }
}

Extension* Extension::create(VM& vm, uint64_t granuleBits)
{
    RELEASE_ASSERT(granuleBits);
    void* storage = vm.heap.cagedAuxiliarySpace.allocate(vm, extensionCellSizeFor(std::popcount(granuleBits)), nullptr, AllocationFailureMode::Assert);
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

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
