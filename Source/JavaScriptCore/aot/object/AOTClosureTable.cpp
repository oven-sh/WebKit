/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosureTable.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTImmortalCells.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTProgramStatics.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "AsyncFunctionPrototype.h"
#include "AsyncGeneratorFunctionPrototype.h"
#include "CustomGetterSetter.h"
#include "FastMarkAppender.h"
#include "FunctionPrototype.h"
#include "GeneratorFunctionPrototype.h"
#include "JSCInlines.h"
#include "MarkedBlockCage.h"
#include "SlotVisitorInlines.h"
#include "TopExceptionScope.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {


static std::span<const uint8_t> allocateClosureCells(State& state)
{
    Instance& instance = state.instance;
    size_t numberOfCells = state.tables.numberOfClosureCells;
    if (instance.options().useWideReferences()) {
        instance.closureCells = state.tableArena.allocate<uint64_t>(numberOfCells);
        return { static_cast<const uint8_t*>(instance.closureCells), numberOfCells * sizeof(uint64_t) };
    }
    instance.closureCells = state.tableArena.allocate<RememberedSlot<uint32_t>>(numberOfCells);
    return { static_cast<const uint8_t*>(instance.closureCells), numberOfCells * sizeof(uint32_t) };
}

// Only a function that can have a closure in the table has a cell. Zero stands for none: nothing is ever stored in cell 0, so a read of it finds no closure.
static uint32_t cellNumberOf(const ProgramTables& tables, FunctionID function)
{
    ClosureFunctionRangeData range = tables.closureFunctions;
    return function.index() >= range.first && function.index() < range.end ? tables.closureCellNumbers()[function.index() - range.first] : 0;
}

static RememberedSlot<uint32_t>* narrowClosureCells(const Instance& instance)
{
    ASSERT(!instance.options().useWideReferences());
    return static_cast<RememberedSlot<uint32_t>*>(instance.closureCells);
}

ClosureTable::ClosureTable(State& state)
    : m_state(state)
    , m_tables(state.tables)
    , m_writtenRegions(allocateClosureCells(state), regionShift)
{
    Instance& instance = state.instance;
    FunctionTables& shared = FunctionTables::of(*instance.vm);
    instance.closures.closureWords = shared.words;
    for (unsigned i = 0; i < FunctionTables::numberOfExecutables; ++i)
        instance.closures.executables[i] = shared.executables[i].get();
    state.caches->add(CacheKind::StrongRoots, "closures"_s, *this);
    ensureStructures(*instance.vm);
}

Structure* ClosureTable::realmClosureStructure(VM& vm, JSGlobalObject* globalObject, unsigned index)
{
    RELEASE_ASSERT(index < numberOfClosureStructures);
    uint64_t& header = Instance::of(globalObject)->closures.closureHeaders[index];
    if (header)
        return std::bit_cast<StructureID>(static_cast<uint32_t>(header)).decode();
    JSObject* prototype = nullptr;
    switch (index) {
    case 0:
        prototype = globalObject->functionPrototype();
        break;
    case 1:
        prototype = globalObject->asyncFunctionPrototype();
        break;
    case 2:
        prototype = globalObject->generatorFunctionPrototype();
        break;
    case 3:
        prototype = globalObject->asyncGeneratorFunctionPrototype();
        break;
    case classObjectClosureStructure:
        prototype = globalObject->functionPrototype();
        break;
    }
    RELEASE_ASSERT(prototype);
    Structure* structure = Closure::createStructure(vm, globalObject, prototype);
    header = cellHeaderFor(structure);
    FunctionTables::of(vm).closureStructuresWithoutTable.append(Strong<Structure>(vm, structure));
    return structure;
}

void ClosureTable::setAsyncBoundFunctionStructure(Structure* structure)
{
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    if (m_asyncBoundFunctionStructure || structure->realm() != globalObject || structure->storedPrototype() != JSValue(globalObject->asyncFunctionPrototype()))
        return;
    m_state.immortalCells->add(structure);
    m_asyncBoundFunctionStructure = structure;
}

void ClosureTable::didGivePrototypePropertyTo(VM& vm, JSObject* closure)
{
    Structure* structure = closure->structure();
    JSGlobalObject* globalObject = m_state.instance.globalObject;
    if (structure->isDictionary() || !structure->isStructureExtensible() || structure->hasPolyProto() || structure->realm() != globalObject || structure->storedPrototype() != JSValue(globalObject->functionPrototype()))
        return;
    unsigned numberOfOtherProperties = 0;
    structure->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
        numberOfOtherProperties += entry.key() != vm.propertyNames->prototype.impl();
        return true;
    });
    if (numberOfOtherProperties)
        return;
    uint32_t id = std::bit_cast<uint32_t>(structure->id());
    for (uint32_t& known : m_state.instance.closures.structuresWithPrototypeProperty) {
        if (known == id)
            return;
        if (known)
            continue;
        m_state.immortalCells->add(structure);
        known = id;
        return;
    }
}

void ClosureTable::ensureStructures(VM& vm)
{
    if (m_closureStructures[0])
        return;
    Instance& instance = m_state.instance;
    for (unsigned i = 0; i < numberOfClosureStructures; ++i) {
        m_closureStructures[i] = realmClosureStructure(vm, instance.globalObject, i);
        instance.closures.closureHeaders[i] = cellHeaderFor(m_closureStructures[i]);
    }
    FunctionTables::of(vm).closureStructuresWithoutTable.removeAllMatching([&](const Strong<Structure>& structure) {
        return structure->realm() == instance.globalObject;
    });
    createSlimClosureStructures(vm);
}

Structure* ClosureTable::slimClosureStructure(VM& vm, FunctionID function)
{
    ensureStructures(vm);
    return m_slimClosureStructures[functionKindData(function).slimClosureKind()];
}

void ClosureTable::createSlimClosureStructures(VM& vm)
{
    Instance& instance = m_state.instance;
    JSGlobalObject* globalObject = instance.globalObject;
    JSObject* prototypes[] = { globalObject->functionPrototype(), globalObject->asyncFunctionPrototype() };
    static_assert(std::size(prototypes) == numberOfSlimClosureKinds);
    for (unsigned i = 0; i < numberOfSlimClosureKinds; ++i) {
        m_slimClosureStructures[i] = SlimClosure::createStructure(vm, globalObject, prototypes[i]);
        m_state.immortalCells->add(m_slimClosureStructures[i]);
    }
    for (unsigned i = 0; i < numberOfSlimClosureKinds; ++i)
        instance.slimClosureHeaders[i] = cellHeaderFor(m_slimClosureStructures[i]);
    CompleteSubspace& closureSpace = *vm.heap.aotClosureSpace<SubspaceAccess::OnMainThread>();
    for (unsigned i = 0; i < ClosureWords::numberOfSlimClosureAllocators; ++i)
        instance.closures.slimClosureAllocators[i] = closureSpace.allocatorFor(MarkedSpace::sizeStep * (i + 1), AllocatorForMode::EnsureAllocator).localAllocator();
}

Structure* ClosureTable::reifiedSlimClosureStructure(VM& vm, FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    bool isAsync = kind.isAsync;
    bool isBound = kind.callable() == CallableKind::Bound;
    bool hasPrototypeProperty = FunctionTables::hasPrototypePropertyAtCreation(kind);
    Structure*& structure = m_reifiedSlimClosureStructures[isAsync][isBound ? 2 : hasPrototypeProperty];
    if (!structure) {
        if (!m_functionIndexPrivateName)
            m_functionIndexPrivateName = PrivateSymbolImpl::create(*StringImpl::create("function"_span8).ptr());
        JSGlobalObject* globalObject = m_state.instance.globalObject;
        JSObject* prototype = isAsync ? static_cast<JSObject*>(globalObject->asyncFunctionPrototype()) : globalObject->functionPrototype();
        Structure* next = ReifiedSlimClosure::createStructure(vm, globalObject, prototype, isBound ? ReifiedSlimClosure::Variant::Bound : ReifiedSlimClosure::Variant::Ordinary);
        std::pair<PropertyName, unsigned> properties[] = {
            { PropertyName(m_functionIndexPrivateName.get()), PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum | PropertyAttribute::DontDelete },
            { vm.propertyNames->length, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum },
            { vm.propertyNames->name, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum },
            { vm.propertyNames->prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::CustomValue },
        };
        for (unsigned i = 0; i < 3u + hasPrototypeProperty; ++i) {
            PropertyOffset offset;
            next = Structure::addPropertyTransition(vm, next, properties[i].first, properties[i].second, offset);
            RELEASE_ASSERT(offset == static_cast<PropertyOffset>(firstOutOfLineOffset + i));
        }
        if (hasPrototypeProperty) {
            next->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(false);
            if (!m_uncreatedPrototypeProperty) {
                m_uncreatedPrototypeProperty = CustomGetterSetter::create(vm, ReifiedSlimClosure::prototypePropertyCreator(), nullptr);
                m_state.immortalCells->add(m_uncreatedPrototypeProperty);
            }
        }
        structure = next;
        m_state.immortalCells->add(structure);
    }
    return structure;
}

Structure* ClosureTable::boundClosureStructure(VM& vm, FunctionID function)
{
    bool isAsync = functionKindData(function).isAsync;
    Structure*& structure = m_boundClosureStructures[isAsync];
    if (!structure) {
        JSGlobalObject* globalObject = m_state.instance.globalObject;
        structure = Closure::createStructure(vm, globalObject, isAsync ? static_cast<JSObject*>(globalObject->asyncFunctionPrototype()) : globalObject->functionPrototype(), true);
        m_state.immortalCells->add(structure);
    }
    return structure;
}

Structure* ClosureTable::closureStructure(VM& vm, unsigned index)
{
    RELEASE_ASSERT(index < numberOfClosureStructures);
    ensureStructures(vm);
    return m_closureStructures[index];
}

JSObject* ClosureTable::createClosure(VM& vm, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    FunctionKindData kind = functionKindData(function);
    ASSERT(!kind.isConstructor);
    if (hasSingletonClosure(function))
        return asObject(closure(vm, function));
    if (!kind.isInRuntimeModule)
        m_state.statics->ensureProgramStaticsInitialized();
    if (kind.isSlim)
        return SlimClosure::create(vm, slimClosureStructure(vm, function), function, captures, numberOfReferences);
    return Closure::create(vm, closureStructureFor(vm, function), function, captures, numberOfReferences);
}

JSObject* ClosureTable::createClosureOver(VM& vm, FunctionID function, JSValue captured)
{
    if (captured.isEmpty())
        return createClosure(vm, function, { }, 0);
    uint64_t capture = JSValue::encode(captured);
    return createClosure(vm, function, std::span { &capture, 1 }, 1);
}

JSObject* ClosureTable::createClosureForOneCall(VM& vm, FunctionID function)
{
    FunctionKindData kind = functionKindData(function);
    ASSERT(!kind.isConstructor && !captureCountsOf(function).captures);
    if (!kind.isInRuntimeModule)
        m_state.statics->ensureProgramStaticsInitialized();
    if (kind.isSlim)
        return SlimClosure::create(vm, slimClosureStructure(vm, function), function);
    return Closure::create(vm, closureStructureFor(vm, function), function);
}

JSObject* ClosureTable::calleeOfModuleBodies(VM& vm, FunctionID body)
{
    ClosureFunctionRangeData closureFunctions = m_tables.closureFunctions;
    RELEASE_ASSERT(closureFunctions.first <= body.index() && body.index() < closureFunctions.end && m_state.instance.closures.closureWords[body.index()]);
    if (!m_calleeOfModuleBodies) [[unlikely]] {
        m_calleeOfModuleBodies = createClosureForOneCall(vm, body);
        m_state.immortalCells->add(m_calleeOfModuleBodies);
    }
    m_state.statics->ensureProgramStaticsInitialized();
    return m_calleeOfModuleBodies;
}

JSCell* ClosureTable::closureIfCreated(FunctionID function) const
{
    uint32_t number = cellNumberOf(m_tables, function);
    if (m_state.instance.options().useWideReferences())
        return static_cast<JSCell**>(m_state.instance.closureCells)[number];
    uint32_t entry = narrowClosureCells(m_state.instance)[number].get();
    return entry ? MarkedBlockCage::decode(m_state.instance.cellCageBase, entry + 1) : nullptr;
}

bool ClosureTable::hasCellFor(FunctionID function) const
{
    return cellNumberOf(m_tables, function);
}

bool ClosureTable::hasSingletonClosure(FunctionID function) const
{
    const LayoutData* constructed = m_state.objects->classes.classConstructedBy(function);
    return constructed && ClassTable::hasOneClassObject(*constructed);
}

void ClosureTable::didCreateClosureFor(FunctionID function, JSCell* cell)
{
    if (!functionKindData(function).isInRuntimeModule)
        m_state.statics->ensureProgramStaticsInitialized();
    uint32_t number = cellNumberOf(m_tables, function);
    RELEASE_ASSERT(number);
    if (m_state.instance.options().useWideReferences()) {
        static_cast<JSCell**>(m_state.instance.closureCells)[number] = cell;
        m_state.immortalCells->add(cell);
        return;
    }
    Locker locker { m_state.caches->lock() };
    narrowClosureCells(m_state.instance)[number].set(m_writtenRegions, MarkedBlockCage::encode(m_state.instance.cellCageBase, cell) - 1);
}

void ClosureTable::registerClassObject(Closure* closure)
{
    FunctionID function = closure->function();
    if (!closureIfCreated(function))
        didCreateClosureFor(function, closure);
}

void ClosureTable::registerClosureOfMember(FunctionID function, JSCell* closure)
{
    ASSERT(hasCellFor(function) && !closureIfCreated(function));
    didCreateClosureFor(function, closure);
}

void ClosureTable::forgetClosure(FunctionID function)
{
    uint32_t number = cellNumberOf(m_tables, function);
    if (m_state.instance.options().useWideReferences()) {
        static_cast<JSCell**>(m_state.instance.closureCells)[number] = nullptr;
        return;
    }
    Locker locker { m_state.caches->lock() };
    narrowClosureCells(m_state.instance)[number].clear();
}

JSCell* ClosureTable::closure(VM& vm, FunctionID function)
{
    if (JSCell* cell = closureIfCreated(function))
        return cell;
    ASSERT(!captureCountsOf(function).captures);
    FunctionKindData kind = functionKindData(function);
    if (kind.isSlim) {
        JSCell* cell = SlimClosure::create(vm, slimClosureStructure(vm, function), function);
        didCreateClosureFor(function, cell);
        return cell;
    }
    if (const LayoutData* classLayout = kind.isConstructor ? m_state.objects->classes.classConstructedBy(function) : nullptr)
        return m_state.objects->classes.createClassObject(vm, *classLayout);
    Closure* created = Closure::create(vm, closureStructureFor(vm, function), function);
    didCreateClosureFor(function, created);
    return created;
}

template<typename Appender>
ALWAYS_INLINE void ClosureTable::visitRegion(Appender& appender, size_t begin, size_t end)
{
    uintptr_t cageBase = m_state.instance.cellCageBase;
    std::span entries { narrowClosureCells(m_state.instance), static_cast<size_t>(m_tables.numberOfClosureCells) };
    RememberedSlotSet::forEachEntryInRegion(entries, begin, end, [&](RememberedSlot<uint32_t>& entry) ALWAYS_INLINE_LAMBDA {
        if (uint32_t biasedIndex = entry.get())
            appender.appendUnbarriered(MarkedBlockCage::decode(cageBase, biasedIndex + 1));
    });
}

template<typename Visitor>
void ClosureTable::visitRoots(Visitor& visitor)
{
    for (Structure* structure : m_closureStructures) {
        if (structure)
            visitor.appendUnbarriered(structure);
    }
    if (m_state.instance.options().useWideReferences())
        return;
    bool isDeferred = m_writtenRegions.tryDeferFullVisit(visitor, [](SlotVisitor& visitor, void* owner, size_t begin, size_t end) {
        appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
            static_cast<ClosureTable*>(owner)->visitRegion(appender, begin, end);
        });
    }, this);
    if (isDeferred)
        return;
    appendRoots(visitor, [&](auto& appender) ALWAYS_INLINE_LAMBDA {
        m_writtenRegions.forEachRegionToVisit(visitor, [&](size_t begin, size_t end) ALWAYS_INLINE_LAMBDA {
            visitRegion(appender, begin, end);
        });
    });
}

template void ClosureTable::visitRoots(AbstractSlotVisitor&);
template void ClosureTable::visitRoots(SlotVisitor&);

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
