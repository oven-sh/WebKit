/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSlimClosure.h"

#if ENABLE(AOT)

#include "AOTClosure.h"
#include "AOTClosureTable.h"
#include "AOTFunctionNames.h"
#include "AOTInstance.h"
#include "AOTObjectModel.h"
#include "AOTState.h"
#include "AOTStubEntrypoints.h"
#include "ButterflyInlines.h"
#include "HeapAnalyzer.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "StructureCreateInlines.h"
#include <wtf/StdLibExtras.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo SlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SlimClosure) };
const ClassInfo ReifiedSlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ReifiedSlimClosure) };

static void analyzeCaptures(JSCell* cell, std::span<const uint64_t> captures, unsigned numberOfReferences, HeapAnalyzer& analyzer)
{
    for (unsigned i = 0; i < numberOfReferences; ++i) {
        if (JSValue reference = JSValue::decode(captures[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

static CallData compiledCallData(JSCell* cell)
{
    return CallData::forCompiledCode(slimClosureEntrypoint().taggedPtr(), cell->structure()->realm());
}

static CallData compiledConstructData(JSCell* cell, bool canConstruct)
{
    if (!canConstruct)
        return CallData();
    return CallData::forCompiledCode(constructEntrypoint().taggedPtr(), cell->structure()->realm());
}

Structure* SlimClosure::createStructure(VM& vm, JSGlobalObject* realm, JSValue prototype)
{
    return Structure::create(vm, realm, prototype, TypeInfo(SlimClosureType, StructureFlags), info(), NonArray, 0);
}

Structure* ReifiedSlimClosure::createStructure(VM& vm, JSGlobalObject* realm, JSValue prototype, Variant variant)
{
    unsigned flags = variant == Variant::Bound ? StructureFlags & ~ImplementsDefaultHasInstance : StructureFlags;
    return Structure::create(vm, realm, prototype, TypeInfo(SlimClosureWithPropertiesType, flags), info(), NonArray, 0);
}

SlimClosure* SlimClosure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    FunctionTables& tables = FunctionTables::of(vm);
    RELEASE_ASSERT(tables.words[function.index()] && functionKindData(function).isSlim);
    CaptureCounts counts = captureCountsOf(function);
    RELEASE_ASSERT(counts.references == numberOfReferences && counts.captures == captures.size());
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize(captures.size()))) SlimClosure(vm, structure, tables.words[function.index()]);
    memcpySpan(std::span { reinterpret_cast<uint64_t*>(closure + 1), captures.size() }, captures);
    closure->finishCreation(vm);
    return closure;
}

SlimClosure* SlimClosure::createForCompiledCode(VM& vm, Structure* structure, size_t allocationSize)
{
    RELEASE_ASSERT(allocationSize >= SlimClosure::allocationSize(0));
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize)) SlimClosure(vm, structure, 0);
    closure->finishCreation(vm);
    return closure;
}

State& SlimClosure::state() const
{
    return State::of(structure()->realm());
}

State& ReifiedSlimClosure::state() const
{
    return State::of(structure()->realm());
}

template<typename Visitor>
void SlimClosure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    if (!tryVisitFields(closure, visitor)) {
        visitor.didRace(cell, "SlimClosure::reify");
        return;
    }
    JSCell::visitChildren(cell, visitor);
}

DEFINE_VISIT_CHILDREN(SlimClosure);

template<typename Visitor>
void ReifiedSlimClosure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(cell);
    Base::visitChildren(cell, visitor);
    visitFields(closure, visitor);
}

DEFINE_VISIT_CHILDREN(ReifiedSlimClosure);

void SlimClosure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(cell);
    JSCell::analyzeHeap(cell, analyzer);
    if (!closure->m_word)
        return;
    analyzeCaptures(cell, closure->captures(), closure->numberOfReferences(), analyzer);
}

void ReifiedSlimClosure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(cell);
    Base::analyzeHeap(cell, analyzer);
    analyzeCaptures(cell, closure->captures(), closure->numberOfReferences(), analyzer);
}

size_t SlimClosure::estimatedSize(JSCell* cell, VM&)
{
    return cell->cellSize();
}

CallData SlimClosure::getCallData(JSCell* cell)
{
    return compiledCallData(cell);
}

CallData ReifiedSlimClosure::getCallData(JSCell* cell)
{
    return compiledCallData(cell);
}

CallData SlimClosure::getConstructData(JSCell* cell)
{
    return compiledConstructData(cell, uncheckedDowncast<SlimClosure>(cell)->canConstruct());
}

CallData ReifiedSlimClosure::getConstructData(JSCell* cell)
{
    return compiledConstructData(cell, uncheckedDowncast<ReifiedSlimClosure>(cell)->canConstruct());
}

ReifiedSlimClosure* SlimClosure::reify(VM& vm, PrototypeObject prototypeObject)
{
    State& state = this->state();
    FunctionID function = this->function();
    Structure* withProperties = state.objects->closures.reifiedSlimClosureStructure(vm, function);
    JSValue values[] = { jsNumber(function.index()), jsNumber(functionLengthOf(function)), initialNamePropertyString(vm, state, function), JSValue() };
    if (FunctionTables::hasPrototypePropertyAtCreation(kind())) {
        values[3] = state.objects->closures.uncreatedPrototypeProperty();
        if (prototypeObject == PrototypeObject::MadeNow) {
            withProperties = Structure::attributeChangeTransition(vm, withProperties, vm.propertyNames->prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
            JSObject* prototype = constructEmptyObject(state.instance.globalObject);
            prototype->putDirect(vm, vm.propertyNames->constructor, this, static_cast<unsigned>(PropertyAttribute::DontEnum));
            values[3] = prototype;
        }
    }
    static_assert(std::size(values) == initialOutOfLineCapacity);
    static_assert(offsetOfWord() == butterflyOffset());
    RELEASE_ASSERT(withProperties->outOfLineCapacity() == std::size(values) && getPrototypeDirect() == withProperties->storedPrototype());
    Butterfly* butterfly = Butterfly::createUninitialized(vm, this, 0, std::size(values), false, 0);
    for (unsigned i = 0; i < std::size(values); ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(firstOutOfLineOffset + i)].setWithoutWriteBarrier(values[i]);
    // JSObject's own setters assert on butterfly(), which is null until the cell's type changes.
    setStructureIDDirectly(structureID().nuke());
    WTF::storeStoreFence();
    m_word = std::bit_cast<uint64_t>(butterfly);
    WTF::storeStoreFence();
    JSCell::setStructure(vm, withProperties);
    vm.writeBarrier(this);
    return uncheckedDowncast<ReifiedSlimClosure>(static_cast<JSObject*>(this));
}

static JSC_DECLARE_CUSTOM_GETTER(createPrototypeProperty);

JSC_DEFINE_CUSTOM_GETTER(createPrototypeProperty, (JSGlobalObject*, EncodedJSValue encodedClosure, PropertyName))
{
    JSObject* closure = asObject(JSValue::decode(encodedClosure));
    VM& vm = closure->vm();
    JSObject* prototype = constructEmptyObject(closure->structure()->realm());
    prototype->putDirect(vm, vm.propertyNames->constructor, closure, static_cast<unsigned>(PropertyAttribute::DontEnum));
    // The closure may have been frozen since.
    unsigned attributes = 0;
    closure->getDirect(vm, vm.propertyNames->prototype, attributes);
    closure->putDirect(vm, vm.propertyNames->prototype, prototype, attributes & ~PropertyAttribute::CustomValue);
    return JSValue::encode(prototype);
}

GetValueFunc ReifiedSlimClosure::prototypePropertyCreator()
{
    return createPrototypeProperty;
}

bool SlimClosure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool ReifiedSlimClosure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool SlimClosure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    VM& vm = globalObject->vm();
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    bool isLengthOrName = name == vm.propertyNames->length || name == vm.propertyNames->name;
    bool isPrototype = name == vm.propertyNames->prototype && FunctionTables::hasPrototypePropertyAtCreation(closure->kind());
    bool isBoundHasInstance = name == vm.propertyNames->hasInstanceSymbol && closure->isBound();
    if (!isLengthOrName && !isPrototype && !isBoundHasInstance)
        return false;
    if (isLengthOrName && !closure->perCellBit()) {
        closure->setPerCellBit(true);
        FunctionID function = closure->function();
        if (name == vm.propertyNames->length)
            slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, jsNumber(functionLengthOf(function)));
        else
            slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, initialNamePropertyString(vm, closure->state(), function));
        return true;
    }
    closure->reify(vm, isPrototype ? PrototypeObject::MadeNow : PrototypeObject::MadeAtFirstRead);
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

bool SlimClosure::getOwnPropertySlotByIndex(JSObject*, JSGlobalObject*, unsigned, PropertySlot&)
{
    return false;
}

void SlimClosure::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    JSObject::getOwnPropertyNames(object, globalObject, names, mode);
}

bool SlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    bool result = ReifiedSlimClosure::put(closure, globalObject, name, value, slot);
    slot.disableCaching();
    return result;
}

bool ReifiedSlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    VM& vm = globalObject->vm();
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(cell);
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didStoreDisplayName(vm, closure);
    if (name == vm.propertyNames->prototype) [[unlikely]]
        slot.disableCaching();
    return result;
}

bool SlimClosure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    return JSObject::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool SlimClosure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    bool isDeleted = JSObject::deleteProperty(cell, globalObject, name, slot);
    slot.disableCaching();
    return isDeleted;
}

bool SlimClosure::deletePropertyByIndex(JSCell*, JSGlobalObject*, unsigned)
{
    return true;
}

bool SlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    ReifiedSlimClosure* closure = uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return ReifiedSlimClosure::defineOwnProperty(closure, globalObject, name, descriptor, shouldThrow);
}

bool ReifiedSlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(object);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didStoreDisplayName(vm, closure);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString()) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        didDefineNameProperty(vm, closure, reported);
    }
    return result;
}

bool SlimClosure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return JSObject::preventExtensions(object, globalObject);
}

bool SlimClosure::isExtensible(JSObject*, JSGlobalObject*)
{
    return true;
}

bool SlimClosure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    if (object->getPrototypeDirect() == prototype)
        return true;
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return JSObject::setPrototype(object, globalObject, prototype, shouldThrow);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
