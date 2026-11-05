/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPoisonedCell.h"

#if ENABLE(AOT)

#include "AOTObject.h"
#include "AOTSlotAccess.h"
#include "AOTVMState.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>

namespace JSC { namespace AOT {

const ClassInfo PoisonedCell::s_info = { "PoisonedCell"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PoisonedCell) };

namespace {

struct PlaceOfAllocation {
    uint32_t function { 0 };
    uint32_t position { 0 };
};

// No word of the cell says it: a visitor of the collector that still goes by the old Structure would take the numbers for what the slots held.
struct PlacesOfPoisonedCells {
    Lock lock;
    UncheckedKeyHashMap<const JSCell*, PlaceOfAllocation> places WTF_GUARDED_BY_LOCK(lock);
};

} // anonymous namespace

static PlacesOfPoisonedCells& placesOfPoisonedCells()
{
    static LazyNeverDestroyed<PlacesOfPoisonedCells> places;
    static std::once_flag once;
    std::call_once(once, [] {
        places.construct();
    });
    return places;
}

Structure* PoisonedCell::createStructure(VM& vm)
{
    return Structure::create(vm, nullptr, jsNull(), TypeInfo(ObjectType, StructureFlags), info());
}

template<typename Visitor>
void PoisonedCell::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    JSCell::visitChildren(cell, visitor);
}

DEFINE_VISIT_CHILDREN(PoisonedCell);

size_t PoisonedCell::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

bool PoisonedCell::tryPoison(VM& vm, JSCell* cell, uint32_t function, uint32_t position)
{
    uint8_t* bytes = reinterpret_cast<uint8_t*>(cell);
    const ClassInfo* classInfo = cell->classInfo();
    if (classInfo == Object::info()) {
        Object* object = uncheckedDowncast<Object>(cell);
        if (object->isInFrame())
            return false;
        for (const FieldData& field : object->layout().fields()) {
            if (void* slot = object->slotOf(field))
                clearSlot(field.slot.representation(), slot);
        }
    } else if (classInfo != JSArray::info())
        return false;

    VMState& state = vm.aotState();
    if (!state.poisonedCellStructure)
        state.poisonedCellStructure.setWithoutWriteBarrier(createStructure(vm));
    Structure* structure = state.poisonedCellStructure.get();

    {
        PlacesOfPoisonedCells& table = placesOfPoisonedCells();
        Locker locker { table.lock };
        table.places.set(cell, PlaceOfAllocation { function, position });
    }

    *reinterpret_cast<uint64_t*>(bytes + JSObject::butterflyOffset()) = 0;
    WTF::storeStoreFence();
    bytes[JSCell::indexingTypeAndMiscOffset()] = structure->indexingModeIncludingHistory();
    bytes[JSCell::typeInfoTypeOffset()] = structure->typeInfo().type();
    bytes[JSCell::typeInfoFlagsOffset()] = structure->typeInfo().inlineTypeFlags();
    *reinterpret_cast<uint32_t*>(bytes + JSCell::structureIDOffset()) = structure->id().bits();
    return true;
}

void PoisonedCell::stop(const JSCell* cell)
{
    PlaceOfAllocation place;
    {
        PlacesOfPoisonedCells& table = placesOfPoisonedCells();
        Locker locker { table.lock };
        place = table.places.get(cell);
    }
    dataLogLn("AOT: f", place.function, " at ", place.position, ": a cell that the compiler took for dead where its function returned is still held. This is a bug in Bun.");
    CRASH();
}

bool PoisonedCell::getOwnPropertySlot(JSObject* object, JSGlobalObject*, PropertyName, PropertySlot&) { stop(object); }
bool PoisonedCell::getOwnPropertySlotByIndex(JSObject* object, JSGlobalObject*, unsigned, PropertySlot&) { stop(object); }
void PoisonedCell::getOwnPropertyNames(JSObject* object, JSGlobalObject*, PropertyNameArrayBuilder&, DontEnumPropertiesMode) { stop(object); }
bool PoisonedCell::put(JSCell* cell, JSGlobalObject*, PropertyName, JSValue, PutPropertySlot&) { stop(cell); }
bool PoisonedCell::putByIndex(JSCell* cell, JSGlobalObject*, unsigned, JSValue, bool) { stop(cell); }
bool PoisonedCell::deleteProperty(JSCell* cell, JSGlobalObject*, PropertyName, DeletePropertySlot&) { stop(cell); }
bool PoisonedCell::deletePropertyByIndex(JSCell* cell, JSGlobalObject*, unsigned) { stop(cell); }
bool PoisonedCell::defineOwnProperty(JSObject* object, JSGlobalObject*, PropertyName, const PropertyDescriptor&, bool) { stop(object); }
bool PoisonedCell::preventExtensions(JSObject* object, JSGlobalObject*) { stop(object); }
bool PoisonedCell::isExtensible(JSObject* object, JSGlobalObject*) { stop(object); }
bool PoisonedCell::setPrototype(JSObject* object, JSGlobalObject*, JSValue, bool) { stop(object); }
JSValue PoisonedCell::getPrototype(JSObject* object, JSGlobalObject*) { stop(object); }

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
