/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosureTable.h"

#if ENABLE(AOT)

#include "AOTBuiltinModules.h"
#include "AOTClassTable.h"
#include "AOTClosure.h"
#include "AOTEffectsNet.h"
#include "AOTHeapCacheRegistry.h"
#include "AOTInstance.h"
#include "AOTObject.h"
#include "AOTObjectModel.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AbstractSlotVisitorInlines.h"
#include "AsyncFunctionPrototype.h"
#include "AsyncGeneratorFunctionPrototype.h"
#include "CustomGetterSetter.h"
#include "FastMarkAppender.h"
#include "FunctionPrototype.h"
#include "GeneratorFunctionPrototype.h"
#include "GetterSetter.h"
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
    instance.functionTables.closureWords = shared.words;
    fillWordsOfCompiledMethodsOfArray(instance);
    for (unsigned i = 0; i < FunctionTables::numberOfExecutables; ++i)
        instance.closures.executables[i] = shared.executables[i].get();
    state.caches->add(CacheKind::StrongRoots, "closures"_s, *this);
    m_writtenRegions.setCardsAreWrittenWithoutRecord();
    createStructures(*instance.vm);
}

Structure* ClosureTable::realmClosureStructure(VM& vm, JSGlobalObject* globalObject, unsigned index)
{
    RELEASE_ASSERT(index < numberOfClosureStructures);
    uint64_t& header = Instance::of(globalObject)->closures.closureHeaders[index];
    if (header)
        return std::bit_cast<StructureID>(static_cast<uint32_t>(header)).decode();
    JSObject* prototypes[] = { globalObject->functionPrototype(), globalObject->asyncFunctionPrototype(), globalObject->generatorFunctionPrototype(), globalObject->asyncGeneratorFunctionPrototype(), globalObject->functionPrototype() };
    static_assert(std::size(prototypes) == numberOfClosureStructures && classObjectClosureStructure == 4);
    RELEASE_ASSERT(prototypes[index]);
    Structure* structure = Closure::createStructure(vm, globalObject, prototypes[index]);
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

void ClosureTable::createStructures(VM& vm)
{
    Instance& instance = m_state.instance;
    JSGlobalObject* globalObject = instance.globalObject;
    for (unsigned i = 0; i < numberOfClosureStructures; ++i) {
        m_closureStructures[i] = realmClosureStructure(vm, globalObject, i);
        instance.closures.closureHeaders[i] = cellHeaderFor(m_closureStructures[i]);
    }
    FunctionTables::of(vm).closureStructuresWithoutTable.removeAllMatching([&](const Strong<Structure>& structure) {
        return structure->realm() == globalObject;
    });
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
    FunctionKindData kind = m_tables.kindOfFunction(function.index());
    bool isAsync = kind.isAsync;
    bool hasPrototypeProperty = FunctionTables::hasPrototypePropertyAtCreation(kind);
    Structure*& structure = m_reifiedSlimClosureStructures[isAsync][hasPrototypeProperty];
    if (!structure) {
        if (!m_functionIndexPrivateName)
            m_functionIndexPrivateName = PrivateSymbolImpl::create(*StringImpl::create("function"_span8).ptr());
        JSGlobalObject* globalObject = m_state.instance.globalObject;
        JSObject* prototype = isAsync ? static_cast<JSObject*>(globalObject->asyncFunctionPrototype()) : globalObject->functionPrototype();
        Structure* next = ReifiedSlimClosure::createStructure(vm, globalObject, prototype);
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
        if (hasPrototypeProperty)
            next->setHasAnyKindOfGetterSetterPropertiesWithProtoCheck(false);
        if (hasPrototypeProperty && !m_uncreatedPrototypeProperty) {
            m_uncreatedPrototypeProperty = CustomGetterSetter::create(vm, createPrototypeProperty, nullptr);
            m_state.immortalCells->add(m_uncreatedPrototypeProperty);
        }
        structure = next;
        m_state.immortalCells->add(structure);
    }
    return structure;
}

Structure* ClosureTable::closureStructure(VM&, unsigned index)
{
    RELEASE_ASSERT(index < numberOfClosureStructures);
    return m_closureStructures[index];
}

JSObject* ClosureTable::createClosure(VM& vm, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    FunctionKindData kind = m_tables.kindOfFunction(function.index());
    ASSERT(!kind.isConstructor);
    if (hasSingletonClosure(function))
        return asObject(closure(vm, function));
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
    FunctionKindData kind = m_tables.kindOfFunction(function.index());
    ASSERT(!kind.isConstructor && !m_tables.captureCountsOf(function.index()).captures);
    if (kind.isSlim)
        return SlimClosure::create(vm, slimClosureStructure(vm, function), function);
    return Closure::create(vm, closureStructureFor(vm, function), function);
}

JSObject* ClosureTable::calleeOfModuleBodies(VM& vm, FunctionID body)
{
    ClosureFunctionRangeData closureFunctions = m_tables.closureFunctions;
    RELEASE_ASSERT(closureFunctions.first <= body.index() && body.index() < closureFunctions.end && m_state.instance.functionTables.closureWords[m_tables.closureNumberOf(body.index())]);
    if (!m_calleeOfModuleBodies) [[unlikely]] {
        m_calleeOfModuleBodies = createClosureForOneCall(vm, body);
        m_state.immortalCells->add(m_calleeOfModuleBodies);
    }
    return m_calleeOfModuleBodies;
}

JSCell* ClosureTable::closureIfCreated(FunctionID function) const
{
    uint32_t number = m_tables.cellOfClosureFunction(function.index());
    if (m_state.instance.options().useWideReferences()) {
        JSCell* cell = static_cast<JSCell**>(m_state.instance.closureCells)[number];
        return cell ? cell : m_state.objects->classes.classObjectHeldWhileReached(function);
    }
    uint32_t entry = narrowClosureCells(m_state.instance)[number].get();
    return entry ? MarkedBlockCage::decode(m_state.instance.cellCageBase, entry + 1) : m_state.objects->classes.classObjectHeldWhileReached(function);
}

bool ClosureTable::hasCellFor(FunctionID function) const
{
    return m_tables.cellOfClosureFunction(function.index());
}

bool ClosureTable::hasSingletonClosure(FunctionID function) const
{
    const LayoutData* constructed = m_state.objects->classes.classConstructedBy(function);
    return constructed && m_state.objects->classes.hasOneClassObject(*constructed);
}

void ClosureTable::didCreateClosureFor(FunctionID function, JSCell* cell)
{
    m_state.objects->classes.didRegisterClosure(function);
    if (!m_tables.kindOfFunction(function.index()).isInRuntimeModule)
        m_state.instance.closureTable.cards = m_writtenRegions.cards();
    uint32_t number = m_tables.cellOfClosureFunction(function.index());
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
    FunctionID function = m_state.objects->classes.functionOfOwnClosure(*closure);
    m_state.objects->classes.didRegisterClosure(function);
    if (!closureIfCreated(function) && !m_state.objects->classes.tryHoldClassObjectWhileReached(function, closure))
        didCreateClosureFor(function, closure);
}

void ClosureTable::registerFirstClassObject(Closure* closure, FunctionID constructor, const LayoutData& classLayout)
{
    ASSERT(constructor == m_state.objects->classes.functionOfOwnClosure(*closure) && !closureIfCreated(constructor));
    m_state.objects->classes.didRegisterClosure(constructor);
    if (!m_state.objects->classes.tryHoldClassObjectWhileReached(classLayout, constructor, closure))
        didCreateClosureFor(constructor, closure);
}

void ClosureTable::registerClosureOfMember(FunctionID function, JSCell* closure)
{
    m_state.objects->classes.didRegisterClosure(function);
    ASSERT(hasCellFor(function) && !closureIfCreated(function));
    if (!m_state.objects->classes.tryHoldClassObjectWhileReached(function, closure))
        didCreateClosureFor(function, closure);
}

void ClosureTable::forgetClosures(uint32_t firstCell, uint32_t numberOfCells)
{
    RELEASE_ASSERT(firstCell && static_cast<uint64_t>(firstCell) + numberOfCells <= m_tables.numberOfClosureCells);
    m_state.countWriteForEffectsNet(Typed::CountInEffectsNet::WordsOfStaticsWritten);
    if (m_state.instance.options().useWideReferences()) {
        std::ranges::fill(std::span { static_cast<JSCell**>(m_state.instance.closureCells) + firstCell, numberOfCells }, nullptr);
        return;
    }
    for (RememberedSlot<uint32_t>& cell : std::span { narrowClosureCells(m_state.instance) + firstCell, numberOfCells }) {
        uint32_t entry = cell.get();
        if (!entry)
            continue;
        m_state.immortalCells->add(MarkedBlockCage::decode(m_state.instance.cellCageBase, entry + 1));
        Locker locker { m_state.caches->lock() };
        cell.set(m_writtenRegions, 0);
    }
}

JSCell* ClosureTable::closure(VM& vm, FunctionID function)
{
    if (JSCell* cell = closureIfCreated(function)) {
        RELEASE_ASSERT(!m_state.objects->classes.isMarkOfEvaluationThatDied(cell));
        return cell;
    }
    ASSERT(!m_tables.captureCountsOf(function.index()).captures);
    FunctionKindData kind = m_tables.kindOfFunction(function.index());
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

void Object::materializeDeferredClosures(VM& vm)
{
    for (auto& field : layout().fields()) {
        if (field.slot.representation() == SlotRepresentation::DeferredClosure && isInSlot(field))
            materializeDeferredClosure(vm, field);
    }
}

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

JSValue Object::materializeDeferredClosure(VM& vm, const FieldData& field)
{
    ASSERT(field.slot.representation() == SlotRepresentation::DeferredClosure && !field.isOutOfLine());
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

    JSObject* closure = state.objects->closures.createClosureOver(vm, FunctionID(state.tables.closureFunctionWithNumber(layout().deferredClosureNumberOf(field))), valueCapturedByClosureOf(*this, field, cageBase));
    JSCell* held = closure;
    if (field.isAccessor()) [[unlikely]]
        held = GetterSetter::create(vm, state.instance.globalObject, closure, nullptr);
    at<uint32_t>(field.slot.offset) = MarkedBlockCage::encode(cageBase, held);
    vm.writeBarrier(this, held);
    return held;
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
