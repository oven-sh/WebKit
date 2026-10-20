/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosure.h"

#if ENABLE(AOT)

#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCall.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTObjectModel.h"
#include "AOTRuntimeLayoutTable.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTStubEntrypoints.h"
#include "ArgList.h"
#include "AsyncGeneratorPrototype.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "GeneratorPrototype.h"
#include "HeapAnalyzer.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "PropertyNameArray.h"
#include "TopExceptionScope.h"
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

const ClassInfo Closure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Closure) };

static_assert(Closure::offsetOfEntry() == offsetOfClosureEntry && Closure::offsetOfCaptures() == offsetOfClosureCaptures);

Structure* Closure::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool isBound)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CompiledClosureType, isBound ? StructureFlags & ~ImplementsDefaultHasInstance : StructureFlags), info());
}

Closure* Closure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    RELEASE_ASSERT(numberOfReferences <= captures.size() && captures.size() <= maxNumberOfCaptures);
    FunctionTables& tables = FunctionTables::of(vm);
    ClosureFunctionRangeData closureFunctions = programTables().closureFunctions;
    RELEASE_ASSERT(closureFunctions.first <= function.index() && function.index() < closureFunctions.end && tables.words[function.index()]);
    CaptureCounts counts = captureCountsOf(function);
    RELEASE_ASSERT(counts.references == numberOfReferences && counts.captures == captures.size());
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize(captures.size()))) Closure(vm, structure, tables.executableOf(function), tables.words[function.index()]);
    memcpySpan(std::span { reinterpret_cast<uint64_t*>(closure + 1), captures.size() }, captures);
    closure->finishCreation(vm);
    return closure;
}

Closure* Closure::createForCompiledCode(VM& vm, Structure* structure, size_t allocationSize)
{
    RELEASE_ASSERT(allocationSize >= Closure::allocationSize(0));
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize)) Closure(vm, structure, nullptr, 0);
    closure->finishCreation(vm);
    return closure;
}

template<typename Visitor>
void Closure::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    Base::visitChildren(cell, visitor);
    visitFields(uncheckedDowncast<Closure>(cell), visitor);
}

DEFINE_VISIT_CHILDREN(Closure);

void Closure::analyzeHeap(JSCell* cell, HeapAnalyzer& analyzer)
{
    Base::analyzeHeap(cell, analyzer);
    Closure* closure = uncheckedDowncast<Closure>(cell);
    if (!closure->m_entry)
        return;
    for (unsigned i = 0; i < closure->numberOfReferences(); ++i) {
        if (JSValue reference = JSValue::decode(closure->captures()[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

State& Closure::state() const
{
    return *Instance::of(structure()->realm())->state;
}

static bool isBoundFunction(const Closure& closure)
{
    return closure.kind().callable() == CallableKind::Bound;
}

JSC_DEFINE_HOST_FUNCTION(callClassConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* classObject = uncheckedDowncast<Closure>(callFrame->jsCallee());
    String name = classObject->kind().hasInferredName ? String() : writtenFunctionName(classObject->function());
    ASCIILiteral description = FunctionDescription(classObject->function()).isNakedConstructor() ? "constructor"_s : "class constructor"_s;
    return throwVMTypeError(globalObject, scope, makeString("Cannot call a "_s, description, name.isEmpty() ? ""_s : " "_s, name, " without |new|"_s));
}

CallData Closure::getCallData(JSCell* cell)
{
    if (!uncheckedDowncast<Closure>(cell)->kind().isConstructor)
        return CallData::forCompiledCode(closureEntrypoint().taggedPtr(), cell->structure()->realm());
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = callClassConstructor;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

CallData Closure::getConstructData(JSCell* cell)
{
    if (!FunctionTables::canConstruct(uncheckedDowncast<Closure>(cell)->kind()))
        return CallData();
    return CallData::forCompiledCode(constructEntrypoint().taggedPtr(), cell->structure()->realm());
}

JSC_DEFINE_HOST_FUNCTION(callCompiledClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* callee = callFrame->jsCallee();
    return JSValue::encode(callCompiledCode(globalObject->vm(), callee, Closure::getCallData(callee), callFrame->thisValue(), nullptr, ArgList(callFrame)));
}

JSC_DEFINE_HOST_FUNCTION(constructCompiledClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    JSObject* callee = callFrame->jsCallee();
    return JSValue::encode(constructCompiledCode(globalObject->vm(), callee, Closure::getConstructData(callee), ArgList(callFrame), callFrame->newTarget()));
}

bool mayHaveLazyProperty(VM& vm, JSCell* cell, UniquedStringImpl* uid, LookupPosition position)
{
    if (cell->type() == ErrorInstanceType)
        return ErrorInstance::isLazilyMaterialized(vm, uid);
    if (uid == vm.propertyNames->length || uid == vm.propertyNames->name || uid == vm.propertyNames->prototype)
        return true;
    if (isSlimClosureType(cell->type()))
        return position == LookupPosition::PrototypeChain;
    if (cell->type() != CompiledClosureType)
        return true;
    auto* closure = uncheckedDowncast<Closure>(cell);
    if (!closure->kind().isConstructor)
        return false;
    const LayoutData* classLayout = closure->classData();
    return !classLayout || !closure->getDirect(vm, vm.propertyNames->prototype) || closure->state().objects->classes.staticField(*classLayout, uid);
}

static JSC_DECLARE_CUSTOM_GETTER(closurePrototypeProperty);

JSC_DEFINE_CUSTOM_GETTER(closurePrototypeProperty, (JSGlobalObject* globalObject, EncodedJSValue encodedClosure, PropertyName name))
{
    JSCell* cell = JSValue::decode(encodedClosure).asCell();
    if (cell->type() == SlimClosureType) {
        auto* closure = uncheckedDowncast<SlimClosure>(cell);
        return JSValue::encode(closure->canConstruct() ? closure->get(globalObject, name) : jsUndefined());
    }
    JSValue prototype = uncheckedDowncast<Closure>(cell)->prototypeProperty(globalObject->vm());
    return JSValue::encode(prototype ? prototype : jsUndefined());
}

bool closureHandlesPrototypeProperty(JSCell* cell)
{
    return cell->type() == SlimClosureType || cell->type() == CompiledClosureType;
}

GetValueFunc closurePrototypePropertyGetter()
{
    return closurePrototypeProperty;
}

const LayoutData* Closure::classData() const
{
    return kind().isConstructor ? state().objects->classes.classConstructedBy(function()) : nullptr;
}

bool Closure::isLazyClassObject() const
{
    if (!kind().isConstructor || getDirect(vm(), vm().propertyNames->prototype))
        return false;
    const LayoutData* classLayout = classData();
    if (!classLayout)
        return false;
    return classLayout->createsClassObjectOnMutation && (classLayout->createsPrototypeBeforeClassObject || state().objects->classes.classPrototypeIfExists(*classLayout));
}

static NEVER_INLINE void createClassObject(Closure* classObject)
{
    State& state = classObject->state();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(classObject->vm());
    state.objects->classes.createClassPart(state.instance.globalObject, classObject, ClassTable::ClassPart::ClassObject);
    scope.assertNoException();
}

void Closure::adoptBoundFunctionStructure(VM& vm)
{
    ClosureTable& closures = state().objects->closures;
    if (structure() == closures.closureStructureFor(vm, function()))
        setStructure(vm, closures.boundClosureStructure(vm, function()));
}

void Closure::ensureClassObject(VM&)
{
    if (isLazyClassObject())
        createClassObject(this);
}

void Closure::prepareForGenericAccess(VM& vm)
{
    if (isBoundFunction(*this)) [[unlikely]]
        adoptBoundFunctionStructure(vm);
    else
        ensureClassObject(vm);
}

JSValue Closure::prototypeProperty(VM& vm)
{
    if (JSValue stored = getDirect(vm, vm.propertyNames->prototype))
        return stored;
    if (isLazyClassObject())
        return state().objects->classes.classPrototype(vm, *classData());
    ensurePrototypeProperty(vm);
    return getDirect(vm, vm.propertyNames->prototype);
}

bool Closure::ensurePrototypeProperty(VM& vm)
{
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    prepareForGenericAccess(vm);
    if (getDirect(vm, vm.propertyNames->prototype))
        return true;
    if (kind().isConstructor || !FunctionTables::hasPrototypePropertyAtCreation(kind()))
        return false;
    JSGlobalObject* globalObject = state().instance.globalObject;
    JSObject* prototype;
    if (kind().isGenerator)
        prototype = constructEmptyObject(globalObject, kind().isAsync ? static_cast<JSObject*>(globalObject->asyncGeneratorPrototype()) : globalObject->generatorPrototype());
    else {
        prototype = constructEmptyObject(globalObject);
        prototype->putDirect(vm, vm.propertyNames->constructor, this, static_cast<unsigned>(PropertyAttribute::DontEnum));
    }
    putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    return true;
}

void Closure::setPrototypeProperty(VM& vm, JSObject* prototype, unsigned attributes)
{
    RELEASE_ASSERT(!kind().isConstructor);
    prepareForGenericAccess(vm);
    if (FunctionRareData* rareData = this->rareData())
        rareData->clear("Store to prototype property of a function");
    putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void Closure::setNameProperty(VM& vm, JSString* name, unsigned attributes)
{
    prepareForGenericAccess(vm);
    putDirect(vm, vm.propertyNames->name, name, attributes);
    FunctionRareData* rareData = ensureRareData(vm);
    rareData->setHasReifiedName();
    rareData->setHasModifiedNameForBoundOrNonHostFunction();
}

JSObject* boundFunctionTarget(VM& vm, State& state, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const BoundFunctionData& bound = boundFunctionData(function);
    std::optional<unsigned> slot = bound.targetClosureSlot();
    if (!slot)
        return asObject(state.objects->closures.closure(vm, FunctionID(bound.target)));
    RELEASE_ASSERT(*slot < numberOfReferences && numberOfReferences <= captures.size());
    return std::bit_cast<JSObject*>(captures[*slot]);
}

bool Closure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    closure->prepareForGenericAccess(vm);
    if (closure->kind().isConstructor)
        StaticFields::ensureProperties(vm, *closure);
    Base::getOwnSpecialPropertyNames(object, globalObject, names, mode);
    if (mode == DontEnumPropertiesMode::Include && FunctionTables::hasPrototypePropertyAtCreation(closure->kind()) && closure->ensurePrototypeProperty(vm))
        names.add(vm.propertyNames->prototype);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
