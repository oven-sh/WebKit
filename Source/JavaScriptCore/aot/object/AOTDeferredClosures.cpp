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
#include "JSCInlines.h"

namespace JSC { namespace AOT {

void Object::materializeDeferredClosures(VM& vm)
{
    for (auto& field : layout().fields()) {
        if (field.slot.representation() == SlotRepresentation::DeferredClosure)
            materializeDeferredClosure(vm, field);
    }
}

JSValue Object::materializeDeferredClosure(VM& vm, const FieldData& field)
{
    ASSERT(field.slot.representation() == SlotRepresentation::DeferredClosure && !field.isOutOfLine());
    // An object whose closures capture nothing may lie in a frame, and that address says nothing about the cage.
    State& state = this->state();
    uintptr_t cageBase = state.instance.cellCageBase;
    if (uint32_t word = at<uint32_t>(field.slot.offset))
        return MarkedBlockCage::decode(cageBase, word);
    JSValue captured = field.flags & fieldDeferredClosureCapturesObject ? JSValue(this) : JSValue();
    JSObject* closure = state.objects->closures.createClosureOver(vm, layout().deferredClosureOf(field), captured);
    at<uint32_t>(field.slot.offset) = MarkedBlockCage::encode(cageBase, closure);
    vm.writeBarrier(this, closure);
    return closure;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
