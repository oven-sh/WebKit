/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTClosure.h"

#if ENABLE(AOT)

#include "AOTImageOfCellScope.h"
#include "AOTCheckedFrameCells.h"
#include "AOTClassTable.h"
#include "AOTClosureTable.h"
#include "AOTCompiledCall.h"
#include "AOTErrorMessages.h"
#include "AOTFunctionDescription.h"
#include "AOTFunctionNames.h"
#include "AOTHeapHooks.h"
#include "AOTInstance.h"
#include "AOTLayoutData.h"
#include "AOTModuleTable.h"
#include "AOTObjectModel.h"
#include "AOTProgramStrings.h"
#include "AOTPropertyWalk.h"
#include "AOTRuntimeValidation.h"
#include "AOTSlimClosure.h"
#include "AOTState.h"
#include "AOTStaticFields.h"
#include "AOTStubEntrypoints.h"
#include "ArgList.h"
#include "AsyncGeneratorPrototype.h"
#include "ButterflyInlines.h"
#include "DeferTermination.h"
#include "Error.h"
#include "ErrorInstance.h"
#include "FunctionPrototype.h"
#include "FunctionRareData.h"
#include "GeneratorPrototype.h"
#include "GetterSetter.h"
#include "HeapAnalyzer.h"
#include "JSCInlines.h"
#include "ObjectConstructor.h"
#include "ObjectConstructorInlines.h"
#include "PropertyDescriptor.h"
#include "PropertyNameArray.h"
#include "StructureCreateInlines.h"
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT {

const ClassInfo Closure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(Closure) };
const ClassInfo SlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(SlimClosure) };
const ClassInfo ReifiedSlimClosure::s_info = { "Function"_s, &Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(ReifiedSlimClosure) };

static ALWAYS_INLINE void validateIsNotInFrame(VM& vm, const JSCell* closure)
{
    if (validatesRuntime(vm) || CheckedFrameCells::isInUse()) [[unlikely]]
        validateFunctionCellIsNotInFrame(vm, closure);
}

static void analyzeCaptures(JSCell* cell, std::span<const uint64_t> captures, unsigned numberOfReferences, HeapAnalyzer& analyzer)
{
    for (unsigned i = 0; i < numberOfReferences; ++i) {
        if (JSValue reference = JSValue::decode(captures[i]); reference && reference.isCell())
            analyzer.analyzeIndexEdge(cell, reference.asCell(), i);
    }
}

static CallData nativeCallData(TaggedNativeFunction function)
{
    CallData callData;
    callData.type = CallData::Type::Native;
    callData.native.function = function;
    callData.native.isBoundFunction = false;
    callData.native.isWasm = false;
    return callData;
}

static CallData constructDataOf(JSCell* cell, uint32_t closureNumber)
{
    ImageOfClosureNumberScope scope(closureNumber);
    FunctionKindData kind = kindOfClosureNumber(closureNumber);
    if (kind.isBound())
        return boundFunctionData(closureFunctionWithNumber(closureNumber)).constructs ? nativeCallData(constructBoundClosure) : CallData();
    if (!FunctionTables::canConstruct(kind))
        return CallData();
    return CallData::forCompiledCode(constructEntrypoint().taggedPtr(), cell->structure()->realm());
}

Structure* Closure::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype, bool isBound)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CompiledClosureType, isBound ? StructureFlags & ~ImplementsDefaultHasInstance : StructureFlags), info());
}

Closure* Closure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    RELEASE_ASSERT(numberOfReferences <= captures.size() && captures.size() <= maxNumberOfCaptures);
    FunctionTables& tables = FunctionTables::of(vm);
    const ProgramTables& programTables = AOT::programTables();
    ClosureFunctionRangeData closureFunctions = programTables.closureFunctions;
    RELEASE_ASSERT(closureFunctions.first <= function.index() && function.index() < closureFunctions.end);
    uint32_t closureNumber = programTables.closureNumberOf(function.index());
    uint64_t word = tables.words[closureNumber];
    CaptureCounts counts = programTables.captureCountsOfClosureNumber(closureNumber);
    RELEASE_ASSERT(word && counts.references == numberOfReferences && counts.captures == captures.size());
    Closure* closure = new (NotNull, allocateCell<Closure>(vm, allocationSize(captures.size()))) Closure(vm, structure, tables.executableOf(function), word);
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
    if (closure->m_entry)
        analyzeCaptures(cell, closure->captures(), closure->numberOfReferences(), analyzer);
}

JSC_DEFINE_HOST_FUNCTION(callClassConstructor, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    ImageOfCellScope imageOfCallee(globalObject->vm(), callFrame->jsCallee());
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* classObject = uncheckedDowncast<Closure>(callFrame->jsCallee());
    String name = classObject->kind().hasInferredName ? String() : writtenFunctionName(classObject->function());
    ConstructorKind kind = FunctionDescription(classObject->function()).isNakedConstructor() ? ConstructorKind::Naked : ConstructorKind::Base;
    SetForScope shownFrame(vm.aotState().classCalledWithoutNew, callFrame);
    return JSValue::encode(throwException(globalObject, scope, createConstructorCalledWithoutNewError(globalObject, kind, name)));
}

CallData Closure::getCallData(JSCell* cell)
{
    // The word of the closure says it as well as the record of its function, and lies in the cell.
    ASSERT(uncheckedDowncast<Closure>(cell)->kind().isConstructor == !!(uncheckedDowncast<Closure>(cell)->entry() >> ClosureWord::isNotCallableBit));
    if (!(uncheckedDowncast<Closure>(cell)->entry() >> ClosureWord::isNotCallableBit)) {
        ImageOfClosureNumberScope scope(uncheckedDowncast<Closure>(cell)->closureNumber());
        return CallData::forCompiledCode(closureEntrypoint().taggedPtr(), cell->structure()->realm());
    }
    return nativeCallData(callClassConstructor);
}

CallData Closure::getConstructData(JSCell* cell)
{
    return constructDataOf(cell, uncheckedDowncast<Closure>(cell)->closureNumber());
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
    if (!classLayout || !closure->getDirect(vm, vm.propertyNames->prototype))
        return true;
    // A static field that a class above declares is a property of that class object alone.
    const FieldData* field = closure->state().objects->classes.staticField(*classLayout, uid);
    std::span<const FieldData> declaredHere = classLayout->staticFields();
    bool isDeclaredHere = field && std::bit_cast<uintptr_t>(field) - std::bit_cast<uintptr_t>(declaredHere.data()) < declaredHere.size_bytes();
    return isDeclaredHere && !isValidOffset(closure->structure()->get(vm, uid));
}

static JSC_DECLARE_CUSTOM_GETTER(closurePrototypeProperty);

JSC_DEFINE_CUSTOM_GETTER(closurePrototypeProperty, (JSGlobalObject* globalObject, EncodedJSValue encodedClosure, PropertyName name))
{
    ImageOfCellScope imageOfClosure(globalObject->vm(), JSValue::decode(encodedClosure).asCell());
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
    ImageOfCellScope imageOfCell(this);
    return kind().isConstructor ? state().objects->classes.classConstructedByClosureWithNumber(closureNumber()) : nullptr;
}

bool Closure::isLazyClassObject() const
{
    ImageOfCellScope imageOfCell(this);
    if (!kind().isConstructor || getDirect(vm(), vm().propertyNames->prototype))
        return false;
    const LayoutData* classLayout = classData();
    if (!classLayout)
        return false;
    return classLayout->createsClassObjectOnMutation && (classLayout->createsPrototypeBeforeClassObject || state().objects->classes.classPrototypeIfExists(*classLayout));
}

bool Closure::hasStructureOfNewClosure(VM& vm) const
{
    ImageOfCellScope imageOfCell(this);
    unsigned index = kind().closureStructure;
    return index != classObjectClosureStructure && structure() == state().objects->closures.closureStructure(vm, index);
}

static NEVER_INLINE void createClassObject(Closure* classObject)
{
    State& state = classObject->state();
    state.objects->classes.createClassPart(state.instance.globalObject, classObject, ClassTable::ClassPart::ClassObject);
}

void Closure::adoptBoundFunctionStructure(VM& vm)
{
    ImageOfCellScope imageOfCell(this);
    ClosureTable& closures = state().objects->closures;
    Structure* unbound = closures.closureStructureFor(vm, function());
    if (structure() != unbound)
        return;
    setStructure(vm, closures.boundClosureStructure(vm, function()));
}

void Closure::ensureClassObject(VM&)
{
    ImageOfCellScope imageOfCell(this);
    if (isLazyClassObject())
        createClassObject(this);
}

// A cache answers for a static field by the Structure of the class object alone, so a class with static members shares its Structure with no other class.
static Structure* structureSharedWithClassesInheritingFrom(VM& vm, const Closure& classObject, JSObject* parent)
{
    const LayoutData& classLayout = *classObject.classData();
    if (!classLayout.staticMethods().empty() || !classLayout.staticFields().empty())
        return nullptr;
    // The table keeps each parent alive, so it is asked for the parents of which a program has a fixed number.
    State& state = classObject.state();
    if (parent != state.instance.globalObject->functionPrototype() && classLayout.evaluation() != EvaluationState::ProvenOnce)
        return nullptr;
    return state.objects->classes.classObjectStructureInheritingFrom(vm, parent);
}

void Closure::initializeClassObject(VM& vm, JSObject* parent, JSObject* prototype)
{
    ImageOfCellScope imageOfCell(this);
    ASSERT(kind().isConstructor && classData());
    Structure* shared = structureSharedWithClassesInheritingFrom(vm, *this, parent);
    if (!shared) {
        Structure* own = createStructure(vm, structure()->realm(), parent);
        if (butterfly())
            nukeStructureAndSetButterfly(vm, structureID(), nullptr);
        setStructure(vm, own);
        putDirectWithoutTransition(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete | PropertyAttribute::ReadOnly);
        return;
    }

    ASSERT(shared->transitionPropertyName() == vm.propertyNames->prototype.impl());
    bool wasPrototype = mayBePrototype();
    nukeStructureAndSetButterfly(vm, structureID(), Butterfly::create(vm, this, shared));
    setStructure(vm, shared);
    putDirectOffset(vm, shared->transitionOffset(), prototype);
    if (wasPrototype)
        didBecomePrototype(vm);
}

void Closure::prepareForGenericAccess(VM& vm)
{
    ImageOfCellScope imageOfCell(this);
    if (kind().isBound()) [[unlikely]]
        adoptBoundFunctionStructure(vm);
    else
        ensureClassObject(vm);
}

JSValue Closure::prototypeProperty(VM& vm)
{
    ImageOfCellScope imageOfCell(this);
    if (JSValue stored = getDirect(vm, vm.propertyNames->prototype))
        return stored;
    if (isLazyClassObject())
        return state().objects->classes.classPrototype(vm, *classData());
    ensurePrototypeProperty(vm);
    return getDirect(vm, vm.propertyNames->prototype);
}

// The object is marked as a prototype from the start, so the names that the program adds before the first construction are not followed by a transition for the mark.
static JSObject* createPrototypeObjectOfFunction(VM& vm, JSGlobalObject* globalObject, JSObject* function)
{
    PropertyOffset offset;
    Structure* structure = Structure::becomePrototypeTransition(vm, globalObject->objectStructureForObjectConstructor());
    structure = Structure::addPropertyTransition(vm, structure, vm.propertyNames->constructor, static_cast<unsigned>(PropertyAttribute::DontEnum), offset);
    JSObject* prototype = constructEmptyObject(vm, structure);
    prototype->putDirectOffset(vm, offset, function);
    return prototype;
}

bool Closure::ensurePrototypeProperty(VM& vm)
{
    ImageOfCellScope imageOfCell(this);
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
    else
        prototype = createPrototypeObjectOfFunction(vm, globalObject, this);
    putDirect(vm, vm.propertyNames->prototype, prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
    state().objects->classes.didCreatePrototypePropertyOf(this);
    state().objects->closures.didGivePrototypePropertyTo(vm, this);
    return true;
}

void Closure::setPrototypeProperty(VM& vm, JSObject* prototype, unsigned attributes)
{
    ImageOfCellScope imageOfCell(this);
    RELEASE_ASSERT(!kind().isConstructor);
    validateIsNotInFrame(vm, this);
    prepareForGenericAccess(vm);
    if (FunctionRareData* rareData = this->rareData())
        rareData->clear("Store to prototype property of a function");
    putDirect(vm, vm.propertyNames->prototype, prototype, attributes);
}

void Closure::setNameProperty(VM& vm, JSString* name, unsigned attributes)
{
    ImageOfCellScope imageOfCell(this);
    validateIsNotInFrame(vm, this);
    prepareForGenericAccess(vm);
    putDirect(vm, vm.propertyNames->name, name, attributes);
    FunctionRareData* rareData = ensureRareData(vm);
    rareData->setHasReifiedName();
    rareData->setHasModifiedNameForBoundOrNonHostFunction();
}

static bool canDefineStaticMemberWithoutTransition(VM& vm, Closure& classObject, PropertyName key)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    if (key == names.prototype || key == names.name || key == names.length || key == names.displayName)
        return false;
    if (classObject.isLazyClassObject() || StaticFields::findOwnField(classObject, key.uid()))
        return false;
    StaticFields::ensureProperties(vm, classObject);
    return true;
}

bool tryDefineClassMemberWithoutTransition(JSGlobalObject* globalObject, Closure& classObject, const LayoutData& classLayout, JSObject* holder, PropertyName key, JSObject* function, MethodKind kind)
{
    VM& vm = globalObject->vm();
    validateIsNotInFrame(vm, &classObject);
    if (parseIndex(key))
        return false;
    if (classObject.state().objects->classes.keepsStructuresForNextEvaluation(classObject, classLayout))
        return false;
    bool isStatic = holder == &classObject;
    if (isStatic ? !canDefineStaticMemberWithoutTransition(vm, classObject, key) : holder->type() != FinalObjectType)
        return false;

    Structure* structure = holder->structure();
    bool isOwnStructure = structure->transitionKind() == TransitionKind::Unknown || structure->transitionKind() == TransitionKind::MakeListedPropertiesImmutable;
    if (!isOwnStructure || !structure->isPinnedPropertyTable() || structure->isDictionary())
        return false;
    if (!structure->isStructureExtensible() || structure->isImmutableProperty(key.uid()))
        return false;

    // Between NewClass and the end of the definition no code of the program can name the class, so no object inherits from its holders and no cache has walked past them.
    bool isMarkedAsPrototype = structure->mayBePrototype();
    if (isMarkedAsPrototype)
        structure->setMayBePrototype(false);
    auto restoreMark = makeScopeExit([&] {
        if (isMarkedAsPrototype)
            structure->setMayBePrototype(true);
    });
    constexpr unsigned accessorAttributes = PropertyAttribute::Accessor | PropertyAttribute::DontEnum;
    unsigned attributes = 0;
    PropertyOffset offset = structure->get(vm, key, attributes);
    if (kind == MethodKind::Method) {
        if (isValidOffset(offset))
            return false;
        holder->putDirectWithoutTransition(vm, key, function, static_cast<unsigned>(PropertyAttribute::DontEnum));
        return true;
    }

    JSObject* getter = kind == MethodKind::Getter ? function : nullptr;
    JSObject* setter = kind == MethodKind::Setter ? function : nullptr;
    if (!isValidOffset(offset)) {
        holder->putDirectNonIndexAccessorWithoutTransition(vm, key, GetterSetter::create(vm, globalObject, getter, setter), accessorAttributes);
        return true;
    }
    if (attributes != accessorAttributes)
        return false;
    auto* current = uncheckedDowncast<GetterSetter>(holder->getDirect(offset));
    if (!getter && !current->isGetterNull())
        getter = current->getter();
    if (!setter && !current->isSetterNull())
        setter = current->setter();
    structure->didReplaceProperty(offset);
    holder->putDirectOffset(vm, offset, GetterSetter::create(vm, globalObject, getter, setter));
    return true;
}

static JSObject* boundFunctionTarget(VM& vm, State& state, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const BoundFunctionData& bound = boundFunctionData(function);
    std::optional<unsigned> slot = bound.targetClosureSlot();
    if (!slot)
        return asObject(state.objects->closures.closure(vm, FunctionID(bound.target)));
    RELEASE_ASSERT(*slot < numberOfReferences && numberOfReferences <= captures.size());
    return std::bit_cast<JSObject*>(captures[*slot]);
}

JSC_DEFINE_HOST_FUNCTION(constructBoundClosure, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    ImageOfCellScope imageOfCallee(globalObject->vm(), callFrame->jsCallee());
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSObject* callee = callFrame->jsCallee();
    FunctionID function;
    std::span<const uint64_t> captures;
    unsigned numberOfReferences = 0;
    State* state = nullptr;
    auto take = [&](auto* closure) {
        function = closure->function();
        captures = closure->captures();
        numberOfReferences = closure->numberOfReferences();
        state = &closure->state();
    };
    if (auto* slim = dynamicDowncast<SlimClosure>(callee))
        take(slim);
    else if (auto* reified = dynamicDowncast<ReifiedSlimClosure>(callee))
        take(reified);
    else
        take(uncheckedDowncast<Closure>(callee));

    const BoundFunctionData& bound = boundFunctionData(function);
    RELEASE_ASSERT(bound.constructs && bound.firstArgumentSlot + bound.numberOfCapturedArguments <= numberOfReferences);
    JSObject* target = boundFunctionTarget(vm, *state, function, captures, numberOfReferences);
    MarkedArgumentBuffer arguments;
    for (unsigned index = 0; index < bound.numberOfCapturedArguments; ++index) {
        // An argument whose type lists its strings lies there as the number of the string.
        uint64_t word = captures[bound.firstArgumentSlot + index];
        arguments.append(isLiteralStringWord(word) ? JSValue(state->strings->stringForLiteralStringWord(word)) : JSValue::decode(word));
    }
    if (bound.numberOfCapturedArguments == bound.numberOfBoundArguments) {
        for (unsigned index = 0; index < callFrame->argumentCount(); ++index)
            arguments.append(callFrame->uncheckedArgument(index));
    }
    if (arguments.hasOverflowed()) [[unlikely]] {
        throwOutOfMemoryError(globalObject, scope);
        return encodedJSValue();
    }
    JSValue newTarget = callFrame->newTarget();
    if (newTarget == JSValue(callee))
        newTarget = target;
    RELEASE_AND_RETURN(scope, JSValue::encode(constructAllowingPrimitiveResult(globalObject, target, JSC::getConstructData(target), arguments, newTarget)));
}

bool Closure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    Closure* closure = uncheckedDowncast<Closure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

void Closure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    // The base class clears whatever its lookups throw.
    DeferTerminationForAWhile deferTermination(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    didLookForNameProperty(vm, closure);
    closure->prepareForGenericAccess(vm);
    if (closure->kind().isConstructor)
        StaticFields::ensureProperties(vm, *closure);
    Base::getOwnSpecialPropertyNames(object, globalObject, names, mode);
    if (mode == DontEnumPropertiesMode::Include && FunctionTables::hasPrototypePropertyAtCreation(closure->kind()) && closure->ensurePrototypeProperty(vm))
        names.add(vm.propertyNames->prototype);
}

static void didChangePrototypeProperty(Closure* closure)
{
    VM& vm = closure->vm();
    if (FunctionRareData* rareData = closure->rareData())
        rareData->clear("Store to prototype property of a function");
    ClassTable& classes = closure->state().objects->classes;
    if (const LayoutData* constructed = classes.classConstructedBy(closure->function())) {
        classes.didChangeMember(*constructed, ClassSide::Static, vm.propertyNames->prototype.impl());
        // The class of a constructor function is defined with the prototype object that the function has then, and its guards speak of that object.
        JSObject* prototypeOfClass = ClassTable::isDefinedByClass(*constructed) ? nullptr : classes.classPrototypeIfExists(*constructed);
        if (prototypeOfClass && closure->getDirect(vm, vm.propertyNames->prototype) != JSValue(prototypeOfClass))
            classes.disarmMemberGuards(*constructed);
    }
    // A construction whose result nobody uses is not compiled, so the object counts as a prototype from here on, as the one that the function is created with does.
    if (JSValue prototype = closure->getDirect(vm, vm.propertyNames->prototype); prototype && prototype.isObject())
        asObject(prototype)->didBecomePrototype(vm);
}

static bool isPlainDataDescriptor(const PropertyDescriptor& descriptor)
{
    if (descriptor.isAccessorDescriptor())
        return false;
    return (!descriptor.writablePresent() || descriptor.writable())
        && (!descriptor.enumerablePresent() || descriptor.enumerable())
        && (!descriptor.configurablePresent() || descriptor.configurable());
}

static bool createsPlainDataProperty(const PropertyDescriptor& descriptor)
{
    return isPlainDataDescriptor(descriptor) && descriptor.writablePresent() && descriptor.enumerablePresent() && descriptor.configurablePresent();
}

static bool restatesInitialName(VM& vm, const Closure& closure, PropertyName name, const PropertyDescriptor& descriptor)
{
    if (name != vm.propertyNames->name || closure.classData() || closure.kind().hasInitialNamePrefix() || closure.kind().notesLooksForName())
        return false;
    FunctionRareData* rareData = closure.rareData();
    if ((rareData && rareData->hasReifiedName()) || closure.getDirect(vm, vm.propertyNames->name))
        return false;
    if (descriptor.isAccessorDescriptor() || !descriptor.value() || !descriptor.value().isString())
        return false;
    if ((descriptor.writablePresent() && descriptor.writable()) || (descriptor.enumerablePresent() && descriptor.enumerable()) || (descriptor.configurablePresent() && !descriptor.configurable()))
        return false;
    auto defined = asString(descriptor.value())->tryGetValue();
    return !defined->isNull() && defined.data == initialNameProperty(closure.function());
}

bool Closure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName propertyName, PropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    if (propertyName == vm.propertyNames->name) [[unlikely]]
        didLookForNameProperty(vm, closure);
    const LayoutData* classLayout = closure->classData();
    if (const FieldData* field = classLayout ? StaticFields::findOwnField(*closure, propertyName.uid()) : nullptr) {
        if (JSValue value = closure->state().objects->classes.readStaticField(*field)) {
            slot.setValue(closure, static_cast<unsigned>(PropertyAttribute::None), value);
            return true;
        }
    }
    if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
        return true;
    if (classLayout && closure->isLazyClassObject()) {
        if (propertyName != vm.propertyNames->prototype && propertyName != vm.propertyNames->name && propertyName != vm.propertyNames->length && !closure->state().objects->classes.hasStaticMethodNamed(*classLayout, propertyName.uid()))
            return false;
        closure->ensureClassObject(vm);
        if (JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot))
            return true;
    }
    if (propertyName == vm.propertyNames->prototype)
        return closure->ensurePrototypeProperty(vm) && JSObject::getOwnPropertySlot(object, globalObject, propertyName, slot);
    bool isLength = propertyName == vm.propertyNames->length;
    bool isName = propertyName == vm.propertyNames->name;
    if ((propertyName == vm.propertyNames->hasInstanceSymbol || isLength || isName) && closure->kind().isBound()) [[unlikely]]
        closure->adoptBoundFunctionStructure(vm);
    else if ((isLength || (isName && !closure->kind().hasInitialNamePrefix())) && closure->hasStructureOfNewClosure(vm)) {
        if (isName && closure->kind().notesLooksForNameInCell())
            closure->noteLookForName();
        JSValue value = isLength ? jsNumber(functionLengthOf(closure->function())) : JSValue(initialNamePropertyString(vm, closure->state(), closure->function()));
        slot.setValue(closure, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, value);
        return true;
    }
    return Base::getOwnPropertySlot(object, globalObject, propertyName, slot);
}

bool Closure::isNameThatBuiltinFunctionsRefuse(VM& vm, UniquedStringImpl* uid)
{
    if (uid->isSymbol())
        return uid == vm.propertyNames->hasInstanceSymbol.impl();
    return uid == vm.propertyNames->bind.impl() || equal(uid, "call"_s) || equal(uid, "apply"_s);
}

bool Closure::refusesOwnProperty(VM& vm, PropertyName name) const
{
    ImageOfCellScope imageOfCell(this);
    return !name.isPrivateName() && isNameThatBuiltinFunctionsRefuse(vm, name.uid()) && isBuiltinFunction();
}

bool Closure::isBuiltinFunction() const
{
    ImageOfCellScope imageOfCell(this);
    return ModuleTable(state().instance.tables()).isRuntimeFunction(function());
}

bool Closure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    validateIsNotInFrame(vm, closure);
    if (slot.thisValue() == JSValue(closure) && closure->refusesOwnProperty(vm, name)) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        slot.disableCaching();
        // Function.prototype[Symbol.hasInstance] is read-only, so the language itself refuses that store, and says nothing of it in sloppy code.
        bool isTold = slot.isStrictMode() || (slot.context() != PutPropertySlot::ReflectSet && name != vm.propertyNames->hasInstanceSymbol);
        return typeError(globalObject, scope, isTold, assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid()));
    }
    ClassTable& classes = closure->state().objects->classes;
    const LayoutData* classLayout = closure->classData();
    bool isStoreToClassObject = classLayout && slot.thisValue() == JSValue(closure);
    const FieldData* field = isStoreToClassObject ? StaticFields::findOwnField(*closure, name.uid()) : nullptr;
    if (field && StaticFields::isInVariable(*closure, *field)) {
        if (classes.tryStoreStaticField(vm, *field, value))
            return true;
        StaticFields::moveToProperty(vm, *closure, *field);
    }
    closure->prepareForGenericAccess(vm);
    if (classLayout)
        StaticFields::ensureProperties(vm, *closure);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (isStoreToClassObject) {
        // No guard speaks of the value of a property that the class does not declare, so the next store to it may go past this function.
        bool replacesUndeclaredProperty = slot.type() == PutPropertySlot::ExistingProperty && !field && !classes.hasStaticMethodNamed(*classLayout, name.uid());
        if (!replacesUndeclaredProperty)
            slot.disableCaching();
        if (slot.type() != PutPropertySlot::SetterProperty)
            classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    }
    if (name == vm.propertyNames->prototype) [[unlikely]] {
        slot.disableCaching();
        didChangePrototypeProperty(closure);
    }
    if (name == vm.propertyNames->displayName) [[unlikely]] {
        slot.disableCaching();
        didChangeDisplayName(vm, closure);
    }
    return result;
}

bool Closure::hasOrdinaryDefineOwnProperty(VM& vm, PropertyName name) const
{
    ImageOfCellScope imageOfCell(this);
    return !classData() && !kind().isBound() && !refusesOwnProperty(vm, name) && ReifiedSlimClosure::hasOrdinaryDefineOwnProperty(vm, name);
}

bool Closure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    if (closure->refusesOwnProperty(vm, name)) [[unlikely]]
        return typeError(globalObject, scope, shouldThrow, assignmentToNameThatBuiltinFunctionInheritsMessage(name.uid()));
    if (restatesInitialName(vm, *closure, name, descriptor)) {
        didLookForNameProperty(vm, closure);
        return true;
    }
    ClassTable& classes = closure->state().objects->classes;
    const LayoutData* classLayout = closure->classData();
    const FieldData* field = classLayout ? StaticFields::findOwnField(*closure, name.uid()) : nullptr;
    if (field && StaticFields::isInVariable(*closure, *field)) {
        if (isPlainDataDescriptor(descriptor) && (!descriptor.value() || classes.tryStoreStaticField(vm, *field, descriptor.value())))
            return true;
        StaticFields::moveToProperty(vm, *closure, *field);
    } else if (field && createsPlainDataProperty(descriptor) && StaticFields::canEnterVariable(vm, *closure, *field)) {
        // Compiled code reads the variable of a field whose definition it could not prove to fit, so the definition by name fills it too.
        if (classes.tryStoreStaticField(vm, *field, descriptor.value() ? descriptor.value() : jsUndefined()))
            return true;
    }
    closure->prepareForGenericAccess(vm);
    if (classLayout)
        StaticFields::ensureProperties(vm, *closure);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (classLayout)
        classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    if (name == vm.propertyNames->prototype) [[unlikely]]
        didChangePrototypeProperty(closure);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didChangeDisplayName(vm, closure);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString()) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        didDefineNameProperty(vm, closure, reported);
    }
    return result;
}

bool Closure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    validateIsNotInFrame(vm, closure);
    const LayoutData* classLayout = closure->classData();
    const FieldData* field = classLayout ? StaticFields::findOwnField(*closure, name.uid()) : nullptr;
    bool wasInVariable = field && StaticFields::isInVariable(*closure, *field);
    closure->prepareForGenericAccess(vm);
    if (classLayout)
        StaticFields::ensureProperties(vm, *closure);
    if (name == vm.propertyNames->prototype)
        closure->ensurePrototypeProperty(vm);
    bool result = Base::deleteProperty(cell, globalObject, name, slot);
    if (result && classLayout) {
        if (wasInVariable)
            StaticFields::didDeleteProperty(vm, *closure, *field);
        else
            closure->state().objects->classes.didChangeMember(*classLayout, ClassSide::Static, name.uid());
    }
    if (name == vm.propertyNames->prototype) [[unlikely]]
        didChangePrototypeProperty(closure);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didChangeDisplayName(vm, closure);
    if (result && name == vm.propertyNames->name) [[unlikely]]
        didDeleteNameProperty(vm, closure);
    return result;
}

bool Closure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(cell);
    validateIsNotInFrame(vm, closure);
    closure->prepareForGenericAccess(vm);
    return Base::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool Closure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    closure->prepareForGenericAccess(vm);
    if (const LayoutData* classLayout = closure->classData()) {
        StaticFields::ensureProperties(vm, *closure);
        closure->state().objects->classes.didChangeEveryMember(*classLayout, ClassSide::Static);
    }
    return Base::preventExtensions(object, globalObject);
}

bool Closure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    Closure* closure = uncheckedDowncast<Closure>(object);
    validateIsNotInFrame(vm, closure);
    // A name that the function refuses as its own could be planted one step up.
    if (object->getPrototypeDirect() != prototype && closure->isBuiltinFunction()) [[unlikely]] {
        auto scope = DECLARE_THROW_SCOPE(vm);
        return typeError(globalObject, scope, shouldThrow, prototypeChangeOfLockedBuiltinMessage("a built-in function"_s));
    }
    closure->prepareForGenericAccess(vm);
    if (const LayoutData* classLayout = closure->classData(); classLayout && object->getPrototypeDirect() != prototype)
        closure->state().objects->classes.didChangePrototypeLink(*classLayout, ClassSide::Static);
    return Base::setPrototype(object, globalObject, prototype, shouldThrow);
}

void PropertyWalk::willPutDirectOnClosure(VM& vm, JSObject* object, UniquedStringImpl* uid)
{
    StaticFields::willPutDirect(vm, *uncheckedDowncast<Closure>(object), uid);
}

// JSObject::putDirect() asks no method table: Error.captureStackTrace(SomeClass) puts `stack` on the class object so. A holder is entered once its class is defined.
void PropertyWalk::didPutDirectOnClosure(VM&, JSObject* object, UniquedStringImpl* uid)
{
    Closure* closure = uncheckedDowncast<Closure>(object);
    const LayoutData* classLayout = closure->classData();
    if (!classLayout)
        return;
    ClassTable& classes = closure->state().objects->classes;
    ClassSide side;
    if (classes.classWithGuardsHeldBy(closure, side) == classLayout && side == ClassSide::Static)
        classes.didChangeMember(*classLayout, ClassSide::Static, uid);
}

Structure* SlimClosure::createStructure(VM& vm, JSGlobalObject* realm, JSValue prototype)
{
    return Structure::create(vm, realm, prototype, TypeInfo(SlimClosureType, StructureFlags), info(), NonArray, 0);
}

Structure* ReifiedSlimClosure::createStructure(VM& vm, JSGlobalObject* realm, JSValue prototype, Variant variant, NameProperty nameProperty)
{
    unsigned flags = variant == Variant::Bound ? StructureFlags & ~ImplementsDefaultHasInstance : StructureFlags;
    if (nameProperty == NameProperty::TakenAtFirstLook)
        flags |= flagsThatSeeLooksForName;
    return Structure::create(vm, realm, prototype, TypeInfo(SlimClosureWithPropertiesType, flags), info(), NonArray, 0);
}

SlimClosure* SlimClosure::create(VM& vm, Structure* structure, FunctionID function, std::span<const uint64_t> captures, unsigned numberOfReferences)
{
    const ProgramTables& programTables = AOT::programTables();
    uint32_t closureNumber = programTables.closureNumberOf(function.index());
    uint64_t word = FunctionTables::of(vm).words[closureNumber];
    CaptureCounts counts = programTables.captureCountsOfClosureNumber(closureNumber);
    RELEASE_ASSERT(word && programTables.kindOfClosureNumber(closureNumber).isSlim && counts.references == numberOfReferences && counts.captures == captures.size());
    SlimClosure* closure = new (NotNull, allocateCell<SlimClosure>(vm, allocationSize(captures.size()))) SlimClosure(vm, structure, word);
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

static ALWAYS_INLINE State& stateOfClosure(const JSCell* closure, uint32_t closureNumber)
{
    Instance* first = closure->structure()->realm()->aotInstance();
    if (!first->above) [[likely]]
        return *first->state;
    return *first->instanceWithClosureNumber(closureNumber).state;
}

State& Closure::state() const
{
    return stateOfClosure(this, closureNumber());
}

State& SlimClosure::state() const
{
    return stateOfClosure(this, closureNumber());
}

State& ReifiedSlimClosure::state() const
{
    return stateOfClosure(this, closureNumber());
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
    if (closure->m_word)
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
    ImageOfClosureNumberScope scope(uncheckedDowncast<SlimClosure>(cell)->closureNumber());
    return CallData::forCompiledCode(slimClosureEntrypoint().taggedPtr(), cell->structure()->realm());
}

CallData ReifiedSlimClosure::getCallData(JSCell* cell)
{
    ImageOfClosureNumberScope scope(uncheckedDowncast<ReifiedSlimClosure>(cell)->closureNumber());
    return CallData::forCompiledCode(slimClosureEntrypoint().taggedPtr(), cell->structure()->realm());
}

CallData SlimClosure::getConstructData(JSCell* cell)
{
    return constructDataOf(cell, uncheckedDowncast<SlimClosure>(cell)->closureNumber());
}

CallData ReifiedSlimClosure::getConstructData(JSCell* cell)
{
    return constructDataOf(cell, uncheckedDowncast<ReifiedSlimClosure>(cell)->closureNumber());
}

Structure* SlimClosure::fillInitialProperties(VM& vm, std::span<JSValue, numberOfInitialProperties> values)
{
    ImageOfCellScope imageOfCell(this);
    validateIsNotInFrame(vm, this);
    State& state = this->state();
    FunctionID function = this->function();
    Structure* structure = state.objects->closures.reifiedSlimClosureStructure(vm, function);
    values[0] = jsNumber(closureNumberOf(function));
    values[1] = jsNumber(functionLengthOf(function));
    values[2] = kind().notesLooksForName() ? JSValue(state.objects->closures.untakenNameProperty()) : JSValue(initialNamePropertyString(vm, state, function));
    values[3] = FunctionTables::hasPrototypePropertyAtCreation(kind()) ? JSValue(state.objects->closures.uncreatedPrototypeProperty()) : JSValue();
    return structure;
}

ReifiedSlimClosure* SlimClosure::becomeReified(VM& vm, Structure* structure, std::span<const JSValue> values)
{
    ImageOfCellScope imageOfCell(this);
    static_assert(offsetOfWord() == butterflyOffset());
    RELEASE_ASSERT(structure->outOfLineCapacity() == values.size() && getPrototypeDirect() == structure->storedPrototype());
    bool takesName = nameWasLookedFor();
    // ReifiedSlimClosure::markRenamed().
    setPerCellBit(false);
    Butterfly* butterfly = Butterfly::createUninitialized(vm, this, 0, values.size(), false, 0);
    for (unsigned i = 0; i < values.size(); ++i)
        butterfly->propertyStorage()[offsetInOutOfLineStorage(firstOutOfLineOffset + i)].setWithoutWriteBarrier(values[i]);
    // JSObject's own setters assert on butterfly(), which is null until the cell's type changes.
    setStructureIDDirectly(structureID().nuke());
    WTF::storeStoreFence();
    m_word = std::bit_cast<uint64_t>(butterfly);
    WTF::storeStoreFence();
    JSCell::setStructure(vm, structure);
    vm.writeBarrier(this);
    auto* reified = uncheckedDowncast<ReifiedSlimClosure>(static_cast<JSObject*>(this));
    if (takesName)
        reified->lookForNameProperty(vm);
    return reified;
}

ReifiedSlimClosure* SlimClosure::reify(VM& vm, PrototypeObject prototypeObject)
{
    ImageOfCellScope imageOfCell(this);
    JSValue values[numberOfInitialProperties];
    static_assert(numberOfInitialProperties == initialOutOfLineCapacity);
    Structure* withProperties = fillInitialProperties(vm, values);
    if (values[3] && prototypeObject == PrototypeObject::MadeNow) {
        withProperties = Structure::attributeChangeTransition(vm, withProperties, vm.propertyNames->prototype, PropertyAttribute::DontEnum | PropertyAttribute::DontDelete);
        values[3] = createPrototypeObjectOfFunction(vm, state().instance.globalObject, this);
    }
    return becomeReified(vm, withProperties, values);
}

ReifiedSlimClosure* SlimClosure::reifyWithAddedProperty(VM& vm, Structure* structureWithProperty, PropertyOffset offset, JSValue value)
{
    ImageOfCellScope imageOfCell(this);
    JSValue values[2 * numberOfInitialProperties];
    Structure* withoutProperty = fillInitialProperties(vm, std::span(values).first<numberOfInitialProperties>());
    unsigned index = offset - firstOutOfLineOffset;
    unsigned capacity = structureWithProperty->outOfLineCapacity();
    RELEASE_ASSERT(structureWithProperty->isTransitionFrom(withoutProperty) && capacity <= std::size(values) && index < capacity && !values[index]);
    values[index] = value;
    return becomeReified(vm, structureWithProperty, std::span<const JSValue>(values).first(capacity));
}

JSC_DEFINE_CUSTOM_GETTER(createPrototypeProperty, (JSGlobalObject*, EncodedJSValue encodedClosure, PropertyName))
{
    ImageOfCellScope imageOfClosure(JSValue::decode(encodedClosure).asCell());
    JSObject* closure = asObject(JSValue::decode(encodedClosure));
    VM& vm = closure->vm();
    JSObject* prototype = createPrototypeObjectOfFunction(vm, closure->structure()->realm(), closure);
    // The closure may have been frozen since.
    unsigned attributes = 0;
    closure->getDirect(vm, vm.propertyNames->prototype, attributes);
    closure->putDirect(vm, vm.propertyNames->prototype, prototype, attributes & ~PropertyAttribute::CustomValue);
    return JSValue::encode(prototype);
}

JSValue ReifiedSlimClosure::lookForNameProperty(VM& vm)
{
    ImageOfCellScope imageOfCell(this);
    didLookForNameProperty(vm, this);
    unsigned attributes = 0;
    JSValue name = getDirect(vm, vm.propertyNames->name, attributes);
    if (!name || !(attributes & PropertyAttribute::CustomValue))
        return name;
    name = initialNamePropertyString(vm, state(), function());
    putDirect(vm, vm.propertyNames->name, name, attributes & ~PropertyAttribute::CustomValue);
    return name;
}

JSC_DEFINE_CUSTOM_GETTER(takeNameProperty, (JSGlobalObject*, EncodedJSValue encodedClosure, PropertyName))
{
    ImageOfCellScope imageOfClosure(JSValue::decode(encodedClosure).asCell());
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(asObject(JSValue::decode(encodedClosure)));
    return JSValue::encode(closure->lookForNameProperty(closure->vm()));
}

bool SlimClosure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool ReifiedSlimClosure::customHasInstance(JSObject* object, JSGlobalObject* globalObject, JSValue value)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(object);
    return boundFunctionTarget(globalObject->vm(), closure->state(), closure->function(), closure->captures(), closure->numberOfReferences())->hasInstance(globalObject, value);
}

bool SlimClosure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    SlimClosure* closure = uncheckedDowncast<SlimClosure>(object);
    if (name == vm.propertyNames->length) {
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, jsNumber(functionLengthOf(closure->function())));
        return true;
    }
    if (name == vm.propertyNames->name) {
        if (closure->kind().notesLooksForNameInCell())
            closure->noteLookForName();
        else if (closure->kind().notesLooksForName()) {
            closure->reify(vm);
            return ReifiedSlimClosure::getOwnPropertySlot(object, globalObject, name, slot);
        } else
            didLookForNameProperty(vm, closure);
        slot.setValue(object, PropertyAttribute::ReadOnly | PropertyAttribute::DontEnum, initialNamePropertyString(vm, closure->state(), closure->function()));
        return true;
    }
    bool isPrototype = name == vm.propertyNames->prototype && FunctionTables::hasPrototypePropertyAtCreation(closure->kind());
    bool isBoundHasInstance = name == vm.propertyNames->hasInstanceSymbol && closure->isBound();
    if (!isPrototype && !isBoundHasInstance)
        return false;
    closure->reify(vm, isPrototype ? PrototypeObject::MadeNow : PrototypeObject::MadeAtFirstRead);
    return JSObject::getOwnPropertySlot(object, globalObject, name, slot);
}

bool ReifiedSlimClosure::getOwnPropertySlot(JSObject* object, JSGlobalObject* globalObject, PropertyName name, PropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    if (name == vm.propertyNames->name) [[unlikely]]
        uncheckedDowncast<ReifiedSlimClosure>(object)->lookForNameProperty(vm);
    return Base::getOwnPropertySlot(object, globalObject, name, slot);
}

void SlimClosure::getOwnPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder& names, DontEnumPropertiesMode mode)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    JSObject::getOwnPropertyNames(object, globalObject, names, mode);
}

void ReifiedSlimClosure::getOwnSpecialPropertyNames(JSObject* object, JSGlobalObject* globalObject, PropertyNameArrayBuilder&, DontEnumPropertiesMode)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    uncheckedDowncast<ReifiedSlimClosure>(object)->lookForNameProperty(globalObject->vm());
}

bool SlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    ReifiedSlimClosure* closure = uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    bool result = ReifiedSlimClosure::put(closure, globalObject, name, value, slot);
    slot.disableCaching();
    return result;
}

bool ReifiedSlimClosure::put(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, JSValue value, PutPropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    VM& vm = globalObject->vm();
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(cell);
    if (name == vm.propertyNames->name) [[unlikely]]
        closure->lookForNameProperty(vm);
    bool result = Base::put(cell, globalObject, name, value, slot);
    if (name == vm.propertyNames->displayName) [[unlikely]] {
        slot.disableCaching();
        didChangeDisplayName(vm, closure);
    }
    if (name == vm.propertyNames->prototype) [[unlikely]]
        slot.disableCaching();
    return result;
}

bool ReifiedSlimClosure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    VM& vm = globalObject->vm();
    bool isDeleted = Base::deleteProperty(cell, globalObject, name, slot);
    if (name == vm.propertyNames->displayName) [[unlikely]] {
        slot.disableCaching();
        didChangeDisplayName(vm, asObject(cell));
    }
    if (isDeleted && name == vm.propertyNames->name) [[unlikely]] {
        slot.disableCaching();
        didDeleteNameProperty(vm, asObject(cell));
    }
    return isDeleted;
}

bool SlimClosure::putByIndex(JSCell* cell, JSGlobalObject* globalObject, unsigned index, JSValue value, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    return JSObject::putByIndex(cell, globalObject, index, value, shouldThrow);
}

bool SlimClosure::deleteProperty(JSCell* cell, JSGlobalObject* globalObject, PropertyName name, DeletePropertySlot& slot)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), cell);
    uncheckedDowncast<SlimClosure>(cell)->reify(globalObject->vm());
    bool isDeleted = JSObject::deleteProperty(cell, globalObject, name, slot);
    slot.disableCaching();
    return isDeleted;
}

bool SlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    ReifiedSlimClosure* closure = uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return ReifiedSlimClosure::defineOwnProperty(closure, globalObject, name, descriptor, shouldThrow);
}

bool ReifiedSlimClosure::defineOwnProperty(JSObject* object, JSGlobalObject* globalObject, PropertyName name, const PropertyDescriptor& descriptor, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    ReifiedSlimClosure* closure = uncheckedDowncast<ReifiedSlimClosure>(object);
    bool result = Base::defineOwnProperty(object, globalObject, name, descriptor, shouldThrow);
    RETURN_IF_EXCEPTION(scope, false);
    if (name == vm.propertyNames->displayName) [[unlikely]]
        didChangeDisplayName(vm, closure);
    if (result && name == vm.propertyNames->name && descriptor.value() && descriptor.value().isString()) [[unlikely]] {
        String reported = asString(descriptor.value())->value(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        didDefineNameProperty(vm, closure, reported);
    }
    return result;
}

bool ReifiedSlimClosure::hasOrdinaryDefineOwnProperty(VM& vm, PropertyName name)
{
    const CommonIdentifiers& names = *vm.propertyNames;
    return name != names.prototype && name != names.name && name != names.length && name != names.displayName;
}

bool SlimClosure::preventExtensions(JSObject* object, JSGlobalObject* globalObject)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return JSObject::preventExtensions(object, globalObject);
}

bool SlimClosure::setPrototype(JSObject* object, JSGlobalObject* globalObject, JSValue prototype, bool shouldThrow)
{
    ImageOfCellScope imageOfCell(globalObject->vm(), object);
    if (object->getPrototypeDirect() == prototype)
        return true;
    uncheckedDowncast<SlimClosure>(object)->reify(globalObject->vm());
    return JSObject::setPrototype(object, globalObject, prototype, shouldThrow);
}

} } // namespace JSC::AOT

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
