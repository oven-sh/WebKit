/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPoisonedCell.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTEndOfThread.h"
#include "AOTEnvironment.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTObject.h"
#include "AOTObjectWords.h"
#include "AOTRuntimeCheckFailure.h"
#include "AOTSlimClosure.h"
#include "AOTSlotAccess.h"
#include "AOTVMState.h"
#include "GCMemoryOperations.h"
#include "JSArray.h"
#include "JSCInlines.h"
#include <wtf/Atomics.h>
#include <wtf/DataLog.h>
#include <wtf/TZoneMallocInlines.h>

namespace JSC { namespace AOT {

const ClassInfo PoisonedCell::s_info = { "PoisonedCell"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(PoisonedCell) };

WTF_MAKE_TZONE_ALLOCATED_IMPL(PoisonedCellSites);

PoisonedCellSites::PoisonedCellSites(CacheRegistry& caches)
{
    caches.add(CacheKind::WeakByKey, "allocation sites of poisoned cells"_s, *this);
}

size_t PoisonedCellSites::removeDeadEntries(VM& vm, CollectionScope)
{
    Sites sitesThatStay;
    for (const Site& site : m_sites) {
        if (vm.heap.isMarked(site.cell.get()))
            sitesThatStay.append(site);
    }
    size_t numberRemoved = m_sites.size() - sitesThatStay.size();
    m_sites = WTF::move(sitesThatStay);
    return numberRemoved;
}

std::optional<uint32_t> PoisonedCellSites::findFunction(const JSCell* cell) const
{
    for (size_t place = m_sites.size(); place--;) {
        if (m_sites[place].cell.get() == cell)
            return m_sites[place].function.get();
    }
    return std::nullopt;
}

namespace {

struct CellLent {
    JSCell* cell;
    const void* frame;
    uint32_t function;
};

} // anonymous namespace

static Vector<CellLent>& cellsLentOnThisThread()
{
    static thread_local Vector<CellLent>* cells;
    if (!cells) {
        cells = new Vector<CellLent>;
        runAtEndOfThread([] {
            delete std::exchange(cells, nullptr);
        });
    }
    return *cells;
}

void PoisonedCell::noteThatCellIsLent(JSCell* cell, const void* frame, uint32_t function)
{
    cellsLentOnThisThread().append({ cell, frame, function });
    ++s_numberOfCellsLent;
}

uint32_t PoisonedCell::poisonCellsLentBelow(VM& vm, const void* frame, bool andByThatFrame)
{
    uint32_t numberPoisoned = 0;
    Vector<CellLent>& lent = cellsLentOnThisThread();
    while (!lent.isEmpty() && (lent.last().frame < frame || (andByThatFrame && lent.last().frame == frame))) {
        CellLent last = lent.last();
        numberPoisoned += tryPoison(vm, last.cell, last.function);
    }
    return numberPoisoned;
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

bool PoisonedCell::tryPoison(VM& vm, JSCell* cell, uint32_t function)
{
    if (s_numberOfCellsLent) {
        s_numberOfCellsLent -= cellsLentOnThisThread().removeLastMatching([&](const CellLent& lent) {
            return lent.cell == cell;
        });
    }
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
    } else if (classInfo == Environment::info() || classInfo == Closure::info() || classInfo == SlimClosure::info())
        gcSafeZeroMemory(reinterpret_cast<uint64_t*>(bytes + sizeof(JSCell)), cell->cellSize() - sizeof(JSCell));
    else if (classInfo != JSArray::info())
        return false;

    VMState& state = vm.aotState();
    if (!state.poisonedCellStructure)
        state.poisonedCellStructure.setWithoutWriteBarrier(createStructure(vm));
    Structure* structure = state.poisonedCellStructure.get();

    if (!state.poisonedCellSites)
        state.poisonedCellSites = makeUnique<PoisonedCellSites>(*state.caches);
    state.poisonedCellSites->append(cell, function);

    *reinterpret_cast<uint64_t*>(bytes + JSObject::butterflyOffset()) = 0;
    constexpr uint64_t cellStateMask = 0xffull << 8 * JSCell::cellStateOffset();
    uint64_t poisonedHeader = cellHeaderFor(structure) & ~cellStateMask;
    reinterpret_cast<Atomic<uint64_t>*>(bytes)->transaction([&](uint64_t& header) {
        header = (header & cellStateMask) | poisonedHeader;
        return true;
    });
    return true;
}

void PoisonedCell::stop(const JSCell* cell)
{
    RuntimeCheckFailure failure("lifetime check failed"_s);
    if (std::optional<uint32_t> function = cell->vm().aotState().poisonedCellSites->findFunction(cell)) {
        failure.inFunction(FunctionID(*function));
        failure.detail("an object that this function allocated was used after the compiler ended its lifetime");
    } else
        failure.detail("an object was used after the compiler ended its lifetime and a collection found it dead");
    failure.report();
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

CallData PoisonedCell::getCallData(JSCell* cell)
{
    if (cell->vm().isCollectorBusyOnCurrentThread())
        return { };
    stop(cell);
}

CallData PoisonedCell::getConstructData(JSCell* cell)
{
    if (cell->vm().isCollectorBusyOnCurrentThread())
        return { };
    stop(cell);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
