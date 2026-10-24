/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTObject.h"

#if ENABLE(AOT)

#include "AOTClosureTable.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "GetterSetter.h"
#include "JSCInlines.h"

namespace JSC { namespace AOT {

void Object::materializeDeferredClosures(VM& vm)
{
    for (auto& field : layout().fields()) {
        if (field.slot.representation() == SlotRepresentation::DeferredClosure && isInSlot(field))
            materializeDeferredClosure(vm, field);
    }
}

// Nothing, the object itself, or the one environment that all closures of the object share. The object keeps that environment in a hidden field, which every allocation fills.
static JSValue valueCapturedByClosureOf(Object& object, const FieldData& field, uintptr_t cageBase)
{
    if (field.flags & fieldDeferredClosureCapturesObject)
        return &object;
    if (!(field.flags & fieldDeferredClosureCapturesEnvironment))
        return { };
    SlotData slot = object.layout().closureEnvironmentSlotOf(field);
    JSCell* environment = cellInSlot(slot.representation(), &object.at<uint8_t>(slot.offset), cageBase);
    RELEASE_ASSERT(environment);
    return environment;
}

// Empty where the property has left the slot. What the program has stored since is in the slot as it is. The slot of an accessor holds its GetterSetter, and the closure is the getter.
JSValue Object::materializeDeferredClosure(VM& vm, const FieldData& field)
{
    ASSERT(field.slot.representation() == SlotRepresentation::DeferredClosure && !field.isOutOfLine());
    // An object whose closures capture nothing may lie in a frame, and that address says nothing about the cage.
    State& state = this->state();
    uintptr_t cageBase = state.instance.cellCageBase;
    uint32_t word = at<uint32_t>(field.slot.offset);
    if (word >= minCell32)
        return MarkedBlockCage::decode(cageBase, word);
    if (word == absentDeferredClosure)
        return { };
    if (word != uncreatedDeferredClosure) {
        ASSERT(word == undefinedCell32 || word == nullCell32);
        return JSValue::decode(word);
    }

    JSObject* closure = state.objects->closures.createClosureOver(vm, layout().deferredClosureOf(field), valueCapturedByClosureOf(*this, field, cageBase));
    JSCell* held = closure;
    if (field.isAccessor()) [[unlikely]]
        held = GetterSetter::create(vm, state.instance.globalObject, closure, nullptr);
    at<uint32_t>(field.slot.offset) = MarkedBlockCage::encode(cageBase, held);
    vm.writeBarrier(this, held);
    return held;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
