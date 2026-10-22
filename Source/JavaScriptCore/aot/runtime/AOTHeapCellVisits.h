/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTCompactTable.h"
#include "AOTEnvironment.h"
#include "AOTHeapHooks.h"
#include "AOTObject.h"
#include "AOTSlimClosure.h"

namespace JSC {

template<> struct InlineCellVisit<CellVisitKind::CompiledObject> {
    ALWAYS_INLINE static bool tryVisit(FastMarkAppender& appender, JSCell* cell, Structure* structure)
    {
        appender.appendStructure(structure);
        auto* object = uncheckedDowncast<AOT::Object>(cell);
        const AOT::LayoutData& layout = object->layout();
        if (layout.hasInnerObjects) [[unlikely]]
            visitOuterObject(appender, object, layout);
        else
            AOT::visitReferences(object, layout, layout.hasExtension(), appender);
        return true;
    }

private:
    // Nothing but a reference keeps an inner cell in use, but its bytes last as long as the outer cell, and a word of the stack may name it long after the last reference is gone.
    // The scan of the stack takes whatever has a header for a root. So an inner cell that this object does not name is noted, and loses its header if the marking ends without a mark for it:
    // before anything that it refers to is freed.
    // A reference into the cell of the object itself is a reference to an inner cell. Its mark is found without a division, the outer cell is known to be marked, and the visit takes place at once, not by way of the stack.
    ALWAYS_INLINE static void visitOuterObject(FastMarkAppender& appender, AOT::Object* object, const AOT::LayoutData& layout)
    {
        if (!layout.referencesOffset)
            return;
        // An object of the layout that was made without the room lies elsewhere.
        bool hasRoom = !object->isPreciseAllocation() && object->markedBlock().hasInnerCells();
        uintptr_t start = std::bit_cast<uintptr_t>(object);
        uintptr_t size = hasRoom ? object->markedBlock().cellSizeInHeader() : 0;
        size_t numberOfInnerCellsNamed = 0;
        auto visitCell = [&](JSCell* target) ALWAYS_INLINE_LAMBDA {
            uintptr_t distance = std::bit_cast<uintptr_t>(target) - start;
            if (distance < size && distance) {
                visitInnerCell(appender, target);
                ++numberOfInnerCellsNamed;
            } else
                appender.appendUnbarriered(target);
        };
        const uint32_t* counts = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&layout) + layout.referencesOffset);
        AOT::forEachReference(object, counts, layout.hasExtension(), [&](AOT::Reference reference) ALWAYS_INLINE_LAMBDA {
            switch (reference.kind) {
            case AOT::Reference::Kind::Value:
                if (JSValue value = JSValue::decode(static_cast<EncodedJSValue>(reference.bits)); value && value.isCell())
                    visitCell(value.asCell());
                return;
            case AOT::Reference::Kind::Cell32:
                if (reference.bits >= MarkedBlockCage::minIndex)
                    visitCell(MarkedBlockCage::decode(cageBaseOf(object), static_cast<uint32_t>(reference.bits)));
                return;
            case AOT::Reference::Kind::Auxiliary:
                appender.markAuxiliary(std::bit_cast<const void*>(static_cast<uintptr_t>(reference.bits)));
                return;
            }
        });
        if (hasRoom && numberOfInnerCellsNamed != layout.offsetsOfInnerObjects().size()) [[unlikely]]
            AOT::HeapHooks::noteInnerCellsWithoutMark(object);
        appender.setCurrentCell(object);
    }

    ALWAYS_INLINE static void visitInnerCell(FastMarkAppender& appender, JSCell* inner)
    {
        if (!appender.markInnerCellOfVisitedCell(inner))
            return;
        Structure* structure = inner->structureID().decontaminate().decode();
        appender.setCurrentCell(inner);
        switch (structure->cellVisitKind()) {
        case CellVisitKind::CompiledObject: {
            // An inner object is no outer object.
            appender.appendStructure(structure);
            if (!AOT::HeapHooks::isSabotaged(AOT::HeapHooks::SabotagedPart::InnerObjectIsVisitedWithoutItsFields)) [[likely]]
                AOT::Object::visitFields(uncheckedDowncast<AOT::Object>(inner), appender);
            return;
        }
        case CellVisitKind::Array:
            if (AOT::HeapHooks::isSabotaged(AOT::HeapHooks::SabotagedPart::InnerArrayIsVisitedWithoutItsElements)) [[unlikely]]
                appender.appendStructure(structure);
            else
                InlineCellVisit<CellVisitKind::Array>::tryVisit(appender, inner, structure);
            return;
        default:
            appender.appendMarkedCell(inner);
            return;
        }
    }
};
template<> struct InlineCellVisit<CellVisitKind::CompiledEnvironment> : CellFieldsVisit<AOT::Environment, AOT::Environment> { };
template<> struct InlineCellVisit<CellVisitKind::CompactTable> : CellFieldsVisit<AOT::CompactTable, AOT::CompactTable> { };
template<> struct InlineCellVisit<CellVisitKind::CompiledClosure> : ObjectFieldsVisit<AOT::Closure, JSCallee, JSFunction, AOT::Closure> { };

template<> struct InlineCellVisit<CellVisitKind::SlimClosure> {
    ALWAYS_INLINE static bool tryVisit(FastMarkAppender& appender, JSCell* cell, Structure* structure)
    {
        appender.appendStructure(structure);
        // Not uncheckedDowncast, which asserts the class: the mutator may be reifying this cell right now.
        SUPPRESS_MEMORY_UNSAFE_CAST return AOT::SlimClosure::tryVisitFields(static_cast<AOT::SlimClosure*>(cell), appender);
    }
};

} // namespace JSC

#endif // ENABLE(AOT)
